#!/bin/sh
# Build the port from a fresh clone and pack it for users, as the GitHub workflow does.
# Usage: ports/desktop/tools/ci-build.sh linux|windows|macos OUT_DIR
# linux: OUT_DIR/circuitpython-linux-ARCH.tar.gz (needs gcc, libsdl2-dev and the Python
# packages of requirements-dev.txt); windows: OUT_DIR/circuitpython-windows-x64.zip, cross built
# with the toolchain bootstrap.sh fetches into CP_TOOLCHAINS (default ~/cp-toolchains); a .vsix
# already packaged in vscode/ goes in too. macos: OUT_DIR/circuitpython-macos-ARCH.tar.gz (needs
# Xcode's command line tools, Homebrew's libusb and the Python packages), with SDL's own
# SDL2.framework fetched into CP_TOOLCHAINS.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
TOOLS=${CP_TOOLCHAINS:-$HOME/cp-toolchains}
JOBS=$(getconf _NPROCESSORS_ONLN)

case "$1" in
linux)
    sh "$HERE/tools/bootstrap.sh" --submodules
    make -C "$HERE" -j"$JOBS" BUILD=build-linux
    NAME=circuitpython-linux-$(uname -m)
    rm -rf "${OUT:?}/$NAME" && mkdir -p "$OUT/$NAME"
    strip -o "$OUT/$NAME/circuitpython" "$HERE/build-linux/circuitpython"
    cp "$HERE/python/debugadapter.py" "$HERE/tools/cp_mcp.py" "$HERE/tools/cpapi.py" "$HERE/README.md" "$OUT/$NAME/"
    mkdir -p "$OUT/$NAME/boards" && cp -r "$HERE/boards/files" "$OUT/$NAME/boards/files"
    cp -r "$HERE/examples" "$OUT/$NAME/examples"
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
macos)
    sh "$HERE/tools/bootstrap.sh" --submodules
    SDL2=SDL2-2.32.8
    if [ ! -d "$TOOLS/SDL2.framework" ]; then
        mkdir -p "$TOOLS"
        curl -fsSL -o "$TOOLS/$SDL2.dmg" "https://github.com/libsdl-org/SDL/releases/download/release-2.32.8/$SDL2.dmg"
        echo "de07f71cc85cd8909c977e105c4f2ca419abd09b981c347e003592186caf6fa0  $TOOLS/$SDL2.dmg" | shasum -a 256 -c --quiet -
        MNT=$(mktemp -d)
        hdiutil attach -nobrowse -readonly -mountpoint "$MNT" "$TOOLS/$SDL2.dmg" > /dev/null
        cp -R "$MNT/SDL2.framework" "$TOOLS/"
        hdiutil detach "$MNT" > /dev/null
        rm "$TOOLS/$SDL2.dmg"
    fi
    # Runs on macOS 11 and later, whatever system builds it.
    MACOSX_DEPLOYMENT_TARGET=11.0 make -C "$HERE" -j"$JOBS" BUILD=build-macos SDL_FRAMEWORK_DIR="$TOOLS"
    NAME=circuitpython-macos-$(uname -m)
    rm -rf "${OUT:?}/$NAME" && mkdir -p "$OUT/$NAME"
    # strip -x keeps the program's ad hoc signature valid, which Apple silicon needs to run it.
    strip -x -o "$OUT/$NAME/circuitpython" "$HERE/build-macos/circuitpython"
    codesign -f -s - "$OUT/$NAME/circuitpython"
    cp -R "$TOOLS/SDL2.framework" "$OUT/$NAME/"
    cp "$(brew --prefix libusb)/lib/libusb-1.0.0.dylib" "$HERE/tools/libusb-LICENSE.txt" "$OUT/$NAME/"
    cp "$HERE/python/debugadapter.py" "$HERE/tools/cp_mcp.py" "$HERE/tools/cpapi.py" "$HERE/README.md" "$OUT/$NAME/"
    cp "$HERE/tools/README-macos.txt" "$OUT/$NAME/README.txt"
    mkdir -p "$OUT/$NAME/boards" && cp -R "$HERE/boards/files" "$OUT/$NAME/boards/files"
    cp -R "$HERE/examples" "$OUT/$NAME/examples"
    tar czf "$OUT/$NAME.tar.gz" -C "$OUT" "$NAME"
    ls -la "$OUT/$NAME.tar.gz"
    ;;
*)
    echo "usage: $0 linux|windows|macos OUT_DIR" >&2
    exit 2
    ;;
esac
