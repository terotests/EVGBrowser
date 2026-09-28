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
#   macOS:         brew install sdl2 sdl2_ttf sdl2_image pkg-config   (libcurl comes with macOS)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RANGER="${RANGER_DIR:-$ROOT/../Ranger}"
OUT="$ROOT/build"
OPT="${OPT:--O2}"

if [[ ! -f "$RANGER/dist/rgrc.js" ]]; then
  echo "error: Ranger compiler not found at $RANGER/dist/rgrc.js (set RANGER_DIR)" >&2
  exit 1
fi
if ! command -v pkg-config >/dev/null 2>&1; then
  echo "error: pkg-config not found." >&2
  echo "  macOS:         brew install pkg-config" >&2
  echo "  Debian/Ubuntu: sudo apt-get install pkg-config" >&2
  exit 1
fi

# Homebrew's curl is keg-only: its .pc file is not on the default path.
if [[ "$(uname -s)" == "Darwin" ]] && command -v brew >/dev/null 2>&1; then
  for f in curl sdl2 sdl2_ttf sdl2_image; do
    pc="$(brew --prefix "$f" 2>/dev/null)/lib/pkgconfig"
    [[ -d "$pc" ]] && PKG_CONFIG_PATH="$pc${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
  done
  export PKG_CONFIG_PATH
fi

MISSING=()
for pkg in sdl2 SDL2_ttf SDL2_image; do
  pkg-config --exists "$pkg" || MISSING+=("$pkg")
done
CURL_FLAGS=""
if pkg-config --exists libcurl; then
  CURL_FLAGS="$(pkg-config --cflags --libs libcurl)"
elif [[ "$(uname -s)" == "Darwin" ]]; then
  CURL_FLAGS="-lcurl"   # the libcurl that ships with macOS
else
  MISSING+=("libcurl")
fi
if (( ${#MISSING[@]} > 0 )); then
  echo "error: not found by pkg-config: ${MISSING[*]}" >&2
  echo "  macOS:         brew install sdl2 sdl2_ttf sdl2_image pkg-config" >&2
  echo "  Debian/Ubuntu: sudo apt-get install libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libcurl4-openssl-dev" >&2
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
if ! "$CXX" -std=c++17 $OPT -w -I"$OUT/cpp" native/sdl/evg_browser_sdl.cpp \
  $(pkg-config --cflags --libs sdl2 SDL2_ttf SDL2_image) $CURL_FLAGS -pthread -o "$OUT/evg-browser"; then
  echo "error: the C++ build failed. If the errors are in the generated file (build/cpp/evg_browser.cpp)," >&2
  echo "       update Ranger: older compilers emit C++ that does not build (git -C \"$RANGER\" pull)." >&2
  exit 1
fi
echo "built $OUT/evg-browser"
