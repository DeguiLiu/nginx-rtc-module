# nginx-rtc-module

自研的 nginx C 扩展（addon），把 **RTMP / WHIP** 推流转换成 **WebRTC**
（RTP/SRTP over UDP）播放，运行于 OpenResty/nginx。装配与部署示例仓库见
[github.com/DeguiLiu/nginx-rtc-example](https://github.com/DeguiLiu/nginx-rtc-example)
（SDP 信令、Lua 鉴权、播放页、HTTP-FLV/HLS 输出、运行脚本）。

一次 `--add-module` 从本仓库注册 4 个 nginx 模块：

| 模块 | 类型 | 职责 |
|---|---|---|
| `ngx_rtc_core_module` | core | `rtc_zone` 共享内存注册表 + 定时器 |
| `ngx_rtmp_rtc_bridge_module` | core (rtmp) | RTMP 直播流 → RTC source 桥接 |
| `ngx_rtc_http_module` | http | WHIP / RTC 信令（`/rtc/v1/play/`）+ 统计 |
| `ngx_rtc_stream_module` | stream | UDP 媒体面（SRTP/SRTCP、ICE/STUN、DTLS 完成回调） |

## 目录结构

- `config` — nginx addon 构建脚本（通过 `NGX_RTC_THIRD` 定位第三方静态库）。
- `src/` — 纯 C 核心（`rtp/sdp/stun/dtls/srtp/ring/audio/rtcp/hsm/session_fsm`，
  不依赖 nginx 头文件，可独立单测）+ 四个模块胶水代码。
- `test/` — host 单测。两套头世界：已 configure 的 nginx 树（Linux 默认）或
  `test/include/` 的 stub（Windows）；部分用例链接真实的 OpenSSL/libsrtp2/FFmpeg。
- `docs/` — [ARCHITECTURE.md](docs/ARCHITECTURE.md)（当前架构）、
  [BACKLOG.md](docs/BACKLOG.md)（未实现能力与已知缺陷）、
  [BUILD-TEST.md](docs/BUILD-TEST.md)、[nginx-coding-standards.md](docs/nginx-coding-standards.md)。

## 构建

模块链接第三方静态库（libavcodec/libswresample/libavutil/libopus/libsrtp2）。
通过环境变量指定其 `include/` + `lib/`：

```sh
export NGX_RTC_THIRD=/path/to/third          # include/ + lib/（见 nginx-rtc-example 的 scripts/build-deps.sh）
./configure ... --add-module=/path/to/nginx-rtc-module
```

host 单测不需要这些：

```sh
make -C test test            # 编译 + 运行；加 NGX_SRC= 强制走 stub 头世界
```

## License

MIT © 2026 DeguiLiu
