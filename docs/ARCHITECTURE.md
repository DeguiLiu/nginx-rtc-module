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
      │  明文 RTP 广播 + GOP 环缓存
      ▼
ngx_rtc_stream_module(owner worker 出队 → SRTP → UDP)
      │
      ▼
浏览器 WebRTC 播放
```

## 四个模块

| 模块 | 职责 |
|---|---|
| `ngx_rtc_core_module` | `rtc_zone` 共享内存注册表（source/session）、跨 worker 媒体环、每 source 重传环；模块身份、`rtc_zone` 指令与 worker 崩溃回溯在 `ngx_rtc_core_module.c`，纯数据结构注册表在 `ngx_rtc_shm.c` |
| `ngx_rtmp_rtc_bridge_module` | RTMP 音视频 → RTP 封包 → 广播 |
| `ngx_rtc_http_module` | WHIP / RTC 信令（SDP offer/answer）、统计透出 |
| `ngx_rtc_stream_module` | UDP 媒体面：STUN/DTLS/SRTP、上行 SRTP 接收、下行 SRTP 发送 |

## 关键设计

- **实现与发行分离**：本仓库只含自研 C 代码与 host 单测，部署/脚本/文档在
  `nginx-rtc-example`。
- **第三方库定位契约**：`config` 只读环境变量 `NGX_RTC_THIRD` 指向
  `include/` + `lib/`，不硬编码路径。
- **多 worker 数据面**：明文 RTP 通过 `rtc_zone` 的 per-worker 媒体环 + eventfd
  跨进程投递，owner worker 做 SRTP 加密后发送。zone 根带 `layout` 标记（`NGX_RTC_SHM_LAYOUT`），
  reload 与 master 重启两条复用路径都校验；不匹配时 `NGX_LOG_EMERG` 并返回 `NGX_ERROR`，
  避免按新结构去读旧内存。
- **重传缓存下沉 shm**：每 source 一个固定窗口视频重传环写入共享内存，任意 worker
  订阅可回放最新 GOP、NACK/PLI 按 seq 命中；环槽统一在 slab pool 锁下读写/释放，append
  按「有无跨 worker viewer」门控（无跨 worker viewer 时同 worker 单 memcpy）。同 worker
  直接读进程内 GOP 环，无锁。发布 worker 每包刷新 `publisher_seen_ms` 心跳，reaper 超过
  `NGX_RTC_SHM_PUBLISH_GRACE_MS` 仍未见心跳即判定发布者已死，回收 source 及其重传环；
  释放前再校验没有 session 骨架仍指向该 source。
- **音频线程隔离**：AAC→Opus 转码在独立 pthread 中执行，避免阻塞 nginx 事件循环。
- **会话状态机**：`ngx_rtc_session_fsm` 跟踪
  NEW → ICE_BOUND → DTLS_HANDSHAKE → SRTP_READY → CLOSED，只做状态判定、无副作用；
  close 类事件挂在根状态上，从任意状态收敛。绑定与 DTLS 起始转移带 guard（校验事件上下文里的
  连接与 DTLS 记录），未处理事件经 `ngx_rtc_session_fsm_set_reporter` 以 debug 上报，避免静默停滞。
  跨 worker 时该状态机每进程一份，信令 worker 的副本会停在 NEW 直到被回收，因此**全局就绪应以
  shm 会话骨架的 `srtp_ready` 为准**，进程内 `ngx_rtc_session_fsm_is_ready()` 只回答本进程。
  shm 会话骨架另带一个 `state` 字节（`ngx_rtc_session_state_t`，仅拥有该 UDP 会话的 worker 可写），
  把 NEW/ICE_BOUND/DTLS_HANDSHAKE/SRTP_READY 透出到 `/rtc/v1/stats`；CLOSED 从不写入，关闭即释放骨架。
  统计按 `session_list` 遍历再按 source 归类，因此未完成握手的会话与 WHIP 发布会话都在
  `streams[].sessions[]` 中可见。
- **会话状态机的取舍与结构约束**：该 FSM 的定位是「闸门」而非「驱动」——转移表 `action` 全为
  `NULL`，只判定状态、不执行动作，真正持有连接与 DTLS/SRTP 上下文的 `ngx_rtc_stream_module.c`
  才是执行者；这是刻意设计，避免状态机里存指针导致生命周期与连接脱钩。守卫承载真实判定责任
  （如 DTLS 起始转移要求长度与类型都像 DTLS 记录，补足 UDP 分派只看首字节的启发式）；
  CLOSE/TIMEOUT/RTCP_BYE 挂在根状态收敛，CLOSED 再吞掉这三个事件。两项结构约束需留意：
  `NGX_RTC_SESSION_FSM_MAX_DEPTH`（当前 2）与状态表层级人工耦合，新增第三层状态必须同步该常量
  （HSM 入口缓冲不足已由 `assert` 中止改为返回 `false` 并上报未处理事件，不再中止 worker）；
  SRTP_READY 的转移表未含 `DTLS_PACKET`，就绪后的 DTLS 报文会被判为未处理事件，当前无害
  （DTLS 数据在传输层处理），但不应依赖状态机判定「是否接收 DTLS 包」。
- **不建议上状态机的地方**：DTLS `handshake_done`、avsync、jitter 初始化标志与各分量标志位都不
  合并成枚举——它们或是 2 状态、或只有单一含义、或位于每包热路径，上状态机属过度设计。源发布权
  （`publisher_kind`/`publishing`）虽是真正的状态机，但也不使用 `ngx_rtc_hsm`：它是「两个副本 +
  一把锁」的原子更新，用普通转移函数 `ngx_rtc_publish_claim/release/mirror` 即可。shm 会话骨架的
  4 个字段正交编码同样暂不枚举化，等真的出现非法组合缺陷再改；记录一处不一致备查：`owner_slot`
  与 `publishing` 是裸整数（靠 `pool->mutex` 保护），而 `srtp_ready`/`close_requested`/`expires`/
  `twcc_*` 是 `ngx_atomic_t`（靠原子性），两种同步原语混在同一结构里。
- **尚未实现的 SRS 差距项**：以下来自 `docs/archive/architecture-review-vs-srs.md`（对照 SRS 6.0
  的架构差距分析），均为评审时标注未动/未开工的开放项，已落地项不在此列。
  - **统一桥接抽象（P2-2，未开工）**：`ngx_rtmp_rtc_bridge_module` 只做 RTMP→RTC 单向，WHIP
    上行的转发逻辑硬编码在 `ngx_rtc_stream_module`；按反过度设计原则不先建接口，等 RTC→RTMP
    录制/转推真正开工时，在改造 bridge 的同期抽出最小桥接接口。
  - **上行 RTCP NACK 生成（未立项）**：`ngx_rtc_rtcp` 只有 NACK 解析
    （`ngx_rtc_rtcp_nack_expand`），没有 nack-pkt-list 生成器；WHIP 上行 jitter 以
    `NGX_RTC_JITTER_TIMEOUT_MS=50ms` 超时跳过缺包，未向推流端发 NACK。发送侧 NACK 响应退避已
    落地，接收侧 NACK 生成暂不立项。

详细设计见发行仓 `nginx-rtc-example/docs/`。
