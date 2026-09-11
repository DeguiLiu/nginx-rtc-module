/*
 * ngx_stream.h - host-test stub for the nginx stream module header.
 *
 * ngx_rtc_stream_module.c is the UDP media plane. The parts worth covering on
 * the host are the datagram state machine and the session lifetime rules
 * (attach from shm, teardown, the close_pending hand-back), not the stream
 * framework itself -- so this stub provides the session/module/conf shapes the
 * file touches, and nothing that needs a real event loop.
 *
 * ctx handling mirrors nginx exactly: the per-module slot is indexed by
 * module.ctx_index, which is why ngx_module_t carries that field in
 * test/include/ngx_core.h.
 */

#ifndef NGX_STUB_STREAM_H
#define NGX_STUB_STREAM_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

/* `ctx` is a POINTER to an array, not an inline array -- that is nginx's shape,
 * and it is not cosmetic: the ctx macros index through it, so a session whose
 * ctx was never allocated (nginx does it in ngx_stream_init_connection, from
 * the cycle pool) dereferences NULL. An inline array here would let a test that
 * forgot to allocate it pass on Windows and crash on Linux. */
typedef struct ngx_stream_session_s ngx_stream_session_t;

struct ngx_stream_session_s
{
    ngx_connection_t  *connection;
    void             **ctx;
    void              *log;
};

typedef struct {
    ngx_connection_t  *connection;
    void              *log;
    void             (*handler)(ngx_stream_session_t *s);
} ngx_stream_core_srv_conf_t;

/* Signatures follow the real ngx_stream_module_t, so the stream module's static
 * callbacks can be listed here without a cast. The loc-conf hooks nginx also
 * declares are omitted: this module does not define them. */
typedef struct {
    void *(*preconfiguration)(ngx_conf_t *cf);
    void *(*postconfiguration)(ngx_conf_t *cf);
    void *(*create_main_conf)(ngx_conf_t *cf);
    void *(*init_main_conf)(ngx_conf_t *cf, void *conf);
    void *(*create_srv_conf)(ngx_conf_t *cf);
    char *(*merge_srv_conf)(ngx_conf_t *cf, void *prev, void *conf);
} ngx_stream_module_t;

/* Referenced by ngx_rtc_stream_rtc() through the macro below. nginx defines
 * this object in ngx_stream_module.c; the host build defines a placeholder in
 * nginx_stub.c so the translation unit links. */
extern ngx_module_t ngx_stream_core_module;

/* ctx handling mirrors nginx exactly: the per-module slot is indexed by
 * module.ctx_index, which is why ngx_module_t carries that field in
 * test/include/ngx_core.h. The srv-conf accessor does the same, over the conf
 * ctx array the tests install on ngx_conf_t. */
#define ngx_stream_get_module_ctx(s, module)  (s)->ctx[module.ctx_index]
#define ngx_stream_set_ctx(s, c, module)      s->ctx[module.ctx_index] = c;

#define ngx_stream_conf_get_module_srv_conf(cf, module)                   \
    ((ngx_stream_core_srv_conf_t *) (cf)->ctx[(module).ctx_index])

/* nginx passes the session's exit status here; the module only ever uses OK. */
#define NGX_STREAM_OK  0

void ngx_stream_finalize_session(ngx_stream_session_t *s, ngx_uint_t rc);

#endif /* NGX_STUB_STREAM_H */
