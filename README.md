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
- `test/` — host unit tests. Two header worlds: a configured nginx tree (the
  Linux default) or the `test/include/` stubs (Windows). Some groups link the
  real OpenSSL/libsrtp2/FFmpeg.
- `docs/` — [ARCHITECTURE.md](docs/ARCHITECTURE.md) (how it works),
  [BACKLOG.md](docs/BACKLOG.md) (what it does not do yet),
  [BUILD-TEST.md](docs/BUILD-TEST.md), [nginx-coding-standards.md](docs/nginx-coding-standards.md).

## Build

The module links static third-party libs
(libavcodec/libswresample/libavutil/libopus/libsrtp2). Point the addon at their
`include/` + `lib/` by exporting the env var `config` reads:

```sh
export NGX_RTC_THIRD=/path/to/third          # include/ + lib/ (see nginx-rtc-example scripts/build-deps.sh)
./configure ... --add-module=/path/to/nginx-rtc-module
```

The host unit tests need no nginx build:

```sh
make -C test test        # build + run; NGX_SRC= forces the stub header world
```

## License

MIT © 2026 DeguiLiu
