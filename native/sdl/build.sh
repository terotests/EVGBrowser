#!/usr/bin/env bash
# EVG Browser → C++ → a native SDL2 desktop binary (Linux, macOS; Windows with MSYS2).
#
#   native/sdl/build.sh            # -> build/evg-browser
#   build/evg-browser [url]
#   build/evg-browser about:demo --screenshot out.png     # render once, headless
#
# Needs: a C++17 compiler, SDL2, SDL2_ttf, SDL2_image and libcurl (with
# pkg-config), and the Ranger checkout in ../Ranger or RANGER_DIR.
#   Debian/Ubuntu: sudo apt-get install libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libcurl4-openssl-dev fonts-dejavu-core
#   macOS:         brew install sdl2 sdl2_ttf sdl2_image curl pkg-config
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RANGER="${RANGER_DIR:-$ROOT/../Ranger}"
OUT="$ROOT/build"
OPT="${OPT:--O2}"

if [[ ! -f "$RANGER/dist/rgrc.js" ]]; then
  echo "error: Ranger compiler not found at $RANGER/dist/rgrc.js (set RANGER_DIR)" >&2
  exit 1
fi
if ! pkg-config --exists sdl2 SDL2_ttf SDL2_image libcurl; then
  echo "error: SDL2, SDL2_ttf, SDL2_image or libcurl not found by pkg-config" >&2
  exit 1
fi
CXX="${CXX:-$(command -v clang++ || command -v g++)}"

mkdir -p "$OUT/cpp"
echo "==> 1/2 Ranger -> C++"
cd "$ROOT"
LOG="$(node "$RANGER/dist/rgrc.js" -l=cpp src/BrowserApp.rgr -d=build/cpp -o=evg_browser.cpp 2>&1)"
if grep -q "FAIL" <<<"$LOG"; then
  echo "$LOG" >&2
  exit 1
fi

echo "==> 2/2 C++ -> build/evg-browser ($CXX $OPT)"
# -w: the generated file is large and warns a lot; the host itself is clean
"$CXX" -std=c++17 $OPT -w -I"$OUT/cpp" native/sdl/evg_browser_sdl.cpp \
  $(pkg-config --cflags --libs sdl2 SDL2_ttf SDL2_image libcurl) -pthread -o "$OUT/evg-browser"
echo "built $OUT/evg-browser"
