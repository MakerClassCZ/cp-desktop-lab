#!/bin/sh
# Run the port's tests on a built program, as the GitHub workflow does, and fail when one does.
# Usage: ports/desktop/tools/ci-test.sh PROGRAM [test_x.py ...]   (default: every test)
# A test fails when its script exits non-zero or prints a FAIL line (the scripts themselves
# exit 0 after a FAIL). Tests of hardware that is not there skip themselves.
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
mkdir "$WORK/board_files" && cp "$HERE"/boards/files/*.toml "$WORK/board_files/"
cd "$WORK" || exit 2
[ $# -gt 0 ] || set -- test_*.py
failed=""
for t in "$@"; do
    "$TIMEOUT" 300 "$PYTHON" "$t" "$PROGRAM" > "$t.out" 2>&1
    rc=$?
    echo "$t: rc=$rc $(grep -c '^PASS' "$t.out") PASS $(grep -c '^FAIL' "$t.out") FAIL $(grep -c '^SKIP' "$t.out") SKIP"
    if [ $rc -ne 0 ] || grep -q '^FAIL' "$t.out"; then
        failed="$failed $t"
        grep '^FAIL' "$t.out" | cut -c1-1200
        [ $rc -ne 0 ] && tail -20 "$t.out"
    fi
done
if [ -n "$failed" ]; then
    echo "failed:$failed"
    exit 1
fi
