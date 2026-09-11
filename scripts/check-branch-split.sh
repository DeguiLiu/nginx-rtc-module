#!/bin/sh
# check-branch-split.sh - fail when win32-compat stops being "main + the Windows set".
#
# The two branches are maintained by hand, and the invariant has drifted three
# times (platform-neutral host-test code, doc refreshes and the Windows guide
# each landed on win32-compat only). This is the machine check for it:
#
#   win32-compat = main + exactly the paths in scripts/branch-split.allow
#
# Anything else -- a file only main has, a path that differs without being on
# the list, a path on the list that no longer differs -- is drift.
#
# usage: scripts/check-branch-split.sh [--repo DIR] [--left BRANCH]
#                                      [--right BRANCH] [--allow FILE]
#                                      [--forbid-win32-tokens]
#
#   --repo DIR              repository to check (default: this script's repo)
#   --left / --right        branch names (default: main / win32-compat)
#   --allow FILE            allow list (default: scripts/branch-split.allow)
#   --forbid-win32-tokens   also require that no _WIN32/winsock/mingw token
#                           appears anywhere on --left (the module repo wants
#                           this; the deploy repo mentions MinGW in its docs)
#
# Exit: 0 clean, 1 drift, 2 usage or setup error.
#
# Keep in sync with nginx-rtc-example/scripts/check-branch-split.sh: the body is
# the same, only the allow list differs.

set -eu

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$SELF_DIR/.." && pwd)
LEFT=main
RIGHT=win32-compat
ALLOW="$REPO/scripts/branch-split.allow"
FORBID_WIN32=0

usage() {
    sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --repo)   REPO=$2; shift 2 ;;
        --left)   LEFT=$2; shift 2 ;;
        --right)  RIGHT=$2; shift 2 ;;
        --allow)  ALLOW=$2; shift 2 ;;
        --forbid-win32-tokens) FORBID_WIN32=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "check-branch-split: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

if [ ! -d "$REPO/.git" ] && [ ! -f "$REPO/.git" ]; then
    echo "check-branch-split: $REPO is not a git repository" >&2
    exit 2
fi
if [ ! -f "$ALLOW" ]; then
    echo "check-branch-split: no allow list at $ALLOW" >&2
    exit 2
fi
for b in "$LEFT" "$RIGHT"; do
    if ! git -C "$REPO" rev-parse --verify --quiet "refs/heads/$b" >/dev/null; then
        echo "check-branch-split: no branch '$b' in $REPO" >&2
        exit 2
    fi
done

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# The allow list: strip comments and blanks, sort.
sed 's/#.*//' "$ALLOW" | awk 'NF' | sort > "$TMP/allow"

git -C "$REPO" diff --name-status "$LEFT" "$RIGHT" > "$TMP/status"

# Only A (the right branch has the file) and M (both have it, contents differ)
# are tolerable, and only for allowed paths. D means the left branch has a file
# the right one does not -- the right branch is no longer a superset. R/C/T
# would need a decision of their own.
if awk '$1 !~ /^[AM]$/ {print}' "$TMP/status" | grep -q .; then
    echo "check-branch-split: DRIFT -- unexpected change kinds between $LEFT and $RIGHT:" >&2
    awk '$1 !~ /^[AM]$/ {print "  " $0}' "$TMP/status" >&2
    if awk '$1 == "D" {found=1} END {exit !found}' "$TMP/status"; then
        echo "  (D: $LEFT has it and $RIGHT does not -- $RIGHT is no longer a superset)" >&2
    fi
    exit 1
fi

awk '$1 == "A" || $1 == "M" {print $NF}' "$TMP/status" | sort > "$TMP/actual"

if ! diff -u "$TMP/allow" "$TMP/actual" > "$TMP/diff"; then
    echo "check-branch-split: DRIFT -- $LEFT and $RIGHT differ outside the allow list:" >&2
    sed -n '3,$p' "$TMP/diff" | sed 's/^/  /' >&2
    echo "  (- lines: allowed but no longer differs; + lines: differs but not allowed)" >&2
    exit 1
fi

# Scope the token check to what gets compiled. The design notes discuss _WIN32
# and MinGW on purpose, so a tree-wide grep fails for the wrong reason; the
# invariant is about code and build inputs.
if [ 1 -eq "$FORBID_WIN32" ]; then
    TOKENS='_WIN32|winsock|ws2tcpip|mingw|_MSC_VER|__declspec|dllimport'
    if git -C "$REPO" grep -n -i -I -E "$TOKENS" "$LEFT" -- src test config | grep -q .; then
        echo "check-branch-split: DRIFT -- Windows-only tokens found on $LEFT (src/ test/ config):" >&2
        git -C "$REPO" grep -n -i -I -E "$TOKENS" "$LEFT" -- src test config | sed 's/^/  /' >&2
        exit 1
    fi
fi

echo "check-branch-split: ok -- $RIGHT = $LEFT + $(wc -l < "$TMP/allow" | tr -d ' ') allowed paths"
