#!/usr/bin/env bash
# sanitize-tests.sh - build and run the host unit tests under a sanitizer.
#
# Same driving style as scripts/cross-win64-tests.sh: every difference from the
# default build is a `make -C test` variable override, so test/Makefile carries
# no sanitizer knowledge and a plain `make -C test test` keeps working (it must:
# the Windows cross build uses that path unchanged).
#
#   scripts/sanitize-tests.sh                 # address,undefined (default)
#   SAN=address scripts/sanitize-tests.sh     # ASan only
#   SAN=undefined scripts/sanitize-tests.sh   # UBSan only
#   SAN=thread scripts/sanitize-tests.sh      # TSan: separate run, see below
#
# ASan and TSan cannot be linked into one binary, so TSan is its own invocation;
# it is only meaningful for the units that actually have threads.
#
# Two binaries are built, deliberately:
#
#   1. run_tests        the main suite. Unchanged from `make -C test test`.
#   2. run_tests_srtp   the srtp unit against the real libsrtp2.
#
# They cannot be one binary. The main suite's session tests drive
# ngx_rtc_session_send_rtp with a zeroed ngx_rtc_session_t (FSM forced ready,
# ngx_rtc_srtp_t never created); that only works because test/nginx_stub.c
# stands in for ngx_rtc_srtp_protect_rtp as a pass-through. Linking the real
# unit in makes 8 of them fail on a state production cannot reach -- the FSM
# only goes ready after ngx_rtc_srtp_create succeeds (ngx_rtc_stream_module.c
# :1104). Splitting keeps both signals clean instead of trading one for the
# other.
#
# Artifacts land in test/build-asan/ -- deliberately NOT test/build/asan/,
# because `make clean` in test/ removes test/build wholesale and a sibling
# directory survives that.
#
# Before trusting a green run, prove the sanitizer is live. The obvious probe
# ("overflow a malloc'd buffer, expect a report") does NOT work:
#
#     char *p = malloc(8);
#     p[8] = 'x';          /* -O1: gcc promotes the object to the stack and
#                             deletes the store -- nothing to report */
#
# With the size and index behind a `volatile`, the store is emitted and ASan
# reports "heap-buffer-overflow ... WRITE of size 1 ... 0 bytes to the right of
# 8-byte region" with the exact source line. Same for UBSan:
#
#     volatile int seed = 0x7fffffff;
#     int v = seed; v = v + 1;   /* signed integer overflow */
#
# A probe that reports nothing is far more likely to have been optimized away
# than to indicate working detection. The leak checker had to be proven the same
# way, and it is the easiest to fool:
#
#     static void *g;
#     int main(void) { g = malloc(4321); g = NULL; return 0; }
#
# compiles to `mov $0,%eax; ret` -- the allocation is gone, there is nothing to
# leak, and the run is green. Writing one byte into the allocation keeps it:
#
#     static void *g;
#     int main(void) { char *p = malloc(4321); p[0] = 1; g = p; g = NULL; return 0; }
#
# reports "4321 byte(s) leaked in 1 allocation(s)". Note also that clearing the
# pointer matters: LeakSanitizer treats a value still visible in a register or
# stack slot as reachable and stays silent.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SAN=${SAN:-address,undefined}

# Keyed by sanitizer so switching SAN never reuses objects compiled with a
# different set of flags (make does not track flag changes).
BUILD_DIR="$ROOT/test/build-asan/$(printf '%s' "$SAN" | tr ',' '-')"

MAIN_CORE="ngx_rtc_rtp ngx_rtc_stun ngx_rtc_sdp ngx_rtc_rtcp ngx_rtc_hsm \
           ngx_rtc_session_fsm ngx_rtc_core ngx_rtc_ring ngx_rtc_avsync \
           ngx_rtc_jitter ngx_rtc_shm ngx_rtc_dtls"
MAIN_TEST="ngx_rtc_test test_main test_rtp test_stun test_sdp test_rtcp \
           test_hsm test_session_fsm nginx_stub test_wrap test_rtc_core \
           test_ring test_avsync test_jitter test_malformed test_shm \
           test_stream_module"

# -O1 instead of the default -O2: -O2 folds away some use-after-scope and
# signed-overflow checks and skews the reported line numbers.
#
# -fno-sanitize-recover=all is the important one. Without it UBSan prints the
# diagnostic and KEEPS GOING, so the suite still reports "all passed" and the
# detection is worthless. With it, the first finding aborts the run.
#
# The suffix is appended to BOTH flag sets. test/Makefile keeps two: NGX_CFLAGS
# for the production sources (nginx's own warning set) and TEST_CFLAGS for the
# test files. Overriding only one would leave the other group built without the
# sanitizer -- a green run that proves nothing, which is worse than a failed
# build.
SAN_FLAGS="-fsanitize=$SAN -fno-omit-frame-pointer -fno-sanitize-recover=all"
NGX_CFLAGS="-std=c11 -O1 -W -Wall -Wpointer-arith -Wno-unused-parameter -g -MMD -MP $SAN_FLAGS"
TEST_CFLAGS="-std=c11 -Wall -Wextra -Werror -O1 -g -MMD -MP $SAN_FLAGS"

case "$SAN" in
    *thread*)
        # TSan has no leak checker and no ASan knobs; keep the env minimal.
        RUN_ENV=(env TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1")
        ;;
    *)
        RUN_ENV=(env
            ASAN_OPTIONS="detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1:allocator_may_return_null=1"
            UBSAN_OPTIONS="print_stacktrace=1")
        ;;
esac

# Locate the third-party tree (headers + static libs) built by nginx-rtc-example.
THIRD=${NGX_RTC_THIRD:-}
if [ -z "$THIRD" ]; then
    for c in "$ROOT/../nginx-rtc-example/build/third" "$ROOT/../build/third"; do
        if [ -d "$c/lib" ]; then
            THIRD="$c"
            break
        fi
    done
fi
HAVE_SRTP=0
if [ -n "${THIRD:-}" ] && [ -f "$THIRD/lib/libsrtp2.a" ]; then
    HAVE_SRTP=1
fi

# ASLR must be off, or every result below is a coin flip.
#
# This host's kernel randomizes mmap base addresses with enough entropy that
# ASan's ~20TB shadow reservation intermittently collides with an existing
# mapping and the process dies during startup -- which shows up as a
# "AddressSanitizer:DEADLYSIGNAL" loop, not as a readable report, so it is easy
# to misread as a finding in the code under test. Measured on the same binary:
# 9 of 30 runs died with ASLR on, 0 of 30 with it off.
#
# `setarch -R` only affects this process, needs no privileges, and is a no-op
# for a plain (non-sanitizer) build -- which is why `make -C test test` is
# unaffected and stays as it is.
#
# The kernel-wide alternative is `sysctl -w vm.mmap_rnd_bits=28` (needs root);
# prefer setarch, it does not change the machine for anything else.
#
# stdbuf is NOT usable here for the same family of reasons: it works by
# LD_PRELOAD, which collides with ASan's requirement to be first in the initial
# library list ("ASan runtime does not come first in initial library list").
if command -v setarch >/dev/null 2>&1; then
    RUN_PREFIX=(setarch "$(uname -m)" -R)
else
    echo "sanitize-tests: setarch not found -- ASan may crash intermittently;" >&2
    echo "                install util-linux or set vm.mmap_rnd_bits=28" >&2
    RUN_PREFIX=()
fi

# Always start clean: the whole suite builds in seconds, and a stale object
# from a previous SAN= run would silently defeat the point of this script.
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo "== sanitize-tests: SAN=$SAN =="

# --- run each unit group in its own binary --------------------------------
#
# Each group links a different set of libraries, so each needs its own binary;
# and each is checked separately so one group's failure cannot be mistaken for
# another's. The main suite runs first because it is the one that must always
# be green.
#
# NAME | core units | test units | extra libs (may be empty) | needs $THIRD/include
run_group() {
    local name="$1" core="$2" tests="$3" libs="$4" third_inc="$5"
    local cpp=""

    echo
    echo "== [$name] =="

    if [ -n "$third_inc" ]; then
        # -I$THIRD/include must precede -Iinclude so the real headers win over
        # the stubs the main build relies on. It goes through EXTRA_CPPFLAGS
        # rather than replacing CPPFLAGS, so test/Makefile keeps its own
        # -D_GNU_SOURCE, its nginx-header paths and its -Iinclude.
        cpp="-I$THIRD/include"
    fi

    make -C "$ROOT/test" -j"${JOBS:-$(nproc)}" \
        NGX_CFLAGS="$NGX_CFLAGS" \
        TEST_CFLAGS="$TEST_CFLAGS" \
        EXTRA_CPPFLAGS="$cpp" \
        LDLIBS="$libs -fsanitize=$SAN" \
        BUILD_DIR="$BUILD_DIR/$name" \
        BIN="$BUILD_DIR/run_tests_$name" \
        CORE_NAMES="$core" \
        TEST_NAMES="$tests"

    "${RUN_PREFIX[@]}" "${RUN_ENV[@]}" "$BUILD_DIR/run_tests_$name"
}

run_group main \
    "$MAIN_CORE" \
    "$MAIN_TEST" \
    "-lssl -lcrypto" \
    ""

if [ 0 -eq "$HAVE_SRTP" ]; then
    echo
    echo "== srtp / audio / audio_worker: SKIPPED ==" >&2
    echo "   no libsrtp2.a under ${THIRD:-<unset>}; build it with" >&2
    echo "   nginx-rtc-example/scripts/build-deps.sh" >&2
    exit 0
fi

AV_LIBS="$THIRD/lib/libavcodec.a $THIRD/lib/libswresample.a \
         $THIRD/lib/libavutil.a $THIRD/lib/libopus.a"

run_group srtp \
    "ngx_rtc_srtp" \
    "ngx_rtc_test test_main test_srtp" \
    "-lcrypto $THIRD/lib/libsrtp2.a -lm -lpthread" \
    yes

# dtls needs the system OpenSSL only (the module creates its own self-signed
# certificate and does the handshake over memory BIOs).
run_group dtls \
    "ngx_rtc_dtls" \
    "ngx_rtc_test test_main test_dtls" \
    "-lssl -lcrypto" \
    ""

run_group audio \
    "ngx_rtc_audio" \
    "ngx_rtc_test test_main test_audio" \
    "$AV_LIBS -lm -lpthread" \
    yes

# audio_worker is the only threaded unit, so it is also the TSan target:
# SAN=thread scripts/sanitize-tests.sh builds this group under ThreadSanitizer.
run_group audio_worker \
    "ngx_rtc_audio_worker ngx_rtc_audio ngx_rtc_ring" \
    "ngx_rtc_test test_main test_audio_worker" \
    "$AV_LIBS -lm -lpthread" \
    yes
