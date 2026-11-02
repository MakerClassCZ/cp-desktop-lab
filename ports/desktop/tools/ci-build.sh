#!/bin/sh
# Build the port from a fresh clone and pack it for users, as the GitHub workflow does.
# Usage: ports/desktop/tools/ci-build.sh TARGET OUT_DIR
# linux: OUT_DIR/circuitpython-linux-ARCH.tar.gz (needs gcc, libsdl2-dev and the Python
# packages of requirements-dev.txt); linux-i386: the 32-bit program, circuitpython-linux-i386.tar.gz
# (needs gcc-multilib and libsdl2-dev:i386, found through PKG_CONFIG_LIBDIR); windows:
# OUT_DIR/circuitpython-windows-x64.zip, cross built with the toolchain bootstrap.sh fetches into
# CP_TOOLCHAINS (default ~/cp-toolchains); a .vsix already packaged in vscode/ goes in too. macos:
# OUT_DIR/circuitpython-macos-ARCH.tar.gz (needs Xcode's command line tools, Homebrew's libusb and
# the Python packages), with SDL's own SDL2.framework fetched into CP_TOOLCHAINS. Each package comes
# twice: stripped, and as NAME-debug with the same program unstripped.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
TOOLS=${CP_TOOLCHAINS:-$HOME/cp-toolchains}
JOBS=$(getconf _NPROCESSORS_ONLN)

# The program in BUILD_DIR, the files beside it and any extra files given, in OUT/NAME.
# STRIP empty keeps the symbols.
pack() {
    NAME=$1; BUILD_DIR=$2; STRIP=$3; shift 3
    rm -rf "${OUT:?}/$NAME" && mkdir -p "$OUT/$NAME"
    if [ -n "$STRIP" ]; then
        $STRIP -o "$OUT/$NAME/circuitpython" "$HERE/$BUILD_DIR/circuitpython"
    else
        cp "$HERE/$BUILD_DIR/circuitpython" "$OUT/$NAME/"
    fi
    cp "$HERE/python/debugadapter.py" "$HERE/tools/cp_mcp.py" "$HERE/tools/cpapi.py" "$HERE/README.md" "$HERE/AGENTS.md" "$OUT/$NAME/"
    mkdir -p "$OUT/$NAME/boards" && cp -R "$HERE/boards/files" "$OUT/$NAME/boards/files"
    cp -R "$HERE/examples" "$OUT/$NAME/examples"
    for f in "$@"; do cp -R "$f" "$OUT/$NAME/"; done
}

tarball() {
    tar czf "$OUT/$1.tar.gz" -C "$OUT" "$1"
    ls -la "$OUT/$1.tar.gz"
}

# NAME stripped and NAME-debug, both as tarballs.
pack_both() {
    BOTH=$1; shift
    pack "$BOTH" "$@"
    tarball "$BOTH"
    B=$1; shift 2
    pack "$BOTH-debug" "$B" "" "$@"
    tarball "$BOTH-debug"
}

case "$1" in
linux)
    sh "$HERE/tools/bootstrap.sh" --submodules
    make -C "$HERE" -j"$JOBS" BUILD=build-linux
    pack_both "circuitpython-linux-$(uname -m)" build-linux strip
    ;;
linux-i386)
    sh "$HERE/tools/bootstrap.sh" --submodules
    # sdl2-config would give the 64-bit SDL; pkg-config with PKG_CONFIG_LIBDIR gives the 32-bit one.
    make -C "$HERE" -j"$JOBS" BUILD=build-linux-i386 CC="gcc -m32" \
        SDL_CFLAGS="$(pkg-config --cflags sdl2)" SDL_LIBS="$(pkg-config --libs sdl2)"
    pack_both circuitpython-linux-i386 build-linux-i386 strip
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
    BASE=circuitpython-macos-$(uname -m)
    for P in "$BASE" "$BASE-debug"; do
        # strip -x keeps the program's ad hoc signature valid, which Apple silicon needs to run it.
        if [ "$P" = "$BASE" ]; then STRIP="strip -x"; else STRIP=""; fi
        pack "$P" build-macos "$STRIP" "$TOOLS/SDL2.framework"
        codesign -f -s - "$OUT/$P/circuitpython"
        # The lines are in the object files, which dsymutil gathers beside the debug program.
        [ -n "$STRIP" ] || dsymutil "$HERE/build-macos/circuitpython" -o "$OUT/$P/circuitpython.dSYM"
        cp "$(brew --prefix libusb)/lib/libusb-1.0.0.dylib" "$HERE/tools/libusb-LICENSE.txt" "$OUT/$P/"
        cp "$HERE/tools/README-macos.txt" "$OUT/$P/README.txt"
        tarball "$P"
    done
    ;;
*)
    echo "usage: $0 linux|linux-i386|windows|macos OUT_DIR" >&2
    exit 2
    ;;
esac
