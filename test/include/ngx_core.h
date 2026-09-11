/*
 * ngx_core.h - host-test stub for the nginx core header.
 *
 * Provides the nginx types and queue/rbtree/memory macros used by
 * ngx_rtc_core.c. Queue macros are real (they are trivially correct); the
 * rbtree insert/lookup helpers and ngx_crc32_long are declared here and defined
 * as link-only no-ops in test/nginx_stub.c, because the host tests never call
 * the source registry (ngx_rtc_source_get / *_first / *_next).
 */

#ifndef NGX_STUB_CORE_H
#define NGX_STUB_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>

/* ngx_connection_t carries a sockaddr for the UDP peer address. */
#include <sys/socket.h>

typedef unsigned char u_char;
typedef unsigned long ngx_msec_t;
typedef intptr_t  ngx_msec_int_t;
typedef int       ngx_fd_t;   /* shared-memory/atomic stubs below reuse it */
typedef uintptr_t ngx_uint_t;
typedef intptr_t  ngx_int_t;
typedef int       ngx_err_t;   /* nginx's errno carrier */

/* Cached monotonic clock (host build: link-only global, see nginx_stub.c). */
extern volatile ngx_msec_t ngx_current_msec;

/* Nginx-style helpers reduced to their C-library equivalents for the host
 * build. The media modules are nginx-free; these macros keep the few nginx
 * idioms used by ngx_rtc_core.c compilable without the real nginx headers. */
#define ngx_strlen(s) strlen((const char *)(s))
#define NGX_ERROR ((ssize_t)-1)
#define NGX_AGAIN ((ssize_t)-2)
#define NGX_EAGAIN EAGAIN
#define ngx_errno errno
#define NGX_LOG_DEBUG_EVENT 8
#define NGX_LOG_INFO 7
#define ngx_socket_errno 0
#define ngx_log_debug0(level, log, err, fmt) ((void)0)
#define ngx_log_debug2(level, log, err, fmt, a1, a2) ((void)0)
#define ngx_log_debug5(level, log, err, fmt, a1, a2, a3, a4, a5) ((void)0)
/* Diagnostic lines are dropped: the host build has no ngx_log_t to write to.
 * The log handle expression is still evaluated so a helper that produces one is
 * not reported as an unused function under -Werror. The variadic tail is not
 * evaluated, which also keeps format specifiers out of this build. */
#define ngx_log_error(level, log, ...) ((void)(log))
/* ngx_rtc_core.c has no ngx_log_t of its own, so its refusal path reports
 * through ngx_log_stderr (the real nginx signature). Nothing to print here. */
#define ngx_log_stderr(err, ...) ((void)0)
/* nginx allocators. The log argument IS evaluated, exactly as nginx evaluates
 * it (it dereferences it only when the allocation fails). Dropping it made the
 * argument expression dead code, which turned real variables into
 * -Wunused-but-set-variable warnings -- a defect the stub invented rather than
 * one the production build has. */
/* Real nginx's ngx_log_error() macro reads log->log_level before deciding
 * whether to call the logger, so the host build needs a log object it can
 * instantiate -- level 0 is below every level and prints nothing. The field is
 * all that is modelled; nothing here writes a log file. */
typedef struct ngx_log_s {
    ngx_uint_t  log_level;
    void       *file;
} ngx_log_t;
#define ngx_alloc(size, log) ((void)(log), malloc(size))
#define ngx_calloc(size, log) ((void)(log), calloc(1, (size)))
#define ngx_free(p) free(p)

/* --- ngx_queue (RFC-style intrusive doubly-linked list) --- */
typedef struct ngx_queue_s ngx_queue_t;
struct ngx_queue_s
{
    ngx_queue_t *prev;
    ngx_queue_t *next;
};

#define ngx_queue_init(q) \
    do { (q)->prev = (q); (q)->next = (q); } while (0)
#define ngx_queue_head(h) ((h)->next)
#define ngx_queue_sentinel(h) (h)
#define ngx_queue_next(q) ((q)->next)
#define ngx_queue_empty(h) ((h) == (h)->next)
#define ngx_queue_insert_head(h, x) \
    do { \
        (x)->next = (h)->next; \
        (x)->next->prev = (x); \
        (x)->prev = (h); \
        (h)->next = (x); \
    } while (0)
#define ngx_queue_remove(x) \
    do { \
        (x)->next->prev = (x)->prev; \
        (x)->prev->next = (x)->next; \
        (x)->prev = NULL; \
        (x)->next = NULL; \
    } while (0)
#define ngx_queue_data(q, type, link) \
    ((type *) ((u_char *) (q) - offsetof(type, link)))

/* --- ngx_rbtree (types only; helpers are link-only stubs) --- */
typedef unsigned long ngx_rbtree_key_t;

typedef struct ngx_rbtree_node_s ngx_rbtree_node_t;
struct ngx_rbtree_node_s
{
    ngx_rbtree_key_t  key;
    ngx_rbtree_node_t *left;
    ngx_rbtree_node_t *right;
    ngx_rbtree_node_t *parent;
    u_char            color;
    u_char            data;
};

typedef struct ngx_str_s
{
    u_char *data;
    size_t  len;
} ngx_str_t;

typedef struct ngx_str_node_s
{
    ngx_rbtree_node_t node;
    ngx_str_t         str;
} ngx_str_node_t;

typedef void (*ngx_rbtree_insert_pt)(ngx_rbtree_node_t *root,
                                     ngx_rbtree_node_t *node,
                                     ngx_rbtree_node_t *sentinel);

typedef struct ngx_rbtree_s
{
    ngx_rbtree_node_t     *root;
    ngx_rbtree_node_t     *sentinel;
    ngx_rbtree_insert_pt   insert;
} ngx_rbtree_t;

/* Colour bits and the min-node walk, matching real nginx. The previous
 * `#define ngx_rbtree_min(node, sentinel) (node)` handed back the subtree root
 * instead of its leftmost node, so any traversal built on it was silently
 * misordered. The source registry needs a working tree, so these are real now. */
#define ngx_rbt_red(node)           ((node)->color = 1)
#define ngx_rbt_black(node)         ((node)->color = 0)
#define ngx_rbt_is_red(node)        ((node)->color)
#define ngx_rbt_is_black(node)      (!ngx_rbt_is_red(node))
#define ngx_rbt_copy_color(n1, n2)  ((n1)->color = (n2)->color)

static inline ngx_rbtree_node_t *
ngx_rbtree_min(ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    while (node->left != sentinel) {
        node = node->left;
    }

    return node;
}

void ngx_rbtree_insert(ngx_rbtree_t *tree, ngx_rbtree_node_t *node);
ngx_rbtree_node_t *ngx_rbtree_next(ngx_rbtree_t *tree, ngx_rbtree_node_t *node);
void ngx_str_rbtree_insert_value(ngx_rbtree_node_t *root,
                                 ngx_rbtree_node_t *node,
                                 ngx_rbtree_node_t *sentinel);
ngx_str_node_t *ngx_str_rbtree_lookup(ngx_rbtree_t *rbtree, ngx_str_t *name,
                                      uint32_t hash);
void ngx_rbtree_delete(ngx_rbtree_t *tree, ngx_rbtree_node_t *node);

/* --- misc nginx helpers --- */
uint32_t ngx_crc32_long(u_char *p, size_t len);
#define ngx_memcpy(dst, src, n) (void)memcpy((dst), (src), (n))
/* Same definitions as nginx src/core/ngx_core.h so the core modules use the
 * nginx min/max instead of ad-hoc ternaries. */
#define ngx_max(val1, val2) ((val1 < val2) ? (val2) : (val1))
#define ngx_min(val1, val2) ((val1 > val2) ? (val2) : (val1))

/* --- connection ---
 * The stream module reaches into more of this struct than the core does: it
 * detaches c->data on close, keys the posted finalize off c->log, and the UDP
 * path reads c->read/c->write. ngx_event_t is forward-declared because
 * ngx_core.h does not include the event header (the stream module includes it
 * separately, in the same order nginx does). */
typedef struct ngx_event_s ngx_event_t;

/* Just enough of ngx_buf_t for the receive path: the first datagram of a UDP
 * session is handed over in c->buffer, later ones through c->recv(). */
typedef struct {
    u_char  *pos;
    u_char  *last;
} ngx_buf_t;

typedef struct ngx_connection_s ngx_connection_t;
struct ngx_connection_s
{
    /* ssize_t, matching nginx's ngx_ssize_t: the send path checks for NGX_ERROR
     * and for a short write, neither of which an int return can express. */
    ssize_t (*send)(ngx_connection_t *c, u_char *buf, size_t size);
    /* UDP receive. Real nginx only reaches for this once c->buffer has been
     * drained, which is how the module reads both. */
    ssize_t (*recv)(ngx_connection_t *c, u_char *buf, size_t size);

    void        *data;      /* owning ngx_stream_session_t * */
    void        *log;       /* ngx_log_t * */
    ngx_fd_t     fd;
    ngx_event_t *read;
    ngx_event_t *write;
    void        *udp;       /* ngx_udp_connection_t *; opaque to the tests */
    ngx_buf_t   *buffer;    /* first datagram of a UDP session */

    struct sockaddr  *sockaddr;
    socklen_t         socklen;
};

/* ============================================================================
 * Shared memory: slab pool, shmtx, shm zone.
 *
 * ngx_rtc_shm.c locks a real shm mutex and allocates from a slab pool; neither
 * exists in a single-process host build, so the mutex becomes a pthread mutex
 * and the slab becomes malloc/free. That is exactly what makes the sanitizer
 * run meaningful: a double free, a use-after-free or a leak inside the
 * source/session registry now trips ASan instead of staying invisible. This gap
 * is not hypothetical -- the eventfd leak in ngx_rtc_core_init_module() lives in
 * that file and was found by reading it, not by a test.
 *
 * The nginx-facing entry points (zone init, per-worker notify fds) still need a
 * real cycle and remain out of scope here.
 * ============================================================================ */

/* Must stay UNSIGNED: real nginx defines ngx_atomic_t as
 * `volatile ngx_atomic_uint_t` (unsigned long), and ngx_rtc_shm.c compares
 * these against ngx_uint_t. A signed stub here produces -Wsign-compare noise
 * that does not exist in the production build. */
typedef volatile unsigned long  ngx_atomic_t;

#define NGX_OK             0
#define NGX_BUSY          (-3)
#define NGX_DECLINED      (-4)
#define NGX_MAX_PROCESSES  1024
#define NGX_RBTREE_BLACK   1

/* Process role, from nginx's ngx_process_cycle.h. The host build is a single
 * process, so nginx_stub.c sets ngx_process to the worker role: the media plane
 * only runs in a worker, and a test that calls init_process should take the
 * same branch production does. */
#define NGX_PROCESS_WORKER  2

extern ngx_int_t   ngx_process;
extern ngx_uint_t  ngx_worker;

/* The real nginx macro; only the sentinel colour and the insert hook matter. */
#define ngx_rbtree_sentinel_init(n)  ((n)->color = NGX_RBTREE_BLACK)
#define ngx_rbtree_init(tree, s, i)                                          \
    ngx_rbtree_sentinel_init(s);                                             \
    (tree)->root = s;                                                        \
    (tree)->sentinel = s;                                                    \
    (tree)->insert = i

typedef struct {
    ngx_atomic_t  lock;
    pid_t         pid;
} ngx_shmtx_sh_t;

typedef struct {
    ngx_atomic_t    *lock;       /* real nginx: points at sh->lock */
    ngx_shmtx_sh_t  *sh;
    pthread_mutex_t  host_lock;  /* host build: the lock that actually runs */
} ngx_shmtx_t;

#define ngx_shmtx_lock(m)    (void) pthread_mutex_lock(&(m)->host_lock)
#define ngx_shmtx_unlock(m)  (void) pthread_mutex_unlock(&(m)->host_lock)

typedef struct ngx_slab_pool_s {
    ngx_shmtx_t    mutex;
    void          *data;      /* real nginx: the zone base; unused here */
    u_char        *log_ctx;
    ngx_uint_t     log_nomem;
} ngx_slab_pool_t;

/* malloc/free, deliberately without zeroing: a caller that forgets to
 * initialise a field should fail here the way it would in production. */
#define ngx_slab_alloc(pool, size)         malloc(size)
#define ngx_slab_alloc_locked(pool, size)  malloc(size)
#define ngx_slab_free_locked(pool, p)      free(p)

typedef struct {
    void       *data;
    void       *shm;      /* real nginx: ngx_shm_t; compile-only here */
    ngx_int_t (*init)(void *zone, void *data);
} ngx_shm_zone_t;

/* Full nginx module layout, in nginx's field order: ngx_rtc_stream_module is
 * initialised with NGX_MODULE_V1, and ngx_stream_get_module_ctx() indexes the
 * per-session ctx array by ctx_index. The spare_hook fields are real nginx ABI
 * padding and are kept so the initialiser arity matches what a module
 * definition written for nginx already contains.
 *
 * The two forward declarations below exist because the module struct names both
 * types before either is defined further down: nginx satisfies them with its
 * own include order, which this single header has to reproduce locally. */
typedef struct ngx_command_s ngx_command_t;
typedef struct ngx_cycle_s   ngx_cycle_t;

typedef struct ngx_module_s {
    ngx_uint_t            ctx_index;
    ngx_uint_t            index;
    char                 *name;
    ngx_uint_t            spare0;
    ngx_uint_t            spare1;
    ngx_uint_t            version;
    const char           *signature;
    void                 *ctx;
    ngx_command_t        *commands;
    ngx_uint_t            type;
    ngx_int_t           (*init_master)(ngx_log_t *log);
    ngx_int_t           (*init_module)(ngx_cycle_t *cycle);
    ngx_int_t           (*init_process)(ngx_cycle_t *cycle);
    ngx_int_t           (*init_thread)(ngx_cycle_t *cycle);
    void                (*exit_thread)(ngx_cycle_t *cycle);
    void                (*exit_process)(ngx_cycle_t *cycle);
    void                (*exit_master)(ngx_cycle_t *cycle);
    uintptr_t             spare_hook0;
    uintptr_t             spare_hook1;
    uintptr_t             spare_hook2;
    uintptr_t             spare_hook3;
    uintptr_t             spare_hook4;
    uintptr_t             spare_hook5;
    uintptr_t             spare_hook6;
    uintptr_t             spare_hook7;
} ngx_module_t;

/* The ngx_module_t initialiser prefix, expanded as nginx expands it, so a
 * module definition written against nginx compiles here unchanged. The version
 * and signature are placeholders: nothing on the host validates them.
 *
 * ctx_index is NGX_MODULE_UNSET_INDEX, exactly as nginx has it -- nginx fills
 * the field in while processing the config. Seeding it with 0 (as this did)
 * hides a real requirement: ngx_stream_set_ctx() indexes the session's ctx
 * array by ctx_index, so (ngx_uint_t) -1 writes one element BEFORE the array.
 * ASan reported that as a global-buffer-overflow the moment the real headers
 * arrived. */
#define NGX_MODULE_UNSET_INDEX  ((ngx_uint_t) -1)
#define NGX_MODULE_V1          NGX_MODULE_UNSET_INDEX, NGX_MODULE_UNSET_INDEX, \
                               NULL, 0, 0, 1, "stub"
#define NGX_MODULE_V1_PADDING  0, 0, 0, 0, 0, 0, 0, 0

/* Configuration-layer flags. The values are nginx's, but nothing here parses a
 * config -- they only have to be distinct integers for the command table. */
#define NGX_STREAM_MODULE           0x03000000u
#define NGX_CONF_NOARGS             0x00000001u
#define NGX_STREAM_SRV_CONF         0x00000100u
#define NGX_STREAM_SRV_CONF_OFFSET  0

/* Field order follows this stub's ngx_str_t ({ data, len }), which is the
 * reverse of nginx's -- see the note on ngx_string() below. */
#define ngx_null_string  { NULL, 0 }
#define ngx_null_command { ngx_null_string, 0, NULL, 0, 0, NULL }

typedef int  ngx_flag_t;

/* Only the fields ngx_rtc_shm.c touches on a cycle. */
struct ngx_cycle_s {
    void      **conf_ctx;
    ngx_log_t  *log;
    void       *pool;
};

#define ngx_get_conf(conf_ctx, module)  ((conf_ctx)[(module).index])

/* Defined in nginx_stub.c: the host build never creates a shared zone. */
ngx_int_t ngx_shmtx_create(ngx_shmtx_t *mtx, ngx_shmtx_sh_t *sh,
                           u_char *name);

/* ============================================================================
 * Memory / string / atomic helpers used by ngx_rtc_shm.c.
 * ============================================================================ */

#define ngx_memzero(buf, n)     (void) memset((buf), 0, (n))
#define ngx_memcmp(s1, s2, n)   memcmp((s1), (s2), (n))

/* The host build is single-process, so plain arithmetic is a faithful stand-in
 * for the real fetch-add. */
typedef unsigned long  ngx_atomic_uint_t;
#define ngx_atomic_fetch_add(p, n)  __sync_fetch_and_add((p), (n))

uint32_t ngx_random(void);
void     ngx_gettimeofday(struct timeval *tp);

/* The cycle pointer the registry reaches for outside config time (publish
 * claim/release). The host tests never install one, so it stays NULL; the
 * helpers only dereference it on paths the tests do not take. */
extern ngx_cycle_t *ngx_cycle;

/* ============================================================================
 * Configuration layer.
 *
 * ngx_rtc_shm.c also carries the module's rtc_zone directive, so compiling that
 * translation unit pulls in the nginx conf types even though the host tests
 * never parse a config. These are compile-only: enough shape for the static
 * command table in that file to be well-formed, with no runtime behaviour.
 * ============================================================================ */

/* The conf object the command setters receive. `pool` is real: the stream
 * module's create_srv_conf allocates from it. `ctx` mirrors nginx's per-module
 * conf slots so ngx_stream_conf_get_module_srv_conf() below can index it the
 * way nginx does, rather than handing back a NULL that a caller dereferences. */
typedef struct ngx_conf_s ngx_conf_t;

struct ngx_conf_s {
    void  *pool;
    void **ctx;
};

struct ngx_command_s {
    ngx_str_t   name;
    ngx_uint_t  type;
    char       *(*set)(ngx_conf_t *cf, struct ngx_command_s *cmd, void *conf);
    ngx_uint_t  conf;
    ngx_uint_t  offset;
    void       *post;
};
typedef struct ngx_command_s ngx_command_t;

/* NOTE: this stub's ngx_str_t is { data, len } -- the opposite field order from
 * real nginx ({ len, data }). The command table in ngx_rtc_shm.c only has to
 * COMPILE here, since the host tests never parse a config, so the macro matches
 * the stub rather than nginx. Do not reuse it for anything that runs. */
#define ngx_string(str)  { (u_char *) str, sizeof(str) - 1 }

/* Referenced by that same command table; never called on the host. Only the
 * setter the host-compiled command tables actually name is declared: the other
 * ngx_conf_set_*_slot variants are used by modules this build does not compile,
 * so declaring them here would be dead surface. */
char *ngx_conf_set_msec_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

#define NGX_CONF_TAKE1      0x00000002u
#define NGX_CONF_UNSET      0
#define NGX_CONF_UNSET_UINT ((ngx_uint_t) -1)
#define NGX_CONF_UNSET_MSEC ((ngx_msec_t) -1)

/* Conf setters return NGX_CONF_OK; nginx defines it as the null pointer, and
 * the stream module compares the value it passes up. */
#define NGX_CONF_OK  NULL

/* Allocators over the conf pool. Nothing on the host owns a pool, but the
 * stream module's create_srv_conf path only needs the memory to be zeroed --
 * the merge step below treats 0 as NGX_CONF_UNSET. */
#define ngx_pcalloc(pool, size)  calloc(1, (size))

/* The real nginx merge macros, so the unset-vs-default logic in
 * ngx_rtc_stream_merge_srv_conf() is the production logic, not a stub of it. */
#define ngx_conf_merge_msec_value(conf, prev, default)                       \
    if ((conf) == NGX_CONF_UNSET_MSEC) {                                     \
        (conf) = ((prev) == NGX_CONF_UNSET_MSEC) ? (default) : (prev);       \
    }

#define ngx_conf_merge_value(conf, prev, default)                            \
    if ((conf) == NGX_CONF_UNSET) {                                          \
        (conf) = ((prev) == NGX_CONF_UNSET) ? (default) : (prev);            \
    }

#endif /* NGX_STUB_CORE_H */
