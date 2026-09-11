#!/bin/sh
# check-worlds.sh - run the host suite in both header worlds and require both green.
#
# test/Makefile compiles against either a configured nginx tree (the real world)
# or test/include (the stub world), and the probe that decides is relative to the
# checkout -- so the same command means different things in different worktrees.
# A green run only ever proves the world it was compiled in, which makes "the
# tests pass" half a claim unless both worlds are run. This does that:
#
#   1. stub world  make -C test NGX_SRC=
#   2. real world  make -C test NGX_SRC=<abs nginx tree>
#
# Each run must print the expected `header world:` line and end with FAIL: 0 and
# PASS == TOTAL; both are counted here rather than hard-coded, so the check
# survives the suite growing.
#
# scripts/sanitize-tests.sh takes the same NGX_SRC and passes it through, so a
# caller that wants the sanitizer in a specific world only has to export it.
#
# usage: scripts/check-worlds.sh [--ngx-src DIR] [--jobs N]
#
#   --ngx-src DIR  nginx tree containing objs/ngx_auto_config.h (default: probe
#                  the same ../../nginx-rtc-example path test/Makefile probes)
#   --jobs N       make -j (default: nproc)
#
# Exit: 0 both worlds green, 1 a world failed, 2 setup error.

set -eu

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SELF_DIR/.." && pwd)
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}
NGX_TREE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --ngx-src) NGX_TREE=$2; shift 2 ;;
        --jobs)    JOBS=$2; shift 2 ;;
        -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "check-worlds: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$NGX_TREE" ]; then
    for d in "$ROOT"/../nginx-rtc-example/build/src/openresty-*/build/nginx-*; do
        if [ -f "$d/objs/ngx_auto_config.h" ]; then
            NGX_TREE=$(CDPATH= cd -- "$d" && pwd)
            break
        fi
    done
fi

if [ -z "$NGX_TREE" ] || [ ! -f "$NGX_TREE/objs/ngx_auto_config.h" ]; then
    echo "check-worlds: no configured nginx tree found; pass --ngx-src DIR" >&2
    echo "              (a tree with objs/ngx_auto_config.h -- the real world" >&2
    echo "               cannot be checked without one)" >&2
    exit 2
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

check_one() {
    name=$1
    expect=$2
    ngx_src=$3
    log="$TMP/$name.log"

    echo
    echo "== [$name] NGX_SRC=${ngx_src:-<empty: stub world>} =="

    if ! make -C "$ROOT/test" -j"$JOBS" NGX_SRC="$ngx_src" test > "$log" 2>&1; then
        echo "check-worlds: $name FAILED to build or run; last 20 lines:" >&2
        tail -20 "$log" >&2
        return 1
    fi

    world=$(grep -m1 '^header world: ' "$log" || true)
    total=$(grep -m1 '^TOTAL: ' "$log" || true)
    echo "  $world"
    echo "  $total"

    if ! printf '%s' "$world" | grep -q "^header world: $expect"; then
        echo "check-worlds: $name ran in the wrong header world (expected '$expect')" >&2
        return 1
    fi

    n_total=$(printf '%s' "$total" | sed -n 's/^TOTAL: \([0-9]*\).*/\1/p')
    n_pass=$(printf '%s' "$total" | sed -n 's/.*PASS: \([0-9]*\).*/\1/p')
    n_fail=$(printf '%s' "$total" | sed -n 's/.*FAIL: \([0-9]*\).*/\1/p')

    if [ -z "$n_total" ] || [ -z "$n_pass" ] || [ -z "$n_fail" ]; then
        echo "check-worlds: $name printed no TOTAL line" >&2
        tail -20 "$log" >&2
        return 1
    fi
    if [ "$n_fail" != 0 ] || [ "$n_pass" != "$n_total" ] || [ "$n_total" = 0 ]; then
        echo "check-worlds: $name is not green (total=$n_total pass=$n_pass fail=$n_fail)" >&2
        return 1
    fi
    echo "  -> ok ($n_pass/$n_total)"
}

rc=0
check_one stub stub "" || rc=1
check_one real real "$NGX_TREE" || rc=1

echo
if [ $rc -eq 0 ]; then
    echo "check-worlds: ok -- both header worlds green"
else
    echo "check-worlds: FAILED -- see above" >&2
fi
exit $rc
