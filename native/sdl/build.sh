#!/usr/bin/env bash
# EVG Browser → C++ → a native SDL3 desktop binary (Linux, macOS; Windows with MSYS2).
#
#   native/sdl/build.sh            # -> build/evg-browser
#   build/evg-browser [url]
#   build/evg-browser about:demo --screenshot out.png     # render once, headless
#
# Needs: a C++17 compiler, SDL3 and libcurl, and `npm install` run once (the
# Ranger compiler and packages). Fonts, images and PNG writing are compiled in.
#   Debian/Ubuntu: sudo apt-get install libsdl3-dev libcurl4-openssl-dev fonts-dejavu-core
#                  (Ubuntu 25.04+, Debian 13+; older ones: build SDL3 from source)
#   macOS:         brew install sdl3            (libcurl comes with macOS)
# SDL3 is found with pkg-config (PKG_CONFIG_PATH for one of your own);
# SDL3_STATIC=1 links a static libSDL3.a.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build"
OPT="${OPT:--O2}"

# the compiler and the Ranger packages (npm install); exits with advice if missing
RGRC="$(cd "$ROOT" && node tools/ranger.mjs rgrc)"
# SDL3 is the one library to install; libcurl comes with macOS and every
# Linux distribution. Everything else is compiled in (third_party/).
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl3; then
  if [ "${SDL3_STATIC:-}" = "1" ]; then
    SDL_FLAGS="$(pkg-config --cflags --libs --static sdl3)"
  else
    SDL_FLAGS="$(pkg-config --cflags --libs sdl3)"
  fi
else
  echo "error: SDL3 not found (pkg-config sdl3)." >&2
  echo "  macOS:         brew install sdl3" >&2
  echo "  Debian/Ubuntu: sudo apt-get install libsdl3-dev" >&2
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

mkdir -p "$OUT/cpp"
echo "==> 1/2 Ranger -> C++"
cd "$ROOT"
# The compiler recurses deeply; --stack-size must stay under the thread's
# real stack or node crashes without a word (macOS: 8 MB by default). Ask
# for a bigger one where the shell allows it.
ulimit -s 65520 2>/dev/null || ulimit -s unlimited 2>/dev/null || true
STACK_KB=8000
if [ "$(ulimit -s)" = "unlimited" ] || [ "$(ulimit -s)" -ge 60000 ]; then
  STACK_KB=48000
fi
set +e
LOG="$(node --max-old-space-size=8192 --stack-size=$STACK_KB "$RGRC" -l=cpp native/sdl/SdlEntry.rgr -d=build/cpp -o=evg_browser.cpp 2>&1)"
STATUS=$?
set -e
if [ $STATUS -ne 0 ] || grep -q "FAIL" <<<"$LOG"; then
  echo "$LOG" | tail -40 >&2
  echo "error: Ranger -> C++ failed (exit $STATUS, stack ${STACK_KB} KB)." >&2
  echo "       If it is an old package, run npm install first." >&2
  exit 1
fi

echo "==> 2/2 C++ -> build/evg-browser ($CXX $OPT)"
# -w: the generated file is large and warns a lot; the host itself is clean
if ! "$CXX" -std=c++17 $OPT -w -I"$OUT/cpp" native/sdl/evg_browser_sdl.cpp \
  $SDL_FLAGS $CURL_FLAGS -pthread -o "$OUT/evg-browser"; then
  echo "error: the C++ build failed. If the errors are in the generated file (build/cpp/evg_browser.cpp)," >&2
  echo "       run npm install to update the Ranger compiler and packages." >&2
  exit 1
fi
echo "built $OUT/evg-browser"
