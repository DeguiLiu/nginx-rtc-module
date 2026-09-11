# 构建、测试与交付拓扑方案

## 结论

五项里两项值得做、两项是防复发、一项建议不动：

| 序 | 项 | 建议 | 理由 |
| --- | --- | --- | --- |
| 1 | `NGX_SRC` 探测路径依赖 | **做** | 现状让「验真头」取决于 checkout 在哪，是个隐式环境耦合 |
| 2 | sanitize 组缺依赖静默跳过 | **做** | 绿灯可能意味着 25 个用例没跑，CI 语义错误 |
| 3 | 分支切分靠人工 | **做**（校验脚本） | 本仓历史里已漂移过三次 |
| 4 | 平台守卫散落 | **做**（集中到一个头） | 每加一个源文件就要记得两分支同步 |
| 5 | host 测试包含 .c 文件 | **不动** | 替代方案更差，只需补一条约束 |

---

## 1 `NGX_SRC` 探测是路径依赖的

### 1.1 现状

`test/Makefile:36`：

```make
NGX_SRC ?= $(shell for d in $(CURDIR)/../../nginx-rtc-example/build/src/openresty-*/build/nginx-*; do \
                     if [ -f "$$d/objs/ngx_auto_config.h" ]; then echo "$$d"; break; fi; \
                   done)
```

探测路径以 `$(CURDIR)` 为基准。main 的工作树在 `/home/ci/...`，与 example 仓库不在同一父目录下，于是**永远探测失败、永远落到 stub 世界**。

现在已经有可见性（打印 `header world:`，探测失败时 `$(warning ...)`，对象按 world 分目录），但根因还在：**「能不能验真头」这件事取决于你在哪 checkout**。

### 1.2 方案

**一、让 world 成为一个显式输入，而不是探测结果。**

保留探测作为便利默认，但在 CI / 脚本里一律显式传：

```sh
make -C test NGX_SRC=/abs/path/to/nginx-1.31.1
```

`test/Makefile` 已经支持（且注释里写了「must be an absolute path」），只是没有调用方这么做。

**二、`scripts/sanitize-tests.sh` 显式透传。** 它现在只传 `EXTRA_CPPFLAGS`，不传 `NGX_SRC`。加上之后，sanitize 跑的 world 与 `make test` 一致，不再各自探测。

**三、探测失败在 CI 里视为错误。**
现在缺 `NGX_SRC` 只是 warning。CI 上应该反过来：**要求显式提供**，探测失败即失败。这样「真头世界跑过没有」就是可判定的。

### 1.3 验收

- 两个工作树分别跑，`header world:` 分别是 `real` 与 `stub`
- 在 main 工作树上显式传 `NGX_SRC` 能跑出 `real`，且 145 全绿
- CI 配置里两者都被显式指定

---

## 2 缺依赖时整组跳过

### 2.1 现状

`scripts/sanitize-tests.sh`：

```sh
if [ 0 -eq "$HAVE_SRTP" ]; then
    echo "== srtp / audio / audio_worker: SKIPPED ==" >&2
    echo "   no libsrtp2.a under ${THIRD:-<unset>}; build it with" >&2
    echo "   nginx-rtc-example/scripts/build-deps.sh" >&2
    exit 0
fi
```

三组（25 个用例）整组跳过，**退出码 0**。命令行上会打印 SKIPPED，但在 CI 日志里，一次 `exit 0` 与「全部通过」没有区别。

### 2.2 后果

- 一个对外宣称「五组全绿」的报告，可能实际只跑了一组
- 依赖在某次环境变更后消失，不会有人注意到

### 2.3 方案

**一、区分「跳过」与「通过」的退出码。**

```sh
SKIPPED=0
...
if [ 0 -eq "$HAVE_SRTP" ]; then
    echo "== srtp / audio / audio_worker: SKIPPED ==" >&2
    SKIPPED=1
else
    run_group srtp ...
    run_group audio ...
    run_group audio_worker ...
fi
...
exit $SKIPPED     # 0 = 全跑完，非 0 = 有组没跑
```

用一个**约定值**（例如 77，Automake 的 SKIP 约定）而不是任意非零，调用方才能和「测试失败」区分开。

**二、加一个 `--require-all` 开关。** CI 传它，缺依赖即硬失败；本地开发不传，保持现在的宽松。

### 2.4 验收

- 无 libsrtp2 时：退出码 77，输出含 SKIPPED
- 带 `--require-all` 且无 libsrtp2 时：非零退出
- 有依赖时：退出码 0，五组全跑

---

## 3 分支切分靠人工维持

### 3.1 现状

`main` 与 `win32-compat` 的约定是：

```
win32-compat = main + win32 集合
```

当前集合：

| 仓库 | 文件数 | 内容 |
| --- | --- | --- |
| module | 9 | `.gitignore`、`README.md`、`README.zh.md`、`config`、`scripts/cross-win64-tests.sh`、`src/ngx_rtc_core.h`、`src/ngx_rtc_core_module.c`、`src/ngx_rtc_stream_module.c`、`test/include/ngx_core.h` |
| example | 8 | `deploy/win32/*`（5）、`docs/archive/win64-linux-crossbuild-notes.md`、`scripts/package-win64.sh`、`scripts/win64-windres-wrapper.sh` |

这条约定**没有机制保证**。本仓历史上至少漂移过三次：

1. host 测试与真实头构建共 1265 行平台中立的代码只在 win32-compat 上
2. docs 归档与设计文档刷新只在 win32-compat 上
3. example 的 Windows guide 两分支新旧不一致

三次都是靠人工发现、人工补的。

### 3.2 方案

**白名单校验脚本**，挂进 CI，也做成一个可手工跑的 target：

```sh
#!/bin/sh
# scripts/check-branch-split.sh
# 退出非零表示分支切分漂移了。
expected_module=".gitignore README.md README.zh.md config \
                 scripts/cross-win64-tests.sh \
                 src/ngx_rtc_core.h src/ngx_rtc_core_module.c \
                 src/ngx_rtc_stream_module.c test/include/ngx_core.h"
actual=$(git diff --name-only main win32-compat | sort)
[ "$actual" = "$(printf '%s\n' $expected_module | sort)" ] || { echo "drift:"; ...; exit 1; }
```

要点：

- **双向校验** —— `git diff win32-compat main` 必须为空（win32-compat 是严格超集），这条比白名单更重要，因为它能抓住「main 上多了东西」
- 白名单变化必须**显式改脚本**，于是每次加 win32 文件都会留下一条 diff，评审时看得见
- 对 example 仓库同样一份

**另一条更根本的问题**：新增源文件时要不要带平台守卫，全靠人记得。见 §4。

### 3.3 验收

- 脚本在干净树上返回 0
- 人为在 main 上加一个 `_WIN32` 分支，脚本返回非零

---

## 4 平台守卫散落

### 4.1 现状

win32 可移植守卫分布在四个文件：

```
src/ngx_rtc_core.h            winsock2.h / ws2tcpip.h  vs  sys/socket.h
src/ngx_rtc_core_module.c     winsock vs sys/eventfd.h + execinfo.h；#ifndef _WIN32 包住 backtrace
src/ngx_rtc_stream_module.c   winsock2.h vs arpa/inet.h
test/include/ngx_core.h       winsock2.h vs sys/socket.h
```

每新增一个源文件，如果要 include 平台相关的头，就要在两分支上分别改一次。而 main 上不允许出现 `_WIN32`，所以这个「分别改」是**必须人工完成且容易漏**的。

### 4.2 方案

**建 `src/ngx_rtc_platform.h`（只存在于 win32-compat）**，把所有平台分支收进去：

```c
/* win32-compat 分支专有。main 上不存在此文件，main 的源文件直接 include
 * POSIX 头。 */
#ifndef NGX_RTC_PLATFORM_H
#define NGX_RTC_PLATFORM_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/eventfd.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#endif

#endif
```

源文件里改成一行 `#include "ngx_rtc_platform.h"`。

**但这会引入一个更糟的问题**：main 上不能 include 一个不存在的文件。所以源文件必须写成

```c
#include "ngx_rtc_platform.h"   /* win32-compat 专有 */
```

而 main 上没有这个文件 → main 编译失败。

**所以这条方案不成立**，除非：

- 方案 A：把 `ngx_rtc_platform.h` 放进 **main**，内容只含 POSIX 头；win32-compat 上覆盖它加入 `_WIN32` 分支。这样两个分支都有这个文件，只是内容不同，源文件在两分支上都只需一行 include。**代价**：main 上多了一个只有 5 行的头文件，且它是「为 win32 预留的位置」。
- 方案 B：保持现状，靠 §3 的校验脚本兜底。

**推荐方案 B。** 理由：方案 A 用「main 上多一个看似无用的头」换取「新增文件少改一处」，但本仓的源文件数量已经稳定（13 个纯 C 核心 + 6 个胶水），新增文件的频率很低。为了一个低频操作引入一个需要解释的头文件，不划算。

**真正该做的是把「新增源文件要不要守卫」写进规范**：`docs/nginx-coding-standards.md` 的 §1 加一句 —— 新文件若 include 平台相关头，必须在两分支上分别处理，且 main 上不得出现 `_WIN32`。

---

## 5 host 测试包含 .c 文件（建议不动）

### 5.1 现状

`test/test_stream_module.c:30`：

```c
#include "../src/ngx_rtc_stream_module.c"
```

因为 `ngx_rtc_stream_module.c` 把所有有价值的入口点都声明为 `static`（接收路径、关闭路径、DTLS 完成回调），只能靠包含 TU 来测。该文件**不在** `CORE_NAMES` 里，所以不会与链接进来的同名符号冲突。

### 5.2 评估

替代方案都不好：

| 方案 | 代价 |
| --- | --- |
| 把 static 改成非 static 并链接 | 为了测试破坏封装，且该文件依赖 nginx 符号，链接不上 |
| 加 `#ifdef NGX_RTC_TEST` 暴露内部函数 | 生产代码里出现测试专用的条件编译，更差 |

**保留包含 TU 的做法。**

### 5.3 但要补一条约束

包含 TU 意味着该文件被编译两次、且两次的编译选项不同：

- nginx 构建：`-O -W -Wall -Wpointer-arith -Wno-unused-parameter`
- 测试构建：同一个 flag 集（`test/Makefile` 给 `test_test_stream_module.o` 单独指定 `NGX_CFLAGS`）

两者一致是当前刻意的选择（见 `test/Makefile` 的注释），但要写清楚：**这个文件永远拿不到 `-Werror`**，所以它的警告要靠人工看构建输出，不能靠测试套件兜底。

这条应该写进 `docs/nginx-coding-standards.md` 的验证章节。

---

## 6 实施顺序与验收

```
第 1 步  §2 退出码（改动最小，收益立刻可见：CI 不再把跳过当通过）
第 2 步  §3 校验脚本（防复发，一次投入长期收益）
第 3 步  §1 显式 NGX_SRC + CI 要求
第 4 步  §5.3 补规范文字
         §4 明确不做，只在规范里补一句
```

每步的验收：

- 第 1 步：无依赖时退出码 77；`--require-all` 时非零
- 第 2 步：干净树返回 0；人为漂移时非零
- 第 3 步：main 工作树上显式 `NGX_SRC` 跑出 `real` 且 145 全绿
- 第 4 步：文档改动，无代码风险

**全部改动都不触碰 `src/`，因此不影响模块行为，也不需要 e2e。**
