#!/bin/sh
# Set a fresh Linux or WSL2 machine up to build the port: the submodules the build needs, and
# the Windows cross toolchain (llvm-mingw, the MinGW package of SDL2, libusb-1.0.dll) into a directory of
# choice. Usage: ports/desktop/tools/bootstrap.sh [TOOLCHAIN_DIR]   (default ~/cp-toolchains)
#        ports/desktop/tools/bootstrap.sh --submodules               (the submodules only)
set -e
TOP=$(cd "$(dirname "$0")/../../.." && pwd)
DIR=${1:-$HOME/cp-toolchains}
LLVM_MINGW=llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64
SDL2=SDL2-2.32.8

cd "$TOP"
git submodule update --init --depth 1 extmod/ulab lib/tinyusb tools/bitmap_font lib/mp3 \
    lib/certificates lib/quirc lib/adafruit_floppy lib/micropython-lib lib/tlsf lib/protomatter \
    tools/huffman
git submodule update --init --depth 1 --recursive lib/mbedtls
if [ "$1" = --submodules ]; then
    exit 0
fi

# fetch URL FILE SHA256: a download that must match its checksum.
fetch() {
    curl -fsSL -o "$2" "$1"
    echo "$3  $2" | sha256sum -c --quiet -
}

mkdir -p "$DIR"
cd "$DIR"
if [ ! -d "$LLVM_MINGW" ]; then
    fetch "https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/$LLVM_MINGW.tar.xz" \
        llvm-mingw.tar.xz 5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4
    tar xf llvm-mingw.tar.xz
    rm llvm-mingw.tar.xz
fi
if [ ! -d "$SDL2" ]; then
    fetch "https://github.com/libsdl-org/SDL/releases/download/release-2.32.8/SDL2-devel-2.32.8-mingw.tar.gz" \
        sdl2.tar.gz f590b3707689562483ded05bf15eaf0f5d33eb97f49d0fd3b894225fe17cc52b
    tar xzf sdl2.tar.gz
    rm sdl2.tar.gz
fi
if [ ! -f libusb-1.0.dll ]; then
    # for --board ft232h on Windows, from libusb's own release
    fetch https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.7z \
        libusb.7z 7fb1dfec805b97983763d7d0ae244320da12add1003d4249c96cc4d586398c79
    7z e -y libusb.7z VS2022/MS64/dll/libusb-1.0.dll > /dev/null
    rm libusb.7z
fi

cat <<MSG

Linux build (needs gcc >= 10, libsdl2-dev, python3 with the requirements of circuitpython):
    cd $TOP/ports/desktop && make -j\$(nproc)

Windows cross build:
    export PATH=$DIR/$LLVM_MINGW/bin:\$PATH
    cd $TOP/ports/desktop && make -j\$(nproc) BUILD=build-windows CROSS_COMPILE=x86_64-w64-mingw32- SDL_DIR=$DIR/$SDL2/x86_64-w64-mingw32

MSG
