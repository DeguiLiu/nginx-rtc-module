# 双 registry 收敛方案

## 结论

本仓同时维护两套源/会话结构 —— 进程内的 `ngx_rtc_source_t` / `ngx_rtc_session_t`，和共享内存里的 `ngx_rtc_shm_source_t` / `ngx_rtc_shm_session_t`。**字段大面积重叠**，为此已经长出五套一致性机制，而每加一个功能都要在两处各加一遍。

本文的结论是：**不做全合并，做分层收敛** —— 把共享内存那套定为「跨 worker 元数据」的唯一真相，进程内那套不再镜像这些字段，改为按需读。OpenSSL 上下文、连接 fd、pacer、jitter buffer 这些**物理上不能进 shm** 的部分留在进程内，边界画清楚。

这项工作**不应顺手做**，需要独立分批与独立验收。

---

## 1 现状：重叠到什么程度

### 1.1 会话层

`ngx_rtc_shm_session_s`（`src/ngx_rtc_shm.h`）与进程内 `ngx_rtc_session_t`（`src/ngx_rtc_core.h:388` 附近）都有：

| 字段 | shm 骨架 | 进程内 | 谁真的需要 |
| --- | --- | --- | --- |
| `video_pt` / `audio_pt` | 有 | 有 | shm：非属主 worker 拼包；进程内：发送时用 |
| `twcc_video_ext` / `twcc_audio_ext` | 有 | 有 | 同上 |
| `publishing` | 有 | 有 | shm：reaper 判定；进程内：分支 |
| `srtp_ready` | `ngx_atomic_t` | FSM 状态 | 两者语义不同（见 §2.3） |
| `owner_slot` | 有 | 无 | 只有 shm 有，正确 |
| `ice_ufrag` / `ice_pwd` | 有 | 有 | shm：重建会话用；进程内：握手用 |
| `twcc_lost` / `twcc_received` / `pacer_bps` / `drop_pacer` / `drop_gop` | 有（原子） | 有 | **纯属重复** —— 这些是进程内产生的，shm 只是为了让 stats 能读 |

最后一行是问题的核心：**这些统计值本来产生于进程内，为了能让 Lua 从任意 worker 读到，被复制进 shm**。

### 1.2 源层

`ngx_rtc_shm_source_s`（`shm.h:81-124`）有 name、publishing、publisher_kind、expires、publisher_seen_ms、publisher_slot、ssrc/pt、seq/ts、包数/字节数、SPS/PPS/ASC、subscribers_version、subscribers 队列、remote_subscribers。

进程内 `ngx_rtc_source_t` 有 name、GOP 环、snap_* 快照、进程内会话列表、`shm_src` 指针（`core.h:397-401`）。

这里重叠较少 —— 源层基本是**职责分离**的，进程内管媒体环，shm 管跨 worker 元数据。问题集中在会话层。

---

## 2 为维持一致而长出的机制

每一条都是补丁，且都在给这个结构还债：

### 2.1 `expires` 宽限期（`shm.h:87`）

源被判定过期后不能立即释放，因为别的 worker 可能还持有它的指针。于是引入「宽限期」—— 一个源在无人引用后还要活一段时间。

### 2.2 `publisher_seen_ms` 心跳（`shm.h:93`）

`publishing` 标志原本表示「有属主」。但 worker 崩溃不会执行释放，标志永远为真，源再也回收不掉。于是加了一个每包心跳，让 reaper 能区分「死了的发布者」和「闲置的发布者」。

### 2.3 `referenced_locked()` 拒释放（`shm.c:990`）

`remove()` 曾经只看 `subscribers` / `publishing`，而一个还没订阅的会话（WHIP 发布者**永远不会**订阅自己）会让源在仍被 `sess->source` 指向时被释放。于是加了引用检查。

### 2.4 `owner_slot` 归属校验（`shm.c` 的 `set_state`）

状态由属主 worker 写。但单 worker 场景下绑定事件落在创建会话的进程里，`bind()` 从不运行，`owner_slot` 一直是 -1。于是校验必须放宽成「-1 时允许写」，否则最值得诊断的状态全被丢弃。

### 2.5 FSM 状态恢复（`ngx_rtc_session_fsm_restore`）

一个通过 `attach_from_shm` 重建会话的 worker 从未执行过 DTLS 握手，没有任何事件序列能把它带到 `SRTP_READY`。于是引入一个「不跑转移、直接采纳状态」的旁路 API。

### 2.6 还有

- `subscribers_version` + 快照缓存（`shm.h:121`、`bridge:1108`）
- `NGX_RTC_SHM_LAYOUT` 布局标记（`shm.h:243`）—— 一个 zone 活过 reload，新二进制必须能拒绝旧布局
- `attach_from_shm` 整条重建路径

**这六条没有一条是设计出来的，都是被 bug 逼出来的。** 它们本身都正确，但每一条都在增加「两份真相之间保持一致」的成本，而这个成本随字段数增长。

---

## 3 根因

进程内 registry 先存在，多 worker 支持是后加的。shm 骨架是为了让「非属主 worker 也能读到会话元数据」而**复制**了一份，而不是把进程内那份**移出去**。

复制带来的一致性成本，被记在了调用方头上，而不是结构上。

---

## 4 方案对比

### 方案 A：维持现状，继续打补丁

- 成本：低（每次改动只针对当前需求）
- 代价：一致性机制继续增长；每个新统计字段都要在两处加、两处同步
- 适用：功能冻结期

### 方案 B：shm 为唯一真相，进程内不再镜像

进程内结构**删掉**与 shm 重叠的字段，需要时读 `shm_sess->`。

- 收益：重叠消失，一致性机制随之失去存在理由
- 代价：热路径多一次 shm 读（跨 worker 可见的缓存行，比进程内慢）；`srtp_ready` 这类**语义不同的同名字段**需要逐个澄清
- 风险：中 —— 触碰会话创建/销毁/发送三条主路径

### 方案 C：进程内为唯一真相，shm 只留最小集

把 `twcc_lost` / `pacer_bps` / `drop_*` 这类「进程内产生、只是为了让 stats 读到」的字段从 shm 撤掉，改为 stats 时**主动向属主 worker 拉取**。

- 收益：shm 占用下降；重复消失
- 代价：需要一套跨 worker 的查询机制（现在没有），比复制字段复杂得多
- 结论：**不划算**

### 方案 D（推荐）：分层收敛

不追求消灭重复，而是**给每个字段定一个唯一属主，并禁止镜像**：

1. **shm 属主**：跨 worker 必须可见的。`owner_slot`、`expires`、`state`、`close_requested`、`subscribers_version`、`publisher_seen_ms`。这些只有 shm 有，保持。
2. **进程内属主**：只有本 worker 用得到的。OpenSSL 上下文、连接 fd、pacer、jitter buffer、GOP 环。这些**不该出现在 shm**，保持。
3. **交界字段**：`twcc_lost` / `pacer_bps` / `drop_*` 这类 —— **保留 shm 里的那份为唯一写点**，进程内不再各存一份，需要时读 shm。

第 3 类是唯一要动的，且它是最安全的一类：这些字段只被 stats 读，不在热路径的判定里。

---

## 5 分批路径

```
第 1 批  交界字段去重（方案 D 第 3 类）
         twcc_lost / twcc_received / pacer_bps / drop_pacer / drop_gop
         进程内删字段，写入点改为写 shm_sess
         验收：stats 输出逐字节不变（有现成的 test_shm / test_stream_module 可扩展）

第 2 批  srtp_ready 与 FSM state 的语义澄清
         现在 srtp_ready 是「已就绪」的布尔，state 是「生命周期阶段」的枚举，
         两者可以互相推导。定一个为准，另一个改为派生。
         动之前必须先补测试 —— 这是 §2.3 / §2.5 两条补丁的根源

第 3 批  ice_ufrag / ice_pwd 去重
         这两者只在握手期用，握手完成后进程内可以不再持有

第 4 批（可选）源层
         源层已经基本分层，除非有具体收益，不动
```

**每一批都必须是可独立回滚的**，且每批之后 `make -C test test` + sanitize 五组全绿。

---

## 6 验收与风险

### 6.1 验收

- 每批都先写 RED。第 1 批的 RED 很直接：改进程内字段的写入点后，断言 stats 输出的 `pacer_bps` 仍然变化
- 用 ASan/LSan 跑 sanitize 五组 —— 这类改动最容易引入的是生命周期错误
- 跨 worker 场景必须 e2e 验证（host 测试覆盖不到多进程）

### 6.2 风险

| 风险 | 缓解 |
| --- | --- |
| 触碰会话创建/销毁路径导致 UAF | 分批，每批只动一个字段组；ASan 全程 |
| shm 读在热路径上变慢 | 第 1 批的字段都不在热路径；若有例外，先测再动 |
| 布局变化导致 reload 读到旧 zone | **每次改 `ngx_rtc_shm_session_s` 的尺寸都必须 bump `NGX_RTC_SHM_LAYOUT`**（`shm.h:243`）。这个标记的存在意义就是让这类错误响亮失败而非静默 |
| 改到一半停下，两份真相更不一致 | 每批独立可回滚；不要跨批提交 |

### 6.3 什么情况下不要做

如果近期有其它大改动落在会话生命周期上（例如扇出截断方案里的分片逻辑），**先做完那个再做本文的工作** —— 两者都会动 `ngx_rtc_shm_session_t` 的邻近代码，并行会互相踩。
