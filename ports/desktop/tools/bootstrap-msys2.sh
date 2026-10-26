#!/bin/sh
# A native build on Windows in an MSYS2 UCRT64 shell (https://www.msys2.org): the packages, the
# submodules, and the command lines. Untried so far; the cross build from Linux is the tested
# path. Run from the repository: sh ports/desktop/tools/bootstrap-msys2.sh
set -e
TOP=$(cd "$(dirname "$0")/../../.." && pwd)
pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2 \
    mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-pip make git zip
cd "$TOP"
git submodule update --init --depth 1 extmod/ulab lib/tinyusb tools/bitmap_font lib/mp3 \
    lib/certificates lib/quirc lib/adafruit_floppy lib/micropython-lib lib/tlsf
git submodule update --init --depth 1 --recursive lib/mbedtls
cat <<MSG

Build (the Makefile sees OS=Windows_NT and builds the Windows program; SDL2 comes from MSYS2):
    cd $TOP/ports/desktop && make -j\$(nproc) BUILD=build-windows SDL_DIR=/ucrt64

Run:
    build-windows/circuitpython.exe --drive C:/some/folder
MSG
