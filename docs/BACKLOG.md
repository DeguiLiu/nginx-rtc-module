# 未实现能力与已知缺陷

本文只列**当前做不到、或做错的事**，以及**已明确决定不做的事**。已落地的行为见
`ARCHITECTURE.md`。每条只写现状、影响、方向；不写提案论证与实施顺序。

判断口径（对照 SRS 6.0）：下行（播放）不弱于 SRS；未修项集中在 **WHIP 上行方向**。

## 1 上行（WHIP 发布者方向）

上行整体是单向接收，服务端不向推流端产生任何反馈。

- **不发 RR / XR** — 接收侧没有 RR 编码器，入站 SR、XR 被丢弃（`ngx_rtc_rtcp` 有 RR 解析但无动作）。
  **影响**：推流端拿不到丢包、抖动、RTT，libwebrtc 类拥塞控制在链路劣化时不降码率。
  **方向**：补 RR + XR RRTR 编码与定时器，用 DLRR 反算 RTT。
- **transport-cc 协商了不实现** — recvonly 应答回显 transport-cc extmap，但
  `ngx_rtc_rtp_strip_header_ext()` 把入站头扩展整个剥掉（含 TWCC 序号），也没有 TWCC 反馈编码器。
  **影响**：推流端按协商打戳却永远收不到反馈，协商形同虚设。
  **方向**：二选一——实现 TWCC 反馈，或在 recvonly 应答里不再回显该 extmap。
- **不发 NACK** — `ngx_rtc_rtcp` 只有 NACK 解析（`ngx_rtc_rtcp_nack_expand`），无 nack-pkt-list
  生成器；上行 jitter 以 `NGX_RTC_JITTER_TIMEOUT_MS` 超时跳过缺包。**方向**：需与推流端 RTX 流配合。

上行 PLI **已实现**（见 `ARCHITECTURE.md`）。

## 2 连接与安全

- **DTLS close_notify 被静默忽略** — `ngx_rtc_dtls.c` 把 `SSL_ERROR_ZERO_RETURN` 当良性返回，
  没有 `SSL_CB_ALERT` 处理。
  **影响**：浏览器关闭连接后，会话要等 reaper 超时（ready 30s / handshake 10s）才回收，
  期间仍向死对端发媒体并占用环与 shm 骨架。**方向**：加 info callback，收到 close_notify 即关闭会话。
- **STUN 先改状态、后验签** — `ngx_rtc_stream_module.c` 在 MESSAGE-INTEGRITY 校验**之前**就做
  attach / owner 绑定与 session 分配。
  **影响**：知道 server ufrag 的攻击者用错误 pwd 的伪造 STUN 即可改动跨 worker 骨架，
  把合法客户端挡在 `NGX_DECLINED` 之外。**方向**：把校验提前到任何状态变更之前。
- **客户端地址迁移不支持** — 会话绑定首个对端地址，不维护地址集合。
  **影响**：WiFi 切 4G 或 NAT 重绑定后媒体中断，只能等超时回收。**方向**：按 ufrag 跟随源地址迁移。
- **传输层只有 UDP** — 无 ICE-TCP。**影响**：UDP 被封的网络无法接入。

## 3 媒体与时间轴

- **音频时间轴自由运行** — 音频 RTP 时间戳只在首帧从 RTMP 时钟播种，之后固定 +960 自由计数；
  且 RTCP SR 只对视频 SSRC 发送。
  **影响**：接收端无法把音频映射到与视频共同的 NTP 轴，长连接下渐进唇音不同步。
- **`ngx_rtc_avsync` 无调用者** — 算法（SR 锚点、晚到 SR 不回退参考点）已实现并单测覆盖，
  但没有任何运行时路径调用它；bridge 只在日志里自行算 skew。**方向**：接进 RTC→RTMP 方向时接线。
- **H264 选型不看 fmtp** — `ngx_rtc_sdp_build_offer` 取第一个 H264 rtpmap 而不比较其 fmtp；
  answer 又用 SPS 的 profile-level-id 覆盖并写死 `packetization-mode=1`。
  **影响**：浏览器列多个 H264 profile 时，会对 `pm=0` 的 PT 宣称 pm=1。
- **多 m-line / 缺 codec 会产出非法 answer** — offer 解析只保留单个 video/audio 槽位，
  被拒 m-line（端口 0）仍答为 active；超过 4 条 m-line 直接解析失败。
  默认浏览器 offer 恰好 1 video + 1 audio + application，不触发。
- **Opus fmtp 不按 offer 协商** — answer 写死 `minptime=10;useinbandfec=1` 与 `48000/2`，
  且编码器从未调用 `OPUS_SET_INBAND_FEC` / `OPUS_SET_DTX`。**影响**：宣称的 inband FEC 实际不产生。
- **音频转码固定延迟与静默丢帧** — 转码在 pthread + 定长环里做，比内联转码多约一个音频 tag 的延迟；
  环满即丢帧，只体现在 `adrop_in` 计数。

## 4 容量与扇出

- **扇出静默截断（两级）** — 同一份 RTP 投给 N 个观众，两级定长容量都会静默截断：
  快照按订阅顺序取前 `NGX_RTC_SOURCE_MAX_SNAPSHOT` 个，同一目标 worker 上再截到
  `NGX_RTC_RING_MAX_SESSIONS` 个。
  **影响**：被截掉的观众永久收不到，且日志无任何线索；症状是"固定几个人永远黑屏"。
  **方向**：截断计数透出到 stats，并把两级容量约束写成显式的配置关系。
- **两级容量无约束关系** — 快照上限与每 worker 环容量各自独立，配置上不保证前者 ≥ 后者。

## 5 锁与内存

- **重传环与注册表共用 slab 全局锁** — 只要存在一个跨 worker 观众，之后每个视频包都在
  `pool->mutex` 内 memcpy。该锁是所有 worker 的注册表锁，**同时是该环的生命周期保证**
  （回收路径在同一把锁内释放环）。
  **影响**：跨 worker 场景下媒体热路径与 slab 分配互相争用。
  **方向**：按 nginx 惯例做单写者无锁发布（版本号发布，环只有一个写入者进程），
  **而不是再加一把锁**——换锁只是把争用换个地方，还会破坏上面的生命周期契约。
- **回放每次 `ngx_alloc` 一块编译期常量大小的缓冲** — 约 1.5 MB，在锁外分配、函数内释放。
  **判定不做**：回放非热路径（PLI 或观众加入触发），复用一个跨调用共享的静态缓冲反而会在
  回调重入时被覆写，收益与风险不成比例。
- **GOP 环与 shm 环双份预留** — 同 worker 走进程内 GOP 环，跨 worker 走 shm 重传环，两者内容重叠。
  **方向**：先测出两侧实际占用再决定降哪个；共享内存是全局稀缺资源，进程内存不是。

## 6 结构性设计工作（未开工）

- **统一桥接抽象** — `ngx_rtmp_rtc_bridge_module` 只做 RTMP→RTC 单向，WHIP 上行的转发逻辑硬编码在
  `ngx_rtc_stream_module`。**方向**：不先建接口；等 RTC→RTMP 录制/转推开工时，在改造 bridge 的
  同期抽出最小桥接接口。
- **进程内注册表与 shm 注册表两份真相** — 同一份会话/源状态在两个注册表里各存一份，
  靠六套机制维持一致：`expires` 到期、`publisher_seen_ms` 心跳、
  `ngx_rtc_shm_source_referenced_locked()` 引用校验、`owner_slot == -1` 半开容忍、
  `ngx_rtc_session_fsm_restore()` 状态回填、`layout` 标签。
  **影响**：任何一处漏改都表现为跨 worker 的边缘 bug。
  **方向**：分层收敛（先统一生命周期，再统一状态表达），分批做，不与其它改动混。

## 7 已明确不做（与 SRS 同级或有意的取舍）

无 DTLS HelloVerify cookie、无 SRTP rekey（RFC 3711 §9.1）、无 ICE restart / renomination、
无 RTX、无 AV1/H265、无 MP3 转码、只发一条 host candidate。

以下方面**强于 SRS 6.0**，不是缺口：DTLS 重传（SRS 服务端 `start_arq()` 是空实现）、
STUN MESSAGE-INTEGRITY 校验（SRS 服务端不验）、入站 RTCP BYE 处理、
pacer/BWE（SRS 6.0 的 RTC 路径没有 pacer）、对播放端发 SR（SRS 不发）。

## 8 测试覆盖缺口

- **弱网机制无 e2e 覆盖** — `ngx_rtc_jitter` 的乱序重排、`drop_until_gop` 整 GOP 丢弃、
  NACK 响应退避、TWCC pacer 的 AIMD 收敛，全部只有 host 单测。e2e 脚本与推流客户端都不制造
  丢包、乱序或抖动（全仓无 netem/tc 注入）。**方向**：给推流客户端加丢包/乱序注入能力。
- **跨协议发布接管未跑过 e2e** — 三种接管场景（RTMP→WHIP、WHIP→RTMP、同协议重推）
  没有对应的 e2e 断言。
