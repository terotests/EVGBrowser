#!/usr/bin/env bash
# EVG Browser → C++ → a native SDL2 desktop binary (Linux, macOS; Windows with MSYS2).
#
#   native/sdl/build.sh            # -> build/evg-browser
#   build/evg-browser [url]
#   build/evg-browser about:demo --screenshot out.png     # render once, headless
#
# Needs: a C++17 compiler, SDL2 and libcurl, and the Ranger checkout in
# ../Ranger or RANGER_DIR. Fonts, images and PNG writing are compiled in.
#   Debian/Ubuntu: sudo apt-get install libsdl2-dev libcurl4-openssl-dev fonts-dejavu-core
#   macOS:         brew install sdl2            (libcurl comes with macOS)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RANGER="${RANGER_DIR:-$ROOT/../Ranger}"
OUT="$ROOT/build"
OPT="${OPT:--O2}"

if [[ ! -f "$RANGER/dist/rgrc.js" ]]; then
  echo "error: Ranger compiler not found at $RANGER/dist/rgrc.js (set RANGER_DIR)" >&2
  exit 1
fi
# SDL2 is the one library to install; libcurl comes with macOS and every
# Linux distribution. Everything else is compiled in (third_party/).
if command -v sdl2-config >/dev/null 2>&1; then
  SDL_FLAGS="$(sdl2-config --cflags --libs)"
elif command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl2; then
  SDL_FLAGS="$(pkg-config --cflags --libs sdl2)"
else
  echo "error: SDL2 not found." >&2
  echo "  macOS:         brew install sdl2" >&2
  echo "  Debian/Ubuntu: sudo apt-get install libsdl2-dev" >&2
  exit 1
fi
if command -v curl-config >/dev/null 2>&1; then
  CURL_FLAGS="$(curl-config --cflags) $(curl-config --libs)"
elif command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libcurl; then
  CURL_FLAGS="$(pkg-config --cflags --libs libcurl)"
else
  CURL_FLAGS="-lcurl"
fi
CXX="${CXX:-$(command -v clang++ || command -v g++)}"

# The script realm's C++ build needs two small ComponentEngine fixes that are
# on Ranger's claude/lucid-darwin-mu2j41 branch until they reach master.
ENGINE="$RANGER/gallery/game_engine/v2/interp/migrate/src/ComponentEngine.rgr"
if grep -q "if ((false == hasBase) && leftNode.left) {" "$ENGINE" 2>/dev/null; then
  echo "error: this Ranger checkout lacks the ComponentEngine fix the desktop build needs." >&2
  echo "  git -C \"$RANGER\" fetch origin claude/lucid-darwin-mu2j41" >&2
  echo "  git -C \"$RANGER\" checkout claude/lucid-darwin-mu2j41" >&2
  exit 1
fi

mkdir -p "$OUT/cpp"
echo "==> 1/2 Ranger -> C++"
cd "$ROOT"
LOG="$(node --stack-size=8000 "$RANGER/dist/rgrc.js" -l=cpp native/sdl/SdlEntry.rgr -d=build/cpp -o=evg_browser.cpp 2>&1)"
if grep -q "FAIL" <<<"$LOG"; then
  echo "$LOG" >&2
  exit 1
fi

echo "==> 2/2 C++ -> build/evg-browser ($CXX $OPT)"
# -w: the generated file is large and warns a lot; the host itself is clean
if ! "$CXX" -std=c++17 $OPT -w -I"$OUT/cpp" native/sdl/evg_browser_sdl.cpp \
  $SDL_FLAGS $CURL_FLAGS -pthread -o "$OUT/evg-browser"; then
  echo "error: the C++ build failed. If the errors are in the generated file (build/cpp/evg_browser.cpp)," >&2
  echo "       update Ranger: older compilers emit C++ that does not build (git -C \"$RANGER\" pull)." >&2
  exit 1
fi
echo "built $OUT/evg-browser"
