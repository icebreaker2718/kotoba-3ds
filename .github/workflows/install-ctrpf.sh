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
# One source only, at a known commit. On 2026-11-14 upstream's newest commit no longer had the start-up heap line,
# this script fell through to another fork without saying more than a warning, and the plugin built against that
# fork (released as 1.2.0) started and then did nothing on the console. So: no other forks, and a commit the
# plugin has run on (CTRPF_COMMIT); if it is empty, the newest commit that still has the line, and its hash is
# printed so it can be pinned.
CTRPF_URL=https://gitlab.com/thepixellizeross/ctrpluginframework.git
CTRPF_COMMIT=""
if [ ! -f "$DKP/libctrpf/lib/libctrpf.a" ]; then
  rm -rf ctrpf-src
  ok=0
  for attempt in 1 2 3; do
    if git clone -q "$CTRPF_URL" ctrpf-src >clone.log 2>&1; then ok=1; break; fi
    note "clone $CTRPF_URL (attempt $attempt)" "$(tailof clone.log)"
    rm -rf ctrpf-src; sleep $((attempt * 20))
  done
  [ $ok = 1 ] || { fail "libctrpf" "could not clone $CTRPF_URL"; exit 1; }
  echo "CTRPF upstream, newest first:"
  git -C ctrpf-src log -12 --format='  %H %ad %s' --date=short
  has_line() { git -C ctrpf-src grep -q "size = 0x100000; ///< Mostly used for Search" "$1" -- Library/source 2>/dev/null; }
  pick="$CTRPF_COMMIT"
  if [ -z "$pick" ]; then
    for c in $(git -C ctrpf-src rev-list -200 HEAD); do
      if has_line "$c"; then pick=$c; break; fi
    done
    [ -n "$pick" ] || { fail "libctrpf" "no recent upstream commit has the start-up heap line"; exit 1; }
    if [ "$pick" != "$(git -C ctrpf-src rev-parse HEAD)" ]; then
      note "libctrpf" "upstream's newest commit lacks the start-up heap line; using $pick ($(git -C ctrpf-src log -1 --format=%ad --date=short $pick))"
      echo "what upstream has there now:"; git -C ctrpf-src grep -n "Search" HEAD -- Library/source | head -8
    fi
  fi
  git -C ctrpf-src checkout -q "$pick" || { fail "libctrpf" "commit $pick not found upstream"; exit 1; }
  git -C ctrpf-src submodule update -q --init --recursive >/dev/null 2>&1 || true
  echo "libctrpf: commit $(git -C ctrpf-src rev-parse HEAD) ($(git -C ctrpf-src log -1 --format='%ad %s' --date=short))"
  # newer GCC warns about things in CTRPF's code; don't let -Werror stop the build
  find ctrpf-src/Library -name Makefile -exec sed -i 's/-Werror[^ ]*//g' {} +
  # its start-up heap: 1 MiB -> 128 KB (see the top of this file)
  init=$(grep -rl "Mostly used for Search" ctrpf-src/Library/source || true)
  [ -n "$init" ] || { fail "libctrpf" "start-up heap line not found at $pick"; exit 1; }
  sed -i 's/size = 0x100000; \/\/\/< Mostly used for Search/size = 0x20000; \/\/\/< Mostly used for Search (Kotoba3DS: 128 KB)/' $init
  grep -q "size = 0x20000;" $init || { fail "libctrpf" "could not shrink the start-up heap at $pick"; exit 1; }
  if make -C ctrpf-src/Library install >ctrpf-build.log 2>&1; then
    echo "libctrpf built from $CTRPF_URL at $(git -C ctrpf-src rev-parse HEAD)"
  else
    fail "libctrpf build" "$(tailof ctrpf-build.log)"; exit 1
  fi
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
