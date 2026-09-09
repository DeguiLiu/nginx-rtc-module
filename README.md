# nginx-rtc-module

Self-developed nginx C addon that turns **RTMP / WHIP** ingest into **WebRTC**
(RTP/SRTP over UDP) playout, running on OpenResty/nginx. The assembly + deploy
repo that consumes this addon lives at
[github.com/DeguiLiu/nginx-rtc-example](https://github.com/DeguiLiu/nginx-rtc-example)
(signaling SDP, Lua auth, player pages, HTTP-FLV/HLS output, run scripts).

One `--add-module` registers four nginx modules from this tree:

| module | type | role |
|---|---|---|
| `ngx_rtc_core_module` | core | `rtc_zone` shared-memory registry + pacing timer |
| `ngx_rtmp_rtc_bridge_module` | core (rtmp) | RTMP live stream → RTC source bridge |
| `ngx_rtc_http_module` | http | WHIP / RTC signaling (`/rtc/v1/play/`) + stats |
| `ngx_rtc_stream_module` | stream | UDP media plane (SRTP/SRTCP, ICE/STUN, DTLS done hook) |

## Layout

- `config` — nginx addon build script (reads `NGX_RTC_THIRD` for third-party static libs).
- `src/` — pure-C core (`rtp/sdp/stun/dtls/srtp/ring/audio/rtcp/hsm/session_fsm`,
  no nginx headers, host-testable) + the four module glues.
- `test/` — host unit tests (no nginx / no third-party dependency).

## Build

The module links static third-party libs
(libavcodec/libswresample/libavutil/libopus/libsrtp2). Point the addon at their
`include/` + `lib/` by exporting the env var `config` reads:

```sh
export NGX_RTC_THIRD=/path/to/third          # include/ + lib/ (see nginx-rtc-example scripts/build-deps.sh)
./configure ... --add-module=/path/to/nginx-rtc-module
```

The host unit tests need none of that:

```sh
make -C test run_tests       # 87 cases
```

## Windows portability

The nginx-free protocol core (`rtp/sdp/stun/rtcp/hsm/session_fsm/core/ring/
avsync`) compiles under MinGW, and its host tests run on Windows x86-64. The
four module glues (`shm/http/stream/bridge`) still require a full
OpenResty/nginx Windows build, which is documented separately in
`nginx-rtc-example/docs/windows-mingw-build-guide.md`.

To cross-compile the Windows test runner from Linux:

```sh
MINGW_PREFIX=$HOME/.local/mingw ./scripts/cross-win64-tests.sh
```

The result lands in `dist/win64/` (`run_tests.exe` plus the two MinGW runtime
DLLs it needs). `dist/` is git-ignored; regenerate it with the script above.

## License

MIT © 2026 DeguiLiu
