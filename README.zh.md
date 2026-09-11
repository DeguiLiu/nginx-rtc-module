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
- `test/` — host 单测（不依赖 nginx / 第三方库）。

## 构建

模块链接第三方静态库（libavcodec/libswresample/libavutil/libopus/libsrtp2）。
通过环境变量指定其 `include/` + `lib/`：

```sh
export NGX_RTC_THIRD=/path/to/third          # include/ + lib/（见 nginx-rtc-example 的 scripts/build-deps.sh）
./configure ... --add-module=/path/to/nginx-rtc-module
```

host 单测不需要这些：

```sh
make -C test run_tests       # 87 个用例
```

## Windows 可移植性

**Windows 平台限制**：Windows 版 nginx 不支持 UDP（见
`nginx.org/en/docs/windows.html` 的 Known issues）。WebRTC 媒体面
（`ngx_rtc_stream_module`）依赖 `listen ... udp`，因此 WebRTC 播放无法在
Windows 上运行。Windows 构建仅支持纯 TCP 路径：RTMP 推流（1935）+
HTTP-FLV / HLS / DASH 播放（18082）。完整的 RTMP → WebRTC 低延迟播放必须在
Linux 上运行。

不依赖 nginx 的协议核心（`rtp/sdp/stun/rtcp/hsm/session_fsm/core/ring/avsync`）
可在 MinGW 下编译，其 host 单测可在 Windows x86-64 上运行。四个模块胶水代码
（`shm/http/stream/bridge`）仍需完整的 OpenResty/nginx Windows 构建，该路径
单独记录在 `nginx-rtc-example/docs/windows-mingw-build-guide.md`。

在 Linux 上交叉编译 Windows 测试可执行文件：

```sh
MINGW_PREFIX=$HOME/.local/mingw ./scripts/cross-win64-tests.sh
```

产物落在 `dist/win64/`（`run_tests.exe` 及所需的两个 MinGW 运行时 DLL）。
`dist/` 已被 git 忽略，用上述脚本重新生成。

## License

MIT © 2026 DeguiLiu
