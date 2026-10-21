#!/bin/bash
# Installs CTRPluginFramework (libctrpf) and 3gxtool into the devkitPro container.
# 1) tries the ThePixellizerOSS pacman repository, 2) falls back to building from source.
# Every problem is reported as a GitHub annotation so it shows on the run's summary page.
set -u
note() { echo "::warning title=$1::$2"; }
fail() { echo "::error title=$1::$2"; }
tailof() { tail -c 1500 "$1" | tr '\n' ' ' ; }

DKP=${DEVKITPRO:-/opt/devkitpro}
export PATH="$DKP/tools/bin:$PATH"

# ---------- 1. pacman repository ----------
CONF=$DKP/pacman/etc/pacman.conf
[ -f "$CONF" ] || CONF=/etc/pacman.conf
cat >> "$CONF" <<'EOF'

[thepixellizeross-lib]
Server = https://thepixellizeross.gitlab.io/packages/any
SigLevel = Optional

[thepixellizeross-linux]
Server = https://thepixellizeross.gitlab.io/packages/x86_64/linux
SigLevel = Optional
EOF
PAC=$(command -v dkp-pacman || command -v pacman || true)
if [ -n "$PAC" ]; then
  for attempt in 1 2 3; do
    if "$PAC" -Sy --noconfirm libctrpf 3gxtool >pac.log 2>&1; then break; fi
    note "pacman repo (attempt $attempt)" "$(tailof pac.log)"
    sleep $((attempt * 20))
  done
else
  note "pacman" "no dkp-pacman/pacman in container"
fi

# ---------- 2. libctrpf from source ----------
if [ ! -f "$DKP/libctrpf/lib/libctrpf.a" ]; then
  ok=0
  for url in https://codeberg.org/Ivylag/CTRPluginFramework.git \
             https://gitlab.com/thepixellizeross/ctrpluginframework.git \
             https://github.com/Tekito-256/CTRPluginFramework.git; do
    rm -rf ctrpf-src
    if git clone -q --depth 1 --recursive "$url" ctrpf-src >clone.log 2>&1; then
      # newer GCC warns about things in CTRPF's code; don't let -Werror stop the build
      find ctrpf-src/Library -name Makefile -exec sed -i 's/-Werror[^ ]*//g' {} +
      if [ ! -d ctrpf-src/Library ]; then note "libctrpf ($url)" "no Library folder"; continue; fi
      if make -C ctrpf-src/Library install >ctrpf-build.log 2>&1; then
        ok=1; echo "libctrpf built from $url"; break
      else
        note "libctrpf build ($url)" "$(tailof ctrpf-build.log)"
      fi
    else
      note "clone $url" "$(tailof clone.log)"
    fi
  done
  [ $ok = 1 ] || { fail "libctrpf" "could not install libctrpf"; exit 1; }
fi

# ---------- 3. 3gxtool from source ----------
if ! command -v 3gxtool >/dev/null 2>&1; then
  (apt-get update -qq && apt-get install -y -qq cmake g++ make libyaml-cpp-dev) >apt.log 2>&1 \
    || note "apt" "$(tailof apt.log)"
  ok=0
  for url in https://gitlab.com/thepixellizeross/3gxtool.git; do
    rm -rf 3gx-src
    if git clone -q --depth 1 --recursive "$url" 3gx-src >clone.log 2>&1; then
      (
        cd 3gx-src
        ls -la
        if [ -f CMakeLists.txt ]; then
          cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
        elif [ -f Makefile ] || [ -f makefile ]; then
          make -j4
        else
          echo "no build system found"; ls -R | head -50; exit 1
        fi
      ) >3gx-build.log 2>&1
      bin=$(find 3gx-src -type f -name '3gxtool' -perm -u+x | head -1)
      if [ -n "$bin" ]; then
        mkdir -p "$DKP/tools/bin" && cp "$bin" "$DKP/tools/bin/3gxtool"
        ok=1; echo "3gxtool built from $url"; break
      else
        note "3gxtool build" "$(tailof 3gx-build.log)"
      fi
    else
      note "clone $url" "$(tailof clone.log)"
    fi
  done
  [ $ok = 1 ] || { fail "3gxtool" "could not install 3gxtool"; exit 1; }
fi

echo "libctrpf: $(ls $DKP/libctrpf/lib)"; echo "3gxtool: $(command -v 3gxtool)"
