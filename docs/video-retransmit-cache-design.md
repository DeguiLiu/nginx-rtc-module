# 视频重传缓存合并设计:GOP 环 + 序号索引,消除 per-session RTX 拷贝

## 结论

把视频明文 RTP 的重传缓存从「每会话一份 RTX 环」收敛为「每个 source 一份权威缓存」,
所有会话按 RTP 序号索引引用。同 worker 会话读进程内 GOP 环(无锁),跨 worker 会话读 shm
重传环(per-ring 锁)。消除每会话约 1.5 MB 明文拷贝与每包一次 `memcpy`,读路径不再碰 slab
pool 大锁。

## 背景与问题

视频数据通路里,同一个 plaintext RTP 包被缓存两份:

- 源级 `src->gop`(进程内 GOP 环,发布者 worker)
- 会话级 `sess->rtx`(每会话 RTX 环,1024 slots ≈ 1.5 MB/会话)

`ngx_rtc_broadcast_rtp` 同 worker 路径对每个视频包先 `rtx_push`(拷入 `sess->rtx`)
再 `send_rtp`(拷入 `sess->cipher` + SRTP);跨 worker 路径经 shm 媒体环投递到 owner
worker 后再 `rtx_push` 一次。`sess->rtx` 存在的唯一理由是「跨 worker 时源 GOP 环不在
本进程」,但同 worker 会话其实可以直接读 `src->gop`,这份拷贝是纯冗余。

## 目标设计

两个物理缓存(各自必要,非冗余):

- **进程内 GOP 环 `src->gop`**:发布者 worker 上,服务同 worker 会话,无锁。
- **shm 重传环**(每 source,替换原有「关键帧快照」):服务跨 worker 会话,
  per-ring `ngx_shmtx_t`(复用媒体环既有模式)。

会话侧移除 `sess->rtx` 整环,改为 per-session NACK 去重数组 `nack_seen[]`(窗口内已重传
seq,预算 128,替代原 `slot->rtx_gen`)。

```mermaid
flowchart LR
    E[emit 视频 RTP] --> G[(src->gop<br/>同 worker 无锁)]
    E --> S[(shm 重传环<br/>per-ring shmtx)]
    E --> B[broadcast_rtp]
    B -->|同 worker| C[cipher+SRTP]
    B -->|跨 worker| M[(shm 媒体环)]
    M --> C2[cipher+SRTP]
    G -.NACK/PLI 同 worker.-> R[(会话按 seq 索引)]
    S -.NACK/PLI 跨 worker.-> R
    style G fill:#8f8,stroke:#060
    style S fill:#8f8,stroke:#060
    style R fill:#ff8,stroke:#aa0
```

### 路由规则:尽力而为,而非精确判定

统一规则是「源环有数据用源环,否则用 shm 环」,落到 `ngx_rtc_source_gop_ready()`:

```c
int ngx_rtc_source_gop_ready(const ngx_rtc_source_t *src)
{
    return (NULL != src && NULL != src->gop.slots && 0 != src->gop.count) ? 1 : 0;
}
```

这只是「非空」启发式:publisher 重启、环尚未 reserve、或跨 worker 的源在本地没有
`src->gop` 时,会**假阴性**地落到 shm 环路径。结果仍然正确(只是慢一档),因为正确性
不依赖这个路由,而是依赖两环各自按 RTP 序号定位时的**校验**:

- 进程内 `ngx_rtc_rtp_ring_get` 与 shm `ngx_rtc_shm_retransmit_get` 都先算
  `dist = (rtp_seq - start_seq) & 0xffff`,再验证 `slot` 里的 RTP seq 与请求一致,
  不一致即 miss。
- 这个 16-bit 模距离算法在窗口跨越回绕点时依然正确,**前提是环容量(1024) << 65536**,
  即 seq 距离在窗口内唯一;否则同距多解。文档明确记下这一前提。

### 为什么只有视频缓存

音频不进入重传环:音频 seq 与视频 seq 是两套独立计数器,混入同一按 seq 索引的环会破坏
NACK 定位;而且音频是连续直播流,无需 fast-start 回放。音频 NACK 在收包路径被 media_ssrc
过滤直接忽略(见 `ngx_rtc_stream_rtcp_cb` 的注释)。这是评审时会被问到的点,故提升到正文。

## 关键决策:固定窗口,而非阻塞引用计数

阻塞式引用计数(槽位等所有会话消费完才回收)不可取——一个卡顿/暂停的 viewer 会拖住
整条环无法前进,头部阻塞直接抬高延迟,与低延迟直播矛盾。正确做法是**固定窗口 + 序号
游标**:

- 环容量固定(≥ NACK 窗口 + 一个 GOP),所有会话同步消费直播流,固定窗口天然覆盖所有人
  的重传需求。
- 按 RTP 扩展序号 O(1) 定位,不维护 per-slot 原子计数,读路径只拿 per-ring 的
  `ngx_shmtx_t`,不碰 slab pool 大锁。
- 落后 viewer 需要的旧包被自然淘汰,由 NACK/PLI 兜底,而不是让它阻塞整条环。

## 容量核算

- 每 source shm 重传环 = `NGX_RTC_SHM_RETX_RING_CAP(1024) × ~1504 B ≈ 1.5 MB`。
  `rtc_zone 32m` 再叠加 per-worker 媒体环后,多分辨率 5 档并发时明显不足;多源部署应将
  zone 提到 64m 起步,或把 `NGX_RTC_SHM_RETX_RING_CAP` 降到 512(NACK 窗口只需覆盖 ~1 s)。
  当前保留 1024 以兜住完整 GOP 回放,容量约束在部署侧解决。
- 热路径拷贝账目变化:原来每视频包 `1 + Nsess` 次 memcpy(源环 + 每会话 rtx),现在是
  **2 次固定**(`src->gop` push + shm append)。会话越多越赚,单 viewer 反而 +1 次——这正是
  「消除 per-session RTX」成立的前提条件。

## 实现状态

已落地(全部编译通过,host 87/87,e2e PASS):

- `src/ngx_rtc_core.h/.c`:删除 `sess->rtx`/`rtx_gen` 与 5 个 rtx 函数;新增
  `ngx_rtc_session_retransmit` / `ngx_rtc_session_retransmit_send` /
  `ngx_rtc_session_nack_reset` / `ngx_rtc_source_gop_ready`;per-session 去重
  `nack_seen[NGX_RTC_NACK_BUDGET]`。
- `src/ngx_rtc_shm.h/.c`:新增 per-source 重传环(per-ring `ngx_shmtx_t`),
  四个 API `retransmit_reset/append/get/replay_gop`,删除 `gop_snapshot_*`。
- `src/ngx_rtmp_rtc_bridge_module.c`:broadcast 去 `rtx_push`;emit 镜像到 shm 环;
  AVC 序列头重置源环 + shm 环(防重推流 seq 回绕误命中)。
- `src/ngx_rtc_stream_module.c`:NACK/PLI/fast-start 统一走 `ngx_rtc_stream_retransmit` /
  `ngx_rtc_stream_replay_gop`;WHIP 收包补 `src->gop` push;drain/close 去 rtx 残留。
- `src/ngx_rtc_core.h`:源 `video_body`/`audio_body` 用匿名 union 复用一块 scratch
  (pipeline 数据结构复用)。
- `src/ngx_rtc_http_module.c`:stats 新增 `retransmit_alloc_failed` 计数。

## 文件清单

- `src/ngx_rtc_core.h` / `src/ngx_rtc_core.c` — 去 per-session 环,加去重 + 源环优先重传。
- `src/ngx_rtc_shm.h` / `src/ngx_rtc_shm.c` — shm 重传环 + per-ring shmtx。
- `src/ngx_rtmp_rtc_bridge_module.c` — broadcast 去 rtx_push、镜像到 shm 环。
- `src/ngx_rtc_stream_module.c` — NACK/PLI/fast-start 路由、drain_ring、去 rtx 残留。
- `src/ngx_rtc_http_module.c` — stats 计数。
- `test/test_rtc_core.c` — 重传/去重用例适配。

## 验证

- **host 单测**:`make -C test test` 全绿 87/87;`-Wall -Wextra -Werror` 无告警。
- **reset 后新流首包单测**:模拟重推流,断言 reset→append→get 命中(抓住「reset 不清
  head」这类回绕误命中回归)。
- **全量编译**:`NGX_RTC_MODULE_SRC=<repo> NGX_RTC_THIRD=<third> scripts/build-openresty.sh`
  0 error。
- **e2e**:`./run.sh nginx` + `./run.sh keep-push` + `./run.sh verify`,断言首帧 < 1s、
  音视频均收包、`ring_drops=0`、`retransmit_alloc_failed=0`。
- **多 worker 路由主证**:worker_processes ≥ 2,同 worker viewer 与跨 worker viewer 各一,
  丢包脚本触发双边 NACK,断言两环各自按 seq 命中(本设计路由正确性的主证)。
