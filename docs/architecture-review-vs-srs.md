# 参考 SRS 的架构改进建议

> 文档范围：以 SRS 6.0（`references/srs-server-6.0-r1`）为参照，对
> `ngx-rtc-module` 当前数据面架构做差距分析，给出改进建议与理由。结论先行，
> 分级标注优先级，避免过度设计。

## 1 结论

协议核心（RTP/SDP/STUN/DTLS/SRTP/RTCP）与 SRS 对齐度已经较高，`publisher_kind`
双发布防护、源超时回收、16-bit 序号翻转（`(uint16_t)(seq - start_seq)` 模距离）
均已正确落地。**真正值得借鉴的不是协议层，而是「慢消费者整 GOP 丢弃」与
「上行 jitter buffer」这两个数据面架构**，它们直接决定弱网下的可看性，也是当前
相对 SRS 最薄的两块。

## 2 评审范围与方法

对照对象为 SRS 6.0 的 `srs_app_rtc_source`、`srs_app_rtc_queue`、
`srs_app_stream_bridge`、`srs_kernel_rtc_rtp`，以及本仓库的
`ngx_rtc_core`、`ngx_rtc_ring`、`ngx_rtc_shm`、`ngx_rtc_stream_module`、
`ngx_rtmp_rtc_bridge_module`。只比较数据面架构，不比较进程模型（SRS 是
协程 + 独立服务，我们是 nginx 事件循环 + shm，范式不同不可照搬）。
音频转码通路（`ngx_rtc_audio_worker` 独立线程 + vring）不在本评审范围，
其「转码 vs 透传」的取舍需要单独对比。

进程/线程协程模型、锁分层与内存预算另见
`docs/srs-memory-scheduling-optimization.md`（本文 §2 排除的部分在该文中展开）。

## 3 建议总表

| 优先级 | 建议 | SRS 参照 | 现状缺口 | 主要收益 |
| --- | --- | --- | --- | --- |
| P1 | 慢消费者隔离 + 整 GOP 丢弃 | `SrsMessageQueue` / `SrsRtcConsumer` | session 同步广播，socket 满单包丢弃 | 弱网下解码连续性 |
| P1 | WHIP 上行 jitter buffer / 乱序重排 | `SrsRtcFrameBuilder` | SRTP 解密后直接广播，无重排 | 公网 WHIP 推流可用性 |
| P2 | 发送侧 NACK 响应退避 | `SrsNackOption`（仅退避思路） | 固定 100ms 窗口，NACK 风暴反复重传 | 收敛重传风暴 |
| P2 | 桥接接口（方向记录，开工时抽） | `ISrsStreamBridge` | 桥接单向且逻辑散落 | 双向桥接扩展性 |

## 4 逐条建议与理由

### 4.1 P1 慢消费者隔离 + 整 GOP 丢弃

> **本策略仅适用于视频。** 音频通路（`ngx_rtc_audio_worker` 线程转码 + vring）
> 严禁主动丢帧：AAC→Opus 转码器要求输入严格连续，任何主动丢帧（丢旧/丢新）都会
> 破坏解码器与重采样稳态。音频慢消费者只能由 vring 字节容量自然背压丢弃，不得套用
> 本文的整 GOP 丢弃。

**SRS 做法**：注意区分两个域。`SrsMessageQueue`（`srs_app_source.hpp:106`）按时间
限长、满时丢整 GOP，但那是 **RTMP 域**的机制；RTC 的 `SrsRtcConsumer` 是裸
`std::vector<SrsRtpPacket*>`，头文件注释明写 `We do not drop packet here, but drop
it in sender`（`srs_app_rtc_source.hpp:103`），即 SRS RTC 的丢弃策略在 **sender 侧**，
不在队列侧。

**现状**：我们同样把丢弃放在 sender 侧。`ngx_rtc_core.c:397-400` 的
`ngx_rtc_session_pacer_admit` 在 token bucket 超载时主动丢包（`return 0`），这是
已落地的 TWCC/REMB loss-based pacer（`fdc384d`）构成的速率闭环；UDP socket 满时
累计 `send_eagain`/`send_failed`。所以当前不是纯「单包丢弃、靠对端补」，已有主动
降速。缺口在于丢的是**单包**而非**整 GOP**，慢客户端会在一个 GOP 内反复跳帧。

**理由**：单包丢弃让慢消费者持续收到不完整 GOP，解码端反复跳帧。整 GOP 丢弃 + 追
下一个 IDR 能保证慢客户端「要么看完整一帧、要么等下一帧」。

**落点**：不引入 per-session 待发送队列（与 `video-retransmit-cache-design.md`
「消除每会话 1.5MB rtx 环」的主旨一致）。nginx UDP send 本就非阻塞，per-session 只需
一个「丢弃至下一 IDR 边界」标志位，配合现有 `is_gop_start` 与 `send_eagain` 计数，即可
在 socket 积压时整 GOP 丢弃、追下一个 IDR。注意区分两个概念：**重传缓存**（source
GOP 环 / shm 环）供 NACK/PLI 回放，**发送缓冲**是这里讨论的待发送侧，二者目的不同。

### 4.2 P1 WHIP 上行 jitter buffer / 乱序重排

**SRS 做法**：`SrsRtcFrameBuilder` 用 512 包缓存（`s_cache_size = 512`）缓存上行
RTP，按 seq 重排，配合 `find_next_lost_sn` 做丢包检测、`check_frame_complete` 校验
关键帧完整性，攒齐一个完整可解码帧再交给下游。

**现状**：`ngx_rtc_stream_on_srtp` 在 SRTP 解密后**直接调用 `broadcast_rtp`**，
到达序即广播序，无任何重排；RTP 乱序/丢包会直接导致 H264 解码花屏或不可解。

**理由**：局域网推流乱序概率低，但 WHIP 一旦上公网，RTP 乱序/丢包是常态。这是 WHIP
推流（P2 功能）「能用于生产」之前必须补的一块，而非可选项。

**落点**：为 WHIP 上行增加一个按 seq 索引的小型重排窗口（例如 64~128 包），攒齐
完整帧再广播。缺口处理优先走「超时丢弃不完整帧」（与 `SrsRtcFrameBuilder` 一致），
而非向推流端发 NACK——那需要同步实现 RTCP NACK 生成（当前 `ngx_rtc_rtcp` 只有 NACK
解析，无 nack-pkt-list 生成器）且依赖推流端 RTX 重传流配合。

### 4.3 P2 发送侧 NACK 响应退避

**SRS 做法**：`SrsNackOption`（`first_nack_interval`/`nack_interval`/
`max_nack_interval`/`max_alive_time`）是**接收方**的重传请求策略，属于 RTC 下行
播放器或 RTC2RTMP 桥接收端的机制。

**现状**：我们是纯**发送方**，重传节奏由 viewer 的 NACK 决定，服务端已有
`NGX_RTC_NACK_WINDOW_MS=100` + `NGX_RTC_NACK_BUDGET=128` 做去重限流。所谓「固定
频率重试放大拥塞」批评的是接收侧行为，当前并不存在；接收侧 NACK 生成当前不做。

**理由**：发送方真正的自我保护点是被 NACK 风暴时的响应预算——固定 100ms 窗口在
弱网下可能对同一批丢包反复重传，退避能收敛突发。

**落点**：把发送侧 NACK 响应预算随窗口退避（例如窗口从固定 100ms 随被 NACK 频次
逐步放宽，或对被反复请求的同一 seq 加大去重保留），作为被 NACK 风暴时的自我保护。
接收侧 NACK 生成暂不立项。

### 4.4 P2 统一桥接抽象

**SRS 做法**：`ISrsStreamBridge` / `SrsFrameToRtcBridge` 把 RTMP→RTC、RTC→RTMP、
RTMP→RTMP 转发收敛到同一接口，源之间通过 bridge 双向搬运。

**现状**：`ngx_rtmp_rtc_bridge_module` 只做 RTMP→RTC 单向；WHIP 上行的转发逻辑
硬编码在 `ngx_rtc_stream_module` 里，没有统一桥接层。

**理由**：一旦要做 WHIP 推流转 RTMP/HTTP-FLV 回放，转发逻辑会继续散落在两个模块。
但「先抽象接口再找实现」属于前瞻性抽象层，与项目「禁 helper/抽象层、最小改动」的
规则冲突。

**落点**：降级为方向记录，不单独立项。等 RTC→RTMP 录制/转推真正开工时，在改造
`ngx_rtmp_rtc_bridge_module` 的过程中同期抽出最小桥接接口，避免先建接口再等实现。

## 5 已对齐、不建议照搬

| 项 | 现状 | 结论 |
| --- | --- | --- |
| republish reset 代际隔离 | bridge 收到新 AVC sequence header 时 `gop.count/gop_start` 清零，shm `ngx_rtc_shm_retransmit_reset` 用 `head=0` 镜像 | 防重推流后旧代 seq 回绕误命中，测试 `ring_reset_serves_only_new_generation`（`test/test_rtc_core.c`） |
| 16-bit 序号翻转 | `ngx_rtc_rtp_ring_get` 用 `(uint16_t)(rtp_seq - start_seq)` 模距离 | 比 SRS `nn_seq_flip_backs` 简洁，无需改 |
| 源生命周期回收 | `expires` + `expire_locked` 回收 | 覆盖 SRS `stream_die_at` + `can_publish` 语义 |
| 双发布防护 | `publisher_kind`（shm ownership tag）+ `publisher_slot`（RTMP worker 归属）独占发布 | 覆盖 SRS `can_publish`，已修复同名跨协议 UAF |
| `SrsSharedPtr` 引用计数 / `srs_error_t` 错误链 | 我们用 nginx pool + shm slab + `NGX_OK/NGX_ERROR` | nginx 范式，照搬反破坏规范 |

## 6 实施顺序

1. P1-1 慢消费者整 GOP 丢弃（弱网质量核心，与现有 ring 出队同构）。
2. P1-2 WHIP 上行 jitter buffer（公网推流前提）。
3. P2-1 发送侧 NACK 响应退避（小改动、低风险）。
4. P2-2 桥接接口（RTC→RTMP 录制/转推开工时同期抽出，暂不单独立项）。
