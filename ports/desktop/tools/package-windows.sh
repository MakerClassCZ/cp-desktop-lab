#!/bin/sh
# Zip the Windows build for a user: the stripped program, SDL2.dll, libusb-1.0.dll (for
# --board ft232h; LIBUSB_DLL names it, default MSYS2's), the debug adapter, the MCP server, the
# board files, the examples, the port's README, the VS Code extension when one was packaged, and
# any demo directories given; and circuitpython-windows-x64-debug.zip, the same with the program
# unstripped. Usage:
#   ports/desktop/tools/package-windows.sh OUT_DIR SDL_DIR [demo-dir ...]
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1; SDL_DIR=$2; shift 2
for NAME in circuitpython-windows-x64 circuitpython-windows-x64-debug; do
    P="$OUT/$NAME"
    rm -rf "$P" && mkdir -p "$P"
    if [ "$NAME" = circuitpython-windows-x64 ]; then
        "${CROSS_COMPILE:-x86_64-w64-mingw32-}strip" -o "$P/circuitpython.exe" "$HERE/build-windows/circuitpython.exe"
    else
        cp "$HERE/build-windows/circuitpython.exe" "$P/"
    fi
    cp "$SDL_DIR/bin/SDL2.dll" "$HERE/python/debugadapter.py" "$HERE/tools/cp_mcp.py" "$HERE/tools/cpapi.py" \
        "$HERE/README.md" "$P/"
    mkdir -p "$P/boards" && cp -r "$HERE/boards/files" "$P/boards/files"
    cp -r "$HERE/examples" "$P/examples"
    cp "${LIBUSB_DLL:-/mnt/c/msys64/ucrt64/bin/libusb-1.0.dll}" "$P/libusb-1.0.dll"
    cp "$HERE/tools/libusb-LICENSE.txt" "$P/"
    cp "$HERE"/vscode/*.vsix "$P/" 2>/dev/null || echo "no .vsix: run 'npx @vscode/vsce package --allow-missing-repository --skip-license' in vscode/"
    for d in "$@"; do mkdir -p "$P/demo" && cp -r "$d" "$P/demo/$(basename "$d")"; done
    cp "$HERE/tools/README-windows.txt" "$P/README.txt"
    (cd "$OUT" && rm -f "$NAME.zip" && zip -qr "$NAME.zip" "$NAME")
    ls -la "$OUT/$NAME.zip"
done
