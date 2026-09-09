#!/bin/sh
# cross-win64-tests.sh - cross-compile the host unit tests for Windows x86-64.
#
# Produces dist/win64/run_tests.exe plus the two MinGW runtime DLLs it needs
# (libcrypto-3-x64.dll from the MSYS2 package, libwinpthread-1.dll from the
# posix-threaded MinGW runtime). This proves the nginx-free RTP/STUN/SDP/RTCP/
# HSM/session-FSM core is Windows-portable.
#
# The full nginx.exe live service is a separate, fragile path; see
# nginx-rtc-example/docs/windows-mingw-build-guide.md. This script deliberately
# builds only the pure-C protocol core, which has no nginx or third-party
# dependency beyond OpenSSL's HMAC-SHA1 used by STUN.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MINGW_PREFIX=${MINGW_PREFIX:-"$HOME/.local/mingw"}
CC=${CC:-"$MINGW_PREFIX/usr/bin/x86_64-w64-mingw32-gcc-posix"}
THIRD="$MINGW_PREFIX/mingw64"

OUT="$ROOT/dist/win64"
BUILD="$ROOT/test/build-win64"

mkdir -p "$OUT"

make -C "$ROOT/test" -j"${JOBS:-4}" \
    CC="$CC" \
    CPPFLAGS="-I../src -I. -Iinclude -I$THIRD/include" \
    LDLIBS="-L$THIRD/lib -lcrypto -lwinpthread" \
    BUILD_DIR="$BUILD" \
    BIN="$BUILD/run_tests.exe"

cp "$BUILD/run_tests.exe" "$OUT/run_tests.exe"
cp "$THIRD/bin/libcrypto-3-x64.dll" "$OUT/libcrypto-3-x64.dll"
cp "$MINGW_PREFIX/usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll" \
    "$OUT/libwinpthread-1.dll"

cat > "$OUT/README.txt" <<EOF
nginx-rtc-module Windows test runner

Keep run_tests.exe, libcrypto-3-x64.dll and libwinpthread-1.dll in the same
directory. Double-click run_tests.exe, or run it from a terminal:

    run_tests.exe

The program runs the host unit tests for the pure C RTP/STUN/SDP/RTCP/HSM
protocol core and prints pass/fail results. It does not start the RTMP/WHIP
to WebRTC live service.
EOF

if command -v zip >/dev/null 2>&1; then
    (cd "$ROOT/dist" && zip -qr nginx-rtc-win64-tests.zip win64)
    echo "Built $OUT/run_tests.exe and dist/nginx-rtc-win64-tests.zip"
else
    echo "Built $OUT/run_tests.exe"
fi
