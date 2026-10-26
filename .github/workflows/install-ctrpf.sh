#!/bin/bash
# Installs CTRPluginFramework (libctrpf) and 3gxtool into the devkitPro container.
# libctrpf is always built from source, with one change: CTRPF sets aside 1 MiB of the plugin's
# memory at start for its cheat-search engine, which Kotoba3DS turns off; 128 KB is plenty for the
# rest (file and folder buffers), and the other ~900 KB goes to the OCR. 3gxtool comes from the
# ThePixellizerOSS pacman repository, or is built from source.
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
    if "$PAC" -Sy --noconfirm 3gxtool >pac.log 2>&1; then break; fi
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
      # its start-up heap: 1 MiB -> 128 KB (see the top of this file)
      init=$(grep -rl "Mostly used for Search" ctrpf-src/Library/source || true)
      if [ -z "$init" ]; then note "libctrpf ($url)" "start-up heap line not found"; continue; fi
      sed -i 's/size = 0x100000; \/\/\/< Mostly used for Search/size = 0x20000; \/\/\/< Mostly used for Search (Kotoba3DS: 128 KB)/' $init
      grep -q "size = 0x20000;" $init || { note "libctrpf ($url)" "could not shrink the start-up heap"; continue; }
      # plugin/Sources/mem_hooks.s finds its way back through the HookContext CTRPF keeps in TLS
      # slot 15 (tls + 0x3C): stop here if this copy of CTRPF does it differently
      hm=$(find ctrpf-src/Library/source -name HookManager.cpp | head -1)
      if [ -z "$hm" ] || ! grep -qiE 'add[[:space:]]+r5,[[:space:]]*r5,[[:space:]]*#0x3C' "$hm"; then
        note "ctrpf hook code" "$hm: $(grep -niE 'tls|0x3C|c13|SetHookContext' "$hm" | head -30 | tr '\n' '|' | tr -s ' ')"
        hh=$(find ctrpf-src/Library/include -name Hook.hpp | head -1)
        note "ctrpf HookContext" "$hh: $(grep -n -A12 'struct[[:space:]]*HookContext' "$hh" | head -16 | tr '\n' '|' | tr -s ' ')"
        note "ctrpf files" "$(find ctrpf-src/Library -iname '*hook*' | tr '\n' ' ')"
        fail "libctrpf ($url)" "hooks don't keep their context in TLS slot 15; mem_hooks.s needs updating"; exit 1
      fi
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
