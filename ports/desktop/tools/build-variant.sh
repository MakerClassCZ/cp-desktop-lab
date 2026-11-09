#!/bin/sh
# Build the lab with a change on top, beside the usual build: a fix from an upstream branch, to
# run the same script on both (cpapi.py ab).
#
#   ports/desktop/tools/build-variant.sh NAME CHANGE... [-- MAKE_ARGS...]
#
# CHANGE is a commit or a range (A..B) to cherry-pick, or a .patch/.diff file to apply. The tree
# is a temporary worktree of HEAD with this checkout's submodules linked in; the program lands in
# ports/desktop/build-variants/NAME/. Uncommitted changes of this checkout are not in it.
# Linux and macOS (a Windows program: MAKE_ARGS as for the cross build, in WSL).
set -e
if [ $# -lt 2 ]; then
    sed -n '2,10p' "$0"
    exit 64
fi
NAME=$1
shift
TOP=$(cd "$(dirname "$0")/../../.." && pwd)
OUT="$TOP/ports/desktop/build-variants/$NAME"
WT=$(mktemp -d "${TMPDIR:-/tmp}/cp-variant-XXXXXX")
cleanup() {
    git -C "$TOP" worktree remove --force "$WT" > /dev/null 2>&1 || rm -rf "$WT"
    git -C "$TOP" worktree prune
}
trap cleanup EXIT

git -C "$TOP" worktree add -q --detach "$WT" HEAD
# The submodules this checkout has fetched, linked rather than fetched again.
git -C "$TOP" submodule status | while read -r sha path rest; do
    case "$sha" in -*) continue ;; esac
    rm -rf "${WT:?}/$path"
    ln -s "$TOP/$path" "$WT/$path"
done

while [ $# -gt 0 ] && [ "$1" != "--" ]; do
    case "$1" in
        *.patch | *.diff)
            git -C "$WT" apply --3way "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
            ;;
        *)
            git -C "$WT" -c user.name=variant -c user.email=variant@localhost cherry-pick -x "$1" > /dev/null
            ;;
    esac
    echo "build-variant.sh: $1 applied"
    shift
done
[ "$1" = "--" ] && shift

make -C "$WT/ports/desktop" BUILD=build-variant -j"$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4)" "$@" > "$WT/build.log" 2>&1 || {
    tail -30 "$WT/build.log"
    echo "build-variant.sh: the build failed" >&2
    exit 1
}
mkdir -p "$OUT"
for f in "$WT"/ports/desktop/build-variant/circuitpython "$WT"/ports/desktop/build-variant/circuitpython.exe; do
    [ -f "$f" ] && cp "$f" "$OUT/"
done
echo "build-variant.sh: $(ls "$OUT"/circuitpython* | head -1)"
