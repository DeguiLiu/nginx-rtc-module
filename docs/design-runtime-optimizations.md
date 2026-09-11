# 运行时优化方案：扇出、回放与唤醒

## 结论

五项按「缺陷先于优化、小改动先于大改动」排序：

| 序 | 项 | 性质 | 改动面 | 风险 |
| --- | --- | --- | --- | --- |
| 1 | 扇出静默截断（两级） | **缺陷** | ~40 行 + 测试 | 低 |
| 2 | 两级容量无约束关系 | **缺陷** | 设计决策 + 常量 | 中（占 shm） |
| 3 | 每次回放分配 1.5 MB | 优化 | ~30 行 | 低 |
| 4 | eventfd 每包无条件唤醒 | 优化 | ~10 行 | 低 |
| 5 | GOP 环与 shm 环双份预留 | 优化 | 常量 + 评估 | 中 |

建议顺序 1 → 3 → 4 → 2 → 5。前四项互不依赖；第 2 项要先做决策（见 §2.4）；第 5 项需要先测出实际占用再动。

---

## 1 扇出静默截断

### 1.1 现状

同一份 RTP 要投给 N 个观众，代码里串了两级定长容量，**两级都是静默截断**：

**第一级** `ngx_rtc_shm.c:1038-1049`：

```c
    n = 0;
    for (q = ngx_queue_head(&src->subscribers);
         q != ngx_queue_sentinel(&src->subscribers) && n < max;   /* :1040 */
         q = ngx_queue_next(q)) {
        ...
        ids[n] = sess->id;  slots[n] = sess->owner_slot;  n++;
    }
```

`max` 由调用方传 `NGX_RTC_SOURCE_MAX_SNAPSHOT = 256`（`ngx_rtc_core.h:142`）。队列按**订阅先后**遍历，所以被截掉的是最后 256 个订阅者。

**第二级** `ngx_rtmp_rtc_bridge_module.c:1140-1146`：

```c
        nsess = 0;
        for (i = 0; i < n; i++) {
            if (src->snap_slots[i] == (ngx_int_t) w
                    && nsess < NGX_RTC_RING_MAX_SESSIONS) {   /* :1143, = 64 */
                sess_ids[nsess++] = src->snap_ids[i];
            }
        }
```

`NGX_RTC_RING_MAX_SESSIONS = 64`（`ngx_rtc_shm.h:61`）。同一个目标 worker 上超过 64 个就丢。

### 1.2 后果

不是"性能下降"，是**功能缺失**，且症状不可诊断：

- 被截掉的观众**永久**收不到。快照只在 `shm_src->subscribers_version` 变化时重算（`bridge:1108`），订阅集合不变就不会重算 —— 被截掉的人不在集合里，也无法通过重订阅把自己救回来。
- 日志里没有任何线索。`subscribers_version`、`snap_count` 都只反映"有多少个"，不反映"有多少个没进去"。
- 谁被截掉取决于订阅顺序，表现为"固定几个人永远黑屏"，而同样的人数、换一个订阅顺序可能就正常。

同时 `ngx_rtc_shm.c:1429` 的返回值校验路径（`nsess > NGX_RTC_RING_MAX_SESSIONS` 直接拒绝）说明这个常量在别处也被当成硬边界用，扩容时要一起看。

### 1.3 方案

**改动一：让截断可见。**

`ngx_rtc_shm_source_snapshot()` 的返回值当前只有两种含义（0 = 没找到，n = 拿到几个），无法区分"正好 256"和"还有更多"。改法：

```c
/* 返回值语义扩展：低位为 n，最高位表示被截断 */
```

更干净的做法是加一个出参，不动返回值：

```c
ngx_uint_t
ngx_rtc_shm_source_snapshot(ngx_rtc_shm_ctx_t *ctx, u_char *name, size_t len,
                            ngx_uint_t *ids, ngx_int_t *slots, ngx_uint_t max,
                            ngx_uint_t *truncated);   /* 新增 */
```

遍历完循环后再走一次队列判断是否还有剩余项（不要用 `n == max` 判断 —— 恰好 256 个订阅者时那是假阳性）：

```c
    if (q != ngx_queue_sentinel(&src->subscribers)) {
        *truncated = 1;
    }
```

**改动二：两级各计一次数、各告警一次。**

- `ngx_rtc_shm_source_t` 加 `ngx_uint_t fanout_dropped;`（累计被丢的订阅者数）
- `ngx_rtc_source_t`（进程内）加 `ngx_uint_t fanout_warned;`（记录已告警的 `subscribers_version`，避免每包刷屏）
- 告警用 `NGX_LOG_WARN`，内容带上 `snap_count`、`cap`、`dropped`、源名

告警去重的键必须是 `subscribers_version` 而不是时间 —— 时间窗口会在大流量下退化成刷屏，而版本号正好对应"订阅集合变了一次"，语义精确。

**改动三：进入 stats。**

`/rtc/v1/stats` 的每个 source 对象加 `fanout_dropped`。已有的 `ring_drops`（`bridge:1166`）只统计 enqueue 失败，不含这里的截断，两者不能混淆。

### 1.4 考虑过的替代方案

| 方案 | 为什么不做 |
| --- | --- |
| 快照数组改成动态扩容 | 定长数组在热路径上是刻意的（§4「热路径禁止每包分配」），扩容就要上堆 |
| 遍历时不设上限，拿到多少算多少 | 调用方的 `snap_ids[256]` 是定长的，越界写 |
| 被截断时直接返回错误、不发包 | 会把"部分观众黑屏"升级成"全部观众黑屏"，更糟 |

### 1.5 验收

先写 RED，跑出失败再改实现：

```c
/* 构造 max+1 个 srtp_ready 订阅者，调用 snapshot(max) */
NGX_RTC_TEST(shm_snapshot_reports_truncation_instead_of_hiding_it)
{
    /* 断言：返回 256；truncated == 1 */
}

/* 恰好 max 个不应报截断（假阳性检查） */
NGX_RTC_TEST(shm_snapshot_at_exactly_capacity_is_not_truncated)
{
    /* 断言：返回 256；truncated == 0 */
}
```

第二例是必须的：只用 `n == max` 判断截断会在这条上错。

---

## 2 两级容量互不约束

### 2.1 现状

```
NGX_RTC_SOURCE_MAX_SNAPSHOT  256   ngx_rtc_core.h:142   进程内快照数组
NGX_RTC_RING_MAX_SESSIONS     64   ngx_rtc_shm.h:61     shm 环条目内的 id 数组
NGX_RTC_RING_DEFAULT_SLOTS   512   ngx_rtc_shm.h:63     环槽数
```

三个数各自独立。单源 65 个观众落在同一个 worker 上时，第一级拿全了 65 个，第二级只放进去 64 个。

### 2.2 为什么简单调大 64 不划算

`ngx_rtc_ring_entry_t`（`ngx_rtc_shm.h:213-221`）是 shm 里的定长条目：

```
seq(8) + media(8) + gop(8) + len(8) + nsess(8) = 40
sess[64]      = 64 * 8                        = 512
rtp[1500]                                      = 1500
                                    合计约 2052 → 2056 对齐
```

每个环 `512` 槽 → 约 1.05 MB；每 worker 一个环，4 worker 就是 4.2 MB。把 64 提到 256，每条约 +1536 字节 → 每环 +0.79 MB → 4 worker 共 +3.1 MB。**观众数是长尾分布的**，为极少数 worker 上超过 64 个观众的场合，让所有部署常驻多占 3 MB shm，不划算。

### 2.3 方案

**一包多条目**：当一个目标 worker 上的会话数超过 `NGX_RTC_RING_MAX_SESSIONS` 时，切分成多个 entry 入队，而不是丢弃。

```c
        /* 现在：nsess < 64 就停 → 丢
         * 改为：每满 64 个就 enqueue 一次，继续下一批 */
```

这是在 `bridge:1140-1167` 外层再包一层循环。语义上安全：每个 entry 仍是一种"同包同 worker 的一组会话"，消费者（`ngx_rtc_shm_ring_dequeue`）逐条独立处理，每个会话仍**恰好收到一次**这个包。同包的多条 entry 相邻出队，会话侧看不出区别。

代价是超过 64 时占多个槽，但这是**按需付费**：只有真的超过才多占。

### 2.4 需要先定的决策

三个数之间的关系要写成明确约束，否则下一个改的人还会踩：

- `NGX_RTC_RING_MAX_SESSIONS` 是**每条的批次大小**，不是观众上限 —— 必须在 shm.h 的注释里写清楚，现在的注释（`:61` 只有一行裸定义）没有表达任何语义。
- `NGX_RTC_SOURCE_MAX_SNAPSHOT` 才是真正的单源观众上限（256）。
- 加编译期断言把关系固定下来：`NGX_RTC_RING_MAX_SESSIONS <= NGX_RTC_SOURCE_MAX_SNAPSHOT`，并用 `static_assert`（C11，本仓已用 C11）。

### 2.5 验收

```c
/* 单 worker 上 65 个会话：断言全部收到，且发生两次 enqueue */
NGX_RTC_TEST(fanout_splits_into_multiple_entries_when_over_batch_size)
```

这条测试要在 bridge 层做，而 bridge 不在 host 构建里（`test/Makefile` 的 `CORE_NAMES` 不含它）。两个选择：把分片逻辑抽成纯 C 函数放进可 host 测试的单元，或用 e2e 覆盖。**倾向前者** —— 分片是纯下标运算，没有 nginx 依赖。

---

## 3 每次回放分配 1.5 MB

### 3.1 现状

`ngx_rtc_shm.c:1280-1287`：

```c
    /* Allocate the replay buffer outside the lock: the ring capacity is a
     * compile-time constant, so the ~1.5 MB heap allocation never holds the slab
     * pool mutex. */
    buf = ngx_alloc((size_t) NGX_RTC_SHM_RETX_RING_CAP * sizeof(*buf),
                    ngx_cycle->log);
```

`NGX_RTC_SHM_RETX_RING_CAP = 1024`，`sizeof(slot) ≈ 1503 → 1504`，乘积约 1.54 MB。每次回放（`ngx_rtc_stream_module.c:1506` 的 GOP 重放路径）分配一次、在 `:1365` 释放。

注释本身是对的：分配放在锁外是有意的。问题不在这里，在于**这个缓冲的大小是编译期常量，内容却每 worker 只需要一份**。

### 3.2 后果

- 每次 PLI/replay 一次 1.5 MB 的 malloc/free。`ngx_alloc` 走的是系统分配器，1.5 MB 通常直接 mmap —— 每次触发一次 `mmap`/`munmap` 加页错误，是明确的抖动源。
- 有分配失败路径（`:1285` 返回 `NGX_ERROR`），在高负载下这条路径会真的走到。
- 分配失败时观众看到的是"GOP 重放没了"，与"重放成功但内容不对"在统计上无法区分。

### 3.3 方案

**结论：每个 worker 一份常驻缓冲，入口加重入检测。**

先排除不成立的候选：

| 持有者 | 容量需求 | 结论 |
| --- | --- | --- |
| 全局一份 | 1 份 | 不安全，多个 worker 并发 |
| 每源一份 | 源数 | 不安全，同源多会话可在同一 worker 内交错执行 |
| 每会话一份 | 会话数 | 安全，但总占用 = 会话数 × 1.5 MB，不可接受 |
| **每 worker 一份 + 重入检测** | 1 份 | 采用 |

它成立的前提是「同一 worker 内不会有两个回放同时使用这份缓冲」。回放是一段 `for` + `cb()` 的同步循环，`cb` 是 `ngx_rtc_session_send_rtp`，循环内没有让出点，所以同一 worker 的事件循环不会重入这段代码。

这个前提是**可验证的假设，不是显然事实**，所以实现时必须做两件事：

- 入口加 `in_use` 标志，命中时退回单次分配而不是复用。把假设变成运行时断言，而不是只写进注释。
- 复用次数计数进 stats，连续两次回放后该计数应为 1。

`ngx_rtc_stream_module.c` 的回放路径在会话事件回调里，nginx 单线程事件循环内不会嵌套回调，但**同一次事件里可以遍历多个会话**。所以真正要检查的是：回放函数内是否有让出点。当前实现是一段 `for` + `cb()` 同步循环，`cb` 是 `ngx_rtc_session_send_rtp`，不 yield。

结论：**每 worker 一份是安全的**，但必须在函数入口加一个"正在使用"标志并在有 flag 时退回栈内分配（或直接跳过），把假设写成断言而不是注释。

### 3.4 验收

- host 测试难覆盖（回放路径在 stream 模块），用 stats 判断：`retx_replay_count` 增长时 `ngx_alloc` 调用次数应为 0。
- 更实际的做法：加一个 `retx_replay_buf_reused` 计数，连续两次回放后该计数为 1。
- 用 `valgrind --tool=massif` 或 ASan 的分配钩子对比改动前后的堆行为。

---

## 4 eventfd 每包无条件唤醒

### 4.1 现状

`ngx_rtmp_rtc_bridge_module.c:1154-1167`：

```c
        if (NGX_OK == ngx_rtc_shm_ring_enqueue(shm->rings[w], ...)) {
            uint64_t one;
            ssize_t  rc;
            one = 1;
            if (-1 != shm->notify_fd[w]) {
                rc = write(shm->notify_fd[w], &one, sizeof(one));   /* :1162 */
                (void) rc;
            }
        } else {
            src->ring_drops++;
        }
```

每成功入队一个 entry，就 `write()` 一次目标 worker 的 eventfd。视频包按 30 fps 算，每包每 worker 一次 syscall。

### 4.2 方案

**只在"环由空转非空"时才唤醒。**

```c
        /* enqueue 之前，环是否为空 */
        if (NGX_OK == ngx_rtc_shm_ring_enqueue(...)) {
            if (was_empty) { write(...); }
        }
```

需要 `ngx_rtc_ring_entry_t` 的入队路径回报"入队前 head == tail"。`ngx_rtc_shm_ring_t`（`shm.h:227-235`）已有 `head`/`tail` 两个原子，读一次旧值即可，不需要加锁。

**为什么安全**：目标 worker 被唤醒后会一直排空到环空。`was_empty` 为真说明入队前环是空的，此时一定写 eventfd；为假说明环里已有数据，消费者醒来后必然看得到。两种情况都不漏唤醒，最坏是多一次写。

握手窗口：消费者把 tail 推到 head（排空）与进入 `epoll_wait` 之间，若生产者入队，环恰好由空转非空，`was_empty` 为真，正常唤醒。tail 只由消费者更新，生产者读到的旧值只会造成假阳性（多写一次），不会造成假阴性（漏唤醒）。

结论：实现时把这个握手写成注释，并加「唤醒次数 / 入队次数」比值统计来验证。

### 4.3 验收

stats 加 `ring_notify_write` 与 `ring_enqueue` 两个计数，稳态下前者应远小于后者（接近"每个 GOP 一次"而不是"每包一次"）。

---

## 5 GOP 环与 shm 环双份预留

### 5.1 现状

同一个视频包被存两遍：

| 环 | 位置 | 容量 | 用途 |
| --- | --- | --- | --- |
| GOP 环 | 进程内 `src->gop`（`core.h:388`） | `NGX_RTC_GOP_RING_CAP = 2048`（`core.h:164`，可配） | 同 worker 观众的 NACK / PLI |
| shm 重传环 | 共享内存 `src->retransmit`（`shm.h:71-77`） | `NGX_RTC_SHM_RETX_RING_CAP = 1024`（`shm.h:60`，不可配） | 跨 worker 观众的 NACK / PLI |

两份内容、两套容量、一套可配一套不可配。

### 5.2 评估（先测再改）

这一项**不建议先改**，理由：

1. 两个环的最坏情况占用没有实测数据。GOP 环 2048 × ~1.5 KB ≈ 3 MB/源 在进程内存；shm 环 1024 × 1.5 KB ≈ 1.5 MB/源 在共享内存。共享内存是**全局稀缺资源**，进程内存不是 —— 这个差异决定了"降哪个"。
2. 两者的容量差异（2048 vs 1024）可能是有意的：跨 worker 的重传走共享内存，成本更高，所以窗口更小。这条假设需要确认（查 `video-retransmit-cache-design.md` 与 `srs-memory-scheduling-optimization.md` §4.4）。
3. 合并两者会打破分层 —— shm 环的存在恰恰是为了让跨 worker 路径不依赖进程内状态。

**要做的第一步是测量**，不是改代码：

- 在 stats 里为两个环各加 `used_slots` 与 `cap`
- 采集真实流（一个源、若干观众、跑一段有丢包的场景）下的实际峰值占用
- 再看 `GOP_RING_CAP` 的默认 2048 是否远高于峰值

如果实测峰值远低于容量，**降低默认值**就是收益，且不需要动结构。

### 5.3 如果要合并

只在测量证明两个窗口确实可以统一时才考虑。合并方案：跨 worker 的重传也从进程内 GOP 环读 —— 但这要求 GOP 环进共享内存，等于把进程内的快路径也拖进 shm 的锁与生命周期管理，得不偿失。

**倾向保留两份，只调容量。**

---

## 附：改动清单与约束

所有改动都必须满足：

- 遵守 `docs/nginx-coding-standards.md`（格式串、内存归属、热路径禁止分配）
- TDD：先写 RED。第 1、2 项的 RED 在 host 套件里；第 3、4 项若落在 bridge/stream 模块内，需先把纯逻辑抽出来才可测
- `make -C test test` 保持 145 全绿；改动若触碰 `CORE_NAMES` 里的单元，sanitize 五组要全跑
- 第 2 项涉及 shm 布局（`ngx_rtc_ring_entry_t` 的 `sess[]` 尺寸），**必须同步 bump `NGX_RTC_SHM_LAYOUT`**（`shm.h:243`），否则 reload/重启时新二进制会读到旧布局
