#!/bin/sh
# Set a fresh Linux or WSL2 machine up to build the port: the submodules the build needs, and
# the Windows cross toolchain (llvm-mingw and the MinGW package of SDL2) into a directory of
# choice. Usage: ports/desktop/tools/bootstrap.sh [TOOLCHAIN_DIR]   (default ~/cp-toolchains)
set -e
TOP=$(cd "$(dirname "$0")/../../.." && pwd)
DIR=${1:-$HOME/cp-toolchains}
LLVM_MINGW=llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64
SDL2=SDL2-2.32.8

cd "$TOP"
git submodule update --init --depth 1 extmod/ulab lib/tinyusb tools/bitmap_font lib/mp3 \
    lib/certificates lib/quirc lib/adafruit_floppy lib/micropython-lib lib/tlsf
git submodule update --init --depth 1 --recursive lib/mbedtls

mkdir -p "$DIR"
cd "$DIR"
if [ ! -d "$LLVM_MINGW" ]; then
    curl -sSL -o llvm-mingw.tar.xz "https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/$LLVM_MINGW.tar.xz"
    tar xf llvm-mingw.tar.xz && rm llvm-mingw.tar.xz
fi
if [ ! -d "$SDL2" ]; then
    curl -sSL -o sdl2.tar.gz "https://github.com/libsdl-org/SDL/releases/download/release-2.32.8/$SDL2-devel-2.32.8-mingw.tar.gz"
    tar xzf sdl2.tar.gz && rm sdl2.tar.gz
fi

cat <<MSG

Linux build (needs gcc >= 10, libsdl2-dev, python3 with the requirements of circuitpython):
    cd $TOP/ports/desktop && make -j\$(nproc)

Windows cross build:
    export PATH=$DIR/$LLVM_MINGW/bin:\$PATH
    cd $TOP/ports/desktop && make -j\$(nproc) BUILD=build-windows CROSS_COMPILE=x86_64-w64-mingw32- SDL_DIR=$DIR/$SDL2/x86_64-w64-mingw32

MSG
