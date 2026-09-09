# nginx-rtc-module 架构设计

## 数据流

```text
RTMP/WHIP 推流
      │
      ▼
ngx_rtmp_rtc_bridge_module / ngx_rtc_stream_module(WHIP 上行)
      │  解析 H264/AAC → 封 RTP / SRTP 解密
      ▼
ngx_rtc_core_module(source/session 注册表, rtc_zone 共享内存)
      │  明文 RTP 广播 + GOP 关键帧快照
      ▼
ngx_rtc_stream_module(owner worker 出队 → SRTP → UDP)
      │
      ▼
浏览器 WebRTC 播放
```

## 四个模块

| 模块 | 职责 |
|---|---|
| `ngx_rtc_core_module` | `rtc_zone` 共享内存注册表（source/session）、跨 worker 媒体环、GOP 快照 |
| `ngx_rtmp_rtc_bridge_module` | RTMP 音视频 → RTP 封包 → 广播 |
| `ngx_rtc_http_module` | WHIP / RTC 信令（SDP offer/answer）、统计透出 |
| `ngx_rtc_stream_module` | UDP 媒体面：STUN/DTLS/SRTP、上行 SRTP 接收、下行 SRTP 发送 |

## 关键设计

- **实现与发行分离**：本仓库只含自研 C 代码与 host 单测，部署/脚本/文档在
  `nginx-rtc-example`。
- **第三方库定位契约**：`config` 只读环境变量 `NGX_RTC_THIRD` 指向
  `include/` + `lib/`，不硬编码路径。
- **多 worker 数据面**：明文 RTP 通过 `rtc_zone` 的 per-worker 媒体环 + eventfd
  跨进程投递，owner worker 做 SRTP 加密后发送。
- **重传缓存下沉 shm**：每 source 一个固定窗口视频重传环写入共享内存，任意 worker
  订阅可回放最新 GOP、NACK/PLI 按 seq 命中；环槽统一在 slab pool 锁下读写/释放，append
  按「有无跨 worker viewer」门控（无跨 worker viewer 时同 worker 单 memcpy）。同 worker
  直接读进程内 GOP 环，无锁。
- **音频线程隔离**：AAC→Opus 转码在独立 pthread 中执行，避免阻塞 nginx 事件循环。

详细设计见发行仓 `nginx-rtc-example/docs/`。
