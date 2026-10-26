#!/bin/sh
# Build the port from a fresh clone and pack it for users, as the GitHub workflow does.
# Usage: ports/desktop/tools/ci-build.sh linux|windows OUT_DIR
# linux: OUT_DIR/circuitpython-linux-ARCH.tar.gz (needs gcc, libsdl2-dev and the Python
# packages of requirements-dev.txt); windows: OUT_DIR/circuitpython-windows-x64.zip, cross built
# with the toolchain bootstrap.sh fetches into CP_TOOLCHAINS (default ~/cp-toolchains); a .vsix
# already packaged in vscode/ goes in too.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
TOOLS=${CP_TOOLCHAINS:-$HOME/cp-toolchains}
JOBS=$(nproc)

case "$1" in
linux)
    sh "$HERE/tools/bootstrap.sh" --submodules
    make -C "$HERE" -j"$JOBS" BUILD=build-linux
    NAME=circuitpython-linux-$(uname -m)
    rm -rf "${OUT:?}/$NAME" && mkdir -p "$OUT/$NAME"
    strip -o "$OUT/$NAME/circuitpython" "$HERE/build-linux/circuitpython"
    cp "$HERE/python/debugadapter.py" "$HERE/README.md" "$OUT/$NAME/"
    tar czf "$OUT/$NAME.tar.gz" -C "$OUT" "$NAME"
    ls -la "$OUT/$NAME.tar.gz"
    ;;
windows)
    sh "$HERE/tools/bootstrap.sh" "$TOOLS"
    LLVM_MINGW=$(ls -d "$TOOLS"/llvm-mingw-*-x86_64 | tail -1)
    SDL_DIR=$(ls -d "$TOOLS"/SDL2-*/x86_64-w64-mingw32 | tail -1)
    export PATH="$LLVM_MINGW/bin:$PATH"
    make -C "$HERE" -j"$JOBS" BUILD=build-windows CROSS_COMPILE=x86_64-w64-mingw32- SDL_DIR="$SDL_DIR"
    LIBUSB_DLL="$TOOLS/libusb-1.0.dll" sh "$HERE/tools/package-windows.sh" "$OUT" "$SDL_DIR"
    ;;
*)
    echo "usage: $0 linux|windows OUT_DIR" >&2
    exit 2
    ;;
esac
