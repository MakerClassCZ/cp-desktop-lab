#!/bin/sh
# Run the port's tests on a built program, as the GitHub workflow does, and fail when one does.
# Usage: ports/desktop/tools/ci-test.sh PROGRAM [test_x.py ...]   (default: every test)
# A test fails when its script exits non-zero or prints a FAIL line (the scripts themselves
# exit 0 after a FAIL), twice: a failed test runs once more. Tests of hardware that is not there
# skip themselves.
HERE=$(cd "$(dirname "$0")/.." && pwd)
if [ ! -f "$1" ]; then
    echo "no program $1" >&2
    exit 2
fi
PROGRAM=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
shift
PYTHON=${PYTHON:-python3}
# macOS has GNU timeout as gtimeout (Homebrew's coreutils).
TIMEOUT=$(command -v timeout || command -v gtimeout)
export PYTHONIOENCODING=utf-8      # the console text has emoji; Windows would write cp1252
WORK=$(mktemp -d)
cp "$HERE"/tests/*.py "$HERE/tools/cpapi.py" "$HERE/tools/cp_mcp.py" "$WORK/"
# test_airlift's certificates and the libraries its programs import
cp "$HERE"/tests/*.pem "$HERE"/tests/*.key "$WORK/"
cp -r "$HERE/tests/airlift_lib" "$WORK/"
mkdir "$WORK/board_files" && cp "$HERE"/boards/files/*.toml "$WORK/board_files/"
# adafruit_ble from its frozen submodule, when fetched, for test_ble's BLERadio() program
# (git submodule update --init frozen/Adafruit_CircuitPython_BLE); a CIRCUITPY_LIB set wins.
BLE_LIB="$HERE/../../frozen/Adafruit_CircuitPython_BLE/adafruit_ble"
if [ -z "$CIRCUITPY_LIB" ] && [ -d "$BLE_LIB" ]; then
    mkdir "$WORK/lib" && cp -r "$BLE_LIB" "$WORK/lib/"
    CIRCUITPY_LIB="$WORK/lib"
    # Windows's Python takes a Windows path.
    command -v cygpath > /dev/null && CIRCUITPY_LIB=$(cygpath -w "$CIRCUITPY_LIB")
    export CIRCUITPY_LIB
fi
cd "$WORK" || exit 2
[ $# -gt 0 ] || set -- test_*.py
failed=""
again=""
# One test run; true when it passed. Its summary line, and what failed.
run_test() {
    "$TIMEOUT" 300 "$PYTHON" "$1" "$PROGRAM" > "$1.out" 2>&1
    rc=$?
    echo "$1: rc=$rc $(grep -c '^PASS' "$1.out") PASS $(grep -c '^FAIL' "$1.out") FAIL $(grep -c '^SKIP' "$1.out") SKIP"
    if [ $rc -ne 0 ] || grep -q '^FAIL' "$1.out"; then
        grep '^FAIL' "$1.out" | cut -c1-1200
        [ $rc -ne 0 ] && tail -20 "$1.out"
        return 1
    fi
}
for t in "$@"; do
    # A failure is run once more: a slow runner's timing passes the second time, a bug fails again.
    if ! run_test "$t"; then
        echo "$t: again"
        if run_test "$t"; then
            again="$again $t"
        else
            failed="$failed $t"
        fi
    fi
done
# Shown so that a test that passes only now and then does not hide.
[ -n "$again" ] && echo "passed the second time:$again"
if [ -n "$failed" ]; then
    echo "failed:$failed"
    exit 1
fi
