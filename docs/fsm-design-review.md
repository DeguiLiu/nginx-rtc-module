# 状态机设计评审

评审对象:本仓全部隐式/显式状态编码。规模:17 个纯 C 单元 + 5 个胶水单元,
1 个形式化状态机引擎(`ngx_rtc_hsm`)、1 个使用它的状态机(`ngx_rtc_session_fsm`)。

## 结论

1. **现存的 session FSM 设计是合理的**,但它的角色是「闸门」而不是「驱动」:只判定状态、
   不执行动作。这是刻意为之,理由已写在 `ngx_rtc_session_fsm.h:15-19`。保留。
2. **HSM 引擎自身有 3 个缺陷**(本轮已修,见 §5):生产环境 assert 中止整个 worker、
   转移失败时留下撕裂状态、`int8_t` 截断导致入口动作被静默跳过。
3. **全仓只有一处真正需要状态机:源发布权(publish ownership)**。但它**不该用 HSM 引擎**
   —— 它要解决的是「两个副本 + 一把锁的原子更新」,不是层级与冒泡。用普通转移函数。
4. shm session 骨架(5 状态)排第二,但转移已全部集中在 `ngx_rtc_shm.c` 内,收益小得多,
   **建议不动**。
5. DTLS / avsync / jitter 那几处 2-3 状态的标志位**不该**上状态机,属于过度设计。

## 1 判据

上状态机的收益来自四件事。四项都不满足时,平铺的顺序代码 + 错误码更清楚。

| 判据 | 说明 |
| --- | --- |
| **非线性** | 状态数 ≥ 3 且转移有分支/重试/回退,不是一条直线 |
| **非法转移必须被拒** | 存在「从网络乱序/重复/竞态到达的、必须拒绝的事件」 |
| **状态需要被外部观测** | 超时回收、诊断、统计要读当前状态 |
| **转移点分散** | 同一状态被 ≥3 处代码改写,且改写序列有隐含顺序 |

反过来说,**不该上**的三种情形:

- 线性流水线,只在单个函数内跑一遍 —— 顺序语句 + `return` 错误码更易读;
- 状态就是 1 个布尔 —— `is_ready()` 谓词够用;
- 「状态」其实是某个算法内部的循环相位(如 jitter 的 gap 窗口)—— 上状态机只是把
  局部变量搬到全局,还平白增加热路径分支。

## 2 现状盘点

### 2.1 已形式化的部分

`ngx_rtc_session_fsm`(5 状态:NEW → ICE_BOUND → DTLS_HANDSHAKE → SRTP_READY → CLOSED)。
驱动点 15 处,分布在 4 个文件:

| 文件 | 驱动点 |
| --- | --- |
| `ngx_rtc_stream_module.c` | init 440、STUN 绑定 557、DTLS 首包 607、RTCP 654、SRTP 799、握手完成 1108、关闭 1219、回收扫描 1444 |
| `ngx_rtc_http_module.c` | 会话创建 700、WHIP 创建 1322 |
| `ngx_rtc_core.c` | 未处理事件上报 335、发送路径就绪判定 476 |
| `ngx_rtmp_rtc_bridge_module.c` | SR 上报 376 |

反面确认(全仓扫描):**除了这个 FSM,不存在任何 `switch (state)` / `if (state == X)` 的
状态推进代码**。仅有的两个 switch 是 `ngx_rtc_rtcp.c:378`(按 RTCP 包类型分派)与
`ngx_rtc_sdp.c:682`(按 SDP 行首字符分派),都不是状态机。也没有 `stage` / `sub_stage` /
`phase` 这类字段。也就是说,本仓**没有藏起来的、正在腐坏的状态机**。

### 2.2 候选清单

| # | 状态承载 | 状态数 | 写入点 | 作用域 | 跨 worker 共享 | 判定 |
| --- | --- | --- | --- | --- | --- | --- |
| A | 源发布权 `publisher_kind` + `publishing` | 3 | **5(本地)+ 2(shm)** | per-source | **是** | **该做** |
| B | shm session 骨架 `owner_slot`/`srtp_ready`/`close_requested`/`expires` | 5 | 10(9 处已在 shm.c 内) | per-session | **是** | 可选,收益小 |
| C | DTLS `handshake_done` | 2 | 3 | per-session | 否 | 不做 |
| D | avsync `has_anchor`/`has_slope` | 3 | 3 | per-source/track | 否 | 不做 |
| E | jitter `initialized` | 2 | 2 | per-source | 否 | 不做 |
| F | 若干分量标志位(`drop_until_gop`、`pacer_started`、`twcc_win_started`、`sps_profile_level_id_valid` …) | 各 2 | 分散 | per-session | 否 | 不做 |

## 3 现存 session FSM 评审

### 3.1 合理之处

- **无动作设计是对的。** 转移表里 `action` 全是 `NULL`,状态机只回答「现在在哪」,不回答
  「该做什么」。真正持有 nginx 连接与 DTLS/SRTP 上下文的 `ngx_rtc_stream_module.c` 才是
  执行者。这样避免了「状态机里存指针 → 生命周期与连接脱钩」这一类最难查的缺陷。
- **守卫承载了真实的判定责任,不是装饰。** `s_session_dtls_guard()`
  (`ngx_rtc_session_fsm.c:68-85`)要求「只有长度与类型都像 DTLS 记录的报文才能建立 DTLS
  上下文」。这挡住了一个真实场景:UDP 分派只看首字节做的启发式判断,可能把噪声误判成
  DTLS。守卫把它补成了长度+范围检查。
- **闸门语义使「只建立一次」成为结构性保证。** `ngx_rtc_stream_module.c:600` 用
  `dtls_pending()` 包住 DTLS 上下文的创建,转移一旦发生该谓词即为假,所以重复到达的
  DTLS 记录不会重复创建上下文。这是状态机真正在挣饭钱的地方。
- **根状态收敛了关闭类事件。** CLOSE / TIMEOUT / RTCP_BYE 挂在根上,任何状态都能关闭;
  CLOSED 状态又把这三个事件吞掉(`ngx_rtc_session_fsm.c:143-148`),避免根回退被重复触发。

### 3.2 值得注意的两点

- `NGX_RTC_SESSION_FSM_MAX_DEPTH` 是 2,与当前层级(root → leaf)正好相等。**新加一个
  第三层的状态而忘记同步这个常量,在修复前会中止整个 worker**。本轮修掉了中止行为
  (§5),但这个常量与状态表之间的耦合仍然是人工维护的 —— 加状态时必须一起改。
- `s_session_srtp_ready_transitions` 没有 `DTLS_PACKET` 项。SRTP 就绪后的 DTLS 报文
  (重协商、alert、重传)会被判为未处理事件并上报。当前无害,因为 DTLS 数据在传输层
  就已处理,状态机不参与;但如果有谁开始依赖状态机判定「这个 DTLS 包该不该收」,这里
  会给出错误的答案。

## 4 候选逐一评估

### A. 源发布权 —— 该做,但不用 HSM 引擎

**它确实是状态机。** 三个状态(NONE / RTMP / WHIP),两个事件(claim / release),且有
必须拒绝的非法转移:

- 「另一种协议已持有该名字时再 claim」→ `NGX_BUSY`(`ngx_rtc_shm.c:933`);
- 「非持有者 release」→ 忽略(`ngx_rtc_shm.c:961`)。

**shm 那一半已经是正确的状态机了。** `ngx_rtc_shm_source_try_publish` /
`release_publish` 在 `pool->mutex` 内完成状态转移,守卫条件正确,`publishing` 与
`publisher_kind` 同步更新。这一半不该动。

**真正的问题在进程本地的镜像。** `ngx_rtc_source_t` 上有第二份
`publisher_kind` / `publishing`,由 5 处互不相干的代码手写:

| 位置 | 做什么 |
| --- | --- |
| `ngx_rtc_http_module.c:1306` | WHIP 认领(shm claim 成功后) |
| `ngx_rtmp_rtc_bridge_module.c:273` | RTMP 释放 |
| `ngx_rtmp_rtc_bridge_module.c:674` | RTMP 认领:先清掉陈旧的 WHIP 标签,再 sync_shm |
| `ngx_rtmp_rtc_bridge_module.c:925` | 同上,音频路径 |
| `ngx_rtc_stream_module.c:477` | `attach_from_shm` 打标签 |

其中 **674 与 925 是逐字节相同的两段**(本轮改 `NGX_RTC_PUBLISHER_NONE` 时,替换工具
报「找到 2 处匹配」即为证据),而 273 是第三份局部拷贝。同一份「清陈旧标签 → 向 shm
认领 → 回写本地」的顺序逻辑,写了三遍。

**这不是「缺状态机」,而是「缺唯一的写入口」。** 两份状态互为镜像,却由不同代码路径
分别更新,漂移是结构性的 —— 本会话已经在此处抓到两个真实缺陷(WHIP 断流不释放发布权;
bridge 的检查顺序导致名字被永久锁死)。两者都属于「一份更新了、另一份没有」或
「顺序错了」。

**建议(已落地,三个函数约 160 行,4 个文件):**

```c
/* ngx_rtc_shm.h —— 本地镜像的唯一写入者 */
ngx_int_t ngx_rtc_publish_claim (ngx_rtc_source_t *src, ngx_uint_t kind);
void      ngx_rtc_publish_release(ngx_rtc_source_t *src);
void      ngx_rtc_publish_mirror (ngx_rtc_source_t *src, ngx_uint_t kind);
```

- `claim`:在 shm 侧 `try_publish`(持锁)→ 成功则本地 `kind`/`publishing`/`shm_src` 一并
  写入;`NGX_BUSY` 则清空本地镜像。**本地镜像只能由这次调用改写。**
- `release`:在 shm 侧 `release_publish` + `source_remove` → 本地一并清空。
- `mirror`:采纳另一个 worker 已持有的认领,只写本地、不碰 shm —— 给
  `attach_from_shm` 的媒体 worker 用。

之所以是三个而不是两个:`mirror` 若混进 `claim`,媒体 worker 会变成第二个认领者。

收敛结果(实测 grep 确认):`publisher_kind` / `publishing` 的赋值点从 5 处散布在 3 个
文件,收敛到 `ngx_rtc_shm.c` 一处三个函数。bridge 里那两段逐字节重复的
「清陈旧标签 → 认领」顺序逻辑随之删除。

**顺带修掉一个跨协议漏洞。** `ngx_rtmp_rtc_sync_shm()` 的指针缓存快路径原先只检查
`src->shm_src != NULL && 新鲜`,不检查归属 kind。但 `attach_from_shm()` 也会给
**WHIP** 已发布的源设置 `shm_src` —— 若同一个 worker 恰好在服务那个名字,RTMP 推流会
走快路径直接返回成功、跳过归属仲裁,于是同名出现两个发布者。已给快路径加上
`NGX_RTC_PUBLISHER_RTMP == src->publisher_kind` 前置条件。

**为什么不用 `ngx_rtc_hsm`:** HSM 引擎的价值在层级、LCA、事件冒泡;这里三个状态是平铺的,
层级机制完全用不上,却要付出状态表 + 守卫函数 + 每实例入口路径缓冲的代价。更关键的是,
HSM 引擎对「两个副本 + 一把锁」一无所知,而这恰恰是这里唯一重要的性质。
按既有的反过度设计原则,这里要的是普通转移函数,不是通用引擎。

### B. shm session 骨架 —— 可选,建议暂不动

状态链 `UNBOUND(owner_slot == -1) → BOUND → READY(srtp_ready) → CLOSE_REQUESTED →
FREED` 由 4 个字段正交编码:`owner_slot` / `srtp_ready` / `close_requested` / `expires`。
2×2×2 共 8 种组合里合法的大概只有 4-5 种,非法组合无人阻挡 —— 这是典型的「该用枚举」。
`owner_slot` 是 `ngx_int_t`,`-1` 哨兵用法正确(`ngx_rtc_shm.h:124`),没有类型错误。

但**转移点几乎全部已在 `ngx_rtc_shm.c` 内部**(`session_add` / `bind` / `activate` /
`remove_if_owner` / `request_close` / `expire_locked` / `session_free_locked`),
外部只有 5 个驱动调用。集中度已经很高,收敛成一个枚举主要是可读性收益,不是缺陷修复。
**结论:等它真的出一次「非法组合」缺陷再改**,现在改属于推测性重构。

顺带记录一处不一致以便将来处理:`owner_slot` 与 `publishing` 是裸整数(靠 `pool->mutex`
保护),而 `srtp_ready` / `close_requested` / `expires` / `twcc_*` 是 `ngx_atomic_t`
(靠原子性)。两种同步原语混在同一结构里,读代码的人无法从字段本身判断该用哪种保护。

### C / D / E / F. 不做

- **C DTLS `handshake_done`**:2 状态、3 个写入点,且真正的握手状态机在 OpenSSL 内部
  (重传、Cookie、epoch 迁移)。在它外面再套一层只会与 OpenSSL 的状态重复。
- **D avsync `has_anchor`/`has_slope`**:3 状态但只有一个含义「标定进度」,转移是单向的
  (EMPTY → ONE_ANCHOR → MAPPABLE),判据四项一条都不满足。
- **E jitter `initialized`**:2 状态,且它在**每包热路径**上。上状态机等于在热路径加一次
  间接调用。
- **F 一组 2 状态标志位**:它们是彼此独立的模式位/计时器位,不是一个整体相位。强行合并
  成枚举会把不相关的概念绑死(`pacer_started` 与 `drop_until_gop` 没有任何关系)。

## 5 本轮已修的引擎缺陷

`ngx_rtc_hsm` 是 Andreas Misje 的轻量 HSM 的 C11 移植。移植保真,但有三处不符合本仓规范。

### 5.1 生产环境 `assert` 中止整个 worker

`ngx_rtc_hsm.c` 原先在入口路径缓冲不够时执行 `NGX_RTC_HSM_ASSERT(0)`,默认展开为
`assert()`。本仓的 nginx 构建 **未定义 `NDEBUG`**(`objs/Makefile` 的 CFLAGS 可证),
所以该断言在生产 worker 里是活的:一次几何配置失误会 `abort()`,把该 worker 上**全部**
会话一起带走。这与本仓规范中「错误处理用错误码返回,不用 assert」一致。

**已改为**返回 `false`,并通过 `ngx_rtc_hsm_dispatch()` 的返回值与未处理事件钩子
(本仓已安装为 `ngx_rtc_session_fsm_report_unhandled`)上报 —— 变成一条可见的日志,
而不是一次进程消失。

### 5.2 转移失败留下撕裂状态

原实现顺序是:先跑退出动作 → 再计算入口路径 → 缓冲不够则断言。后果是**退出动作已经执行、
`current_state` 却仍指向那个刚刚退出的状态**,机器停在「已退出、未进入」的位置上。
即便断言被编译掉,状态依然是错的。

**已改为**先计算入口路径(纯读状态表,无副作用),失败则直接返回 —— 不跑任何动作、
不改 `current_state`,机器停在原地。退出动作在路径确认可用之后才执行。

### 5.3 `int8_t` 截断导致入口动作被静默跳过

`hsm_execute_entry_actions()` 原先以 `int8_t entry_idx = (int8_t)path_length - 1` 起算。
层级深度超过 127 时该值为负,循环一次都不执行,于是**全部入口动作被静默跳过**——
又是一例「看起来正常」的静默失败。`path_length` 的上界是调用方给的
`buffer_size`(`uint8_t`),所以 127 以上是可达的。

**已改为**无符号递减,整段 `uint8_t` 范围都可用。

## 6 验证

新增 host 用例 `hsm_short_entry_path_leaves_state_unchanged`(`test/test_hsm.c`):
构造一个比调用方缓冲更深的层级,断言转移被拒、事件被上报为未处理、**且退出动作没有执行**。

该用例在修复前是 RED —— 用旧顺序编译同一份测试得到:

```
FAIL test_hsm.c:265: g_log ("Au") != "u" ("u")
TOTAL: 114  PASS: 113  FAIL: 1
```

`"Au"` 中的 `A` 就是「退出动作已经跑了」的直接证据。修复后:

```
TOTAL: 114  PASS: 114  FAIL: 0
```

## 7 结论与建议次序

| 次序 | 事项 | 规模 | 是否建议现在做 |
| --- | --- | --- | --- |
| — | HSM 引擎 3 处缺陷 | 已改完 | 已完成 |
| 1 | 源发布权收敛为 `ngx_rtc_publish_claim/release/mirror` | 3 个函数,4 个文件 | **已完成** |
| 2 | session FSM 的 `MAX_DEPTH` 与状态表耦合 | 加注释/编译期校验 | 顺手做 |
| 3 | shm session 骨架枚举化 | ~100 行 | 不建议,等出现缺陷 |
| 4 | DTLS / avsync / jitter / 标志位 | — | 不做,属过度设计 |

## 8 第 1 项落地后的验证

| 项 | 结果 |
| --- | --- |
| 写入口收敛 | `grep 'publisher_kind *=\|publishing *='` 在 `ngx_rtc_core.h` 的
`ngx_rtc_source_t` 上只剩 `ngx_rtc_shm.c` 的三个函数(shm source 与 shm session 上的同名字段不在此列) |
| 编译 | `rc=0`,改动文件零告警(仅有 `ngx_rtc_audio.c` 既存的 ffmpeg 弃用告警) |
| host 单测 | 114 / 114 |
| `nginx -t` | 配置解析通过 |
| **e2e** | **未跑。**规范 §11:胶水层改动 host 单测覆盖不到,必须 e2e。归属仲裁的
三条路径都只在运行期可见,见下 |

e2e 需要覆盖的三条路径:

1. WHIP 推流 → 断开 → 同名 RTMP 推流能接管(即本会话早先修的那个缺陷,回归验证);
2. RTMP 推流进行中 → 同名 WHIP 推流被拒(HTTP 409);
3. WHIP 推流进行中、同一 worker 服务该名字时 → RTMP 推流被拒(本轮新修的快路径漏洞)。

第 3 条需要构造「WHIP 的 UDP 会话与 RTMP 连接落在同一个 worker」,单 worker 部署下必然
成立,是最省事的验证方式。
