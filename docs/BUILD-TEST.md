# 构建与测试

## 两个头世界

host 单测按 `NGX_SRC` 决定用哪套 nginx 头，两套都必须能过：

| `NGX_SRC` | 头来源 | 用途 |
| --- | --- | --- |
| 空 | `test/include/` 手写 stub | Windows（nginx 头与 Linux configure 不可分） |
| 指向已 configure 的 nginx 树（含 `objs/ngx_auto_config.h`） | nginx 自己的头 | Linux host 主力 |

`test/Makefile` 默认在 `../nginx-rtc-example/build/src/openresty-*/build/nginx-*` 下自动探测，
探不到就退回 stub。`nginx_stub.c` 用 `NGX_RTC_REAL_NGINX_HEADERS` 区分自己该提供符号还是提供对象。

**为什么两套都要跑**：stub 世界下 `NGX_PTR_SIZE`、`ngx_queue_t` 布局、`ngx_shmtx_lock` 的语义
都由我们自己定义，与真实 nginx 不同；只在 stub 世界通过的用例可能掩盖真实前提
（历史上 `ngx_shmtx_create` 未调用就是这样被掩盖的）。`scripts/check-worlds.sh` 两套各跑一遍，
要求各自 `FAIL: 0` 且 `PASS == TOTAL`。

## 测试命令

```sh
make -C test test                 # 编译 + 运行，默认头世界
make -C test test NGX_SRC=        # 强制 stub 世界
scripts/check-worlds.sh           # 两套头世界各跑一遍
scripts/sanitize-tests.sh         # ASan + UBSan，按组运行
```

## 用例数

共注册 **174** 个用例：默认组 **149**，另加 sanitizer 的四个专题组 10 / 5 / 5 / 5
（音频转码、音频线程、DTLS、SRTP）。改动后这个数会变，写文档时以实测输出为准。

## `sanitize-tests.sh` 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 全部组通过 |
| 1 | 有组失败；或给了 `--require-all` 而某组跑不起来 |
| 77 | 某组因缺依赖被跳过（未给 `--require-all`） |

跳过信息里带该组的用例数，避免"跳过"被读成"通过"。`NGX_SRC` 会透传给各组。

## 分支切分校验

```sh
scripts/check-branch-split.sh
```

断言 `win32-compat = main + scripts/branch-split.allow` 里列出的路径，其余文件必须逐字节相同。
新增一个 Windows 文件就要往白名单加一行——差异因此必然出现在 review 里。

## 一处已知的编译约束

`test/test_stream_module.c` 直接 include 生产代码 `ngx_rtc_stream_module.c`（该单元的有趣入口
全是 static），因此这个 TU 拿不到 `-Werror`，`test/Makefile` 里在该规则上方注明了原因。
