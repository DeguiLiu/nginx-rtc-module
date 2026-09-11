/*
 * ngx_rtc_core_module.c - the `rtc_zone` directive and the core module identity.
 *
 * Split out of ngx_rtc_shm.c, which carried three unrelated jobs: the
 * slab-backed source/session registry, this module's configuration parsing, and
 * its nginx module definition. The registry is pure data-structure code that
 * must run under the host sanitizer suite; the other two only exist inside
 * nginx. Keeping them apart is what lets test/Makefile compile the registry
 * with plain slab/shmtx stubs instead of dragging in the nginx config system
 * (ngx_conf_t, ngx_command_t, NGX_MODULE_V1, ...), which was the reason the
 * file had never been covered by the host tests.
 *
 * Nothing here is called by the host tests: the zone is created from a parsed
 * config, which only nginx has.
 */

#include "ngx_rtc_shm.h"
#include "ngx_rtc_rtp.h"
#include "ngx_rtc_core.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/eventfd.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#endif

#define NGX_RTC_BT_MAX_DEPTH  64
#define NGX_RTC_BT_BUF        512

#ifndef _WIN32
/* Worker crash backtrace (implemented at the end of this file), wired to the
 * core module's init_process so every worker registers the handlers. */
static void       ngx_rtc_bt_handler(int signo, siginfo_t *si, void *uc);
#endif
static ngx_int_t  ngx_rtc_bt_init_process(ngx_cycle_t *cycle);

static ngx_int_t ngx_rtc_core_init_zone(ngx_shm_zone_t *shm_zone, void *data);
static void     *ngx_rtc_core_create_conf(ngx_cycle_t *cycle);
static char     *ngx_rtc_core_init_conf(ngx_cycle_t *cycle, void *conf);
static char     *ngx_rtc_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char     *ngx_rtc_core_set_gop_ring_slots(ngx_conf_t *cf,
                     ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_rtc_core_init_module(ngx_cycle_t *cycle);

/* Tunables snapshot handed to the pure C units. Static storage keeps it alive
 * for the whole process: ngx_rtc_core_set_tunables() only publishes the
 * pointer, it does not copy. */
static ngx_rtc_tunables_t  ngx_rtc_shm_tunables;

static ngx_command_t  ngx_rtc_core_commands[] = {

    { ngx_string("rtc_zone"),
      NGX_MAIN_CONF | NGX_DIRECT_CONF | NGX_CONF_TAKE2,
      ngx_rtc_zone,
      0,
      0,
      NULL },

    { ngx_string("rtc_ring_slots"),
      NGX_MAIN_CONF | NGX_DIRECT_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      0,
      offsetof(ngx_rtc_core_conf_t, ring_slots),
      NULL },

    /* Runtime tunables (see ngx_rtc_tunables_t). All optional; the defaults in
     * ngx_rtc_core_init_conf are the historical #define values, so an existing
     * config keeps behaving exactly as before. */
    { ngx_string("rtc_jitter_timeout"),
      NGX_MAIN_CONF | NGX_DIRECT_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      0,
      offsetof(ngx_rtc_core_conf_t, jitter_timeout),
      NULL },

    { ngx_string("rtc_nack_window"),
      NGX_MAIN_CONF | NGX_DIRECT_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      0,
      offsetof(ngx_rtc_core_conf_t, nack_window),
      NULL },

    { ngx_string("rtc_nack_window_max"),
      NGX_MAIN_CONF | NGX_DIRECT_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      0,
      offsetof(ngx_rtc_core_conf_t, nack_window_max),
      NULL },

    { ngx_string("rtc_eagain_streak"),
      NGX_MAIN_CONF | NGX_DIRECT_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      0,
      offsetof(ngx_rtc_core_conf_t, eagain_streak_max),
      NULL },

    /* Custom setter: the GOP ring rejects a non-power-of-two capacity at
     * runtime, which would silently leave the ring unallocated. */
    { ngx_string("rtc_gop_ring_slots"),
      NGX_MAIN_CONF | NGX_DIRECT_CONF | NGX_CONF_TAKE1,
      ngx_rtc_core_set_gop_ring_slots,
      0,
      offsetof(ngx_rtc_core_conf_t, gop_ring_slots),
      NULL },

      ngx_null_command
};

static ngx_core_module_t  ngx_rtc_core_module_ctx = {
    ngx_string("rtc"),
    ngx_rtc_core_create_conf,
    ngx_rtc_core_init_conf
};

ngx_module_t  ngx_rtc_core_module = {
    NGX_MODULE_V1,
    &ngx_rtc_core_module_ctx,           /* module context */
    ngx_rtc_core_commands,              /* module directives */
    NGX_CORE_MODULE,                    /* module type */
    NULL,                               /* init master */
    ngx_rtc_core_init_module,           /* init module */
    ngx_rtc_bt_init_process,            /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_rtc_core_create_conf(ngx_cycle_t *cycle)
{
    ngx_rtc_core_conf_t *ccf;

    ccf = ngx_pcalloc(cycle->pool, sizeof(ngx_rtc_core_conf_t));
    if (NULL == ccf) {
        return NULL;
    }

    ccf->shm_zone = NULL;
    ccf->ring_slots = NGX_CONF_UNSET_UINT;

    ccf->jitter_timeout = NGX_CONF_UNSET_MSEC;
    ccf->nack_window = NGX_CONF_UNSET_MSEC;
    ccf->nack_window_max = NGX_CONF_UNSET_MSEC;
    ccf->eagain_streak_max = NGX_CONF_UNSET_UINT;
    ccf->gop_ring_slots = NGX_CONF_UNSET_UINT;

    return ccf;
}


static char *
ngx_rtc_core_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_rtc_core_conf_t *ccf = conf;
    ngx_core_conf_t     *cccf;

    /* The zone-init callback (ngx_rtc_core_init_zone) runs later, during
     * ngx_init_zone_pool, and populates ccf->sh / ccf->shpool. */
    ngx_conf_init_uint_value(ccf->ring_slots, NGX_RTC_RING_DEFAULT_SLOTS);

    /* Tunable defaults are the historical compile-time values, so a config that
     * sets none of them behaves exactly as before. */
    ngx_conf_init_msec_value(ccf->jitter_timeout, NGX_RTC_JITTER_TIMEOUT_MS);
    ngx_conf_init_msec_value(ccf->nack_window, NGX_RTC_NACK_WINDOW_MS);
    ngx_conf_init_msec_value(ccf->nack_window_max, NGX_RTC_NACK_WINDOW_MAX_MS);
    ngx_conf_init_uint_value(ccf->eagain_streak_max, NGX_RTC_EAGAIN_STREAK_MAX);
    ngx_conf_init_uint_value(ccf->gop_ring_slots, NGX_RTC_GOP_RING_CAP);

    cccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
    ccf->nworkers = (NULL != cccf && cccf->worker_processes > 0)
                    ? (ngx_uint_t) cccf->worker_processes : 1;

    /* Publish one read-only snapshot for the pure C units. This runs at config
     * time, before the workers fork, so every worker inherits the same values
     * and nothing has to be locked afterwards. */
    ngx_rtc_shm_tunables.jitter_timeout_ms = (uint32_t) ccf->jitter_timeout;
    ngx_rtc_shm_tunables.nack_window_ms = (uint32_t) ccf->nack_window;
    ngx_rtc_shm_tunables.nack_window_max_ms = (uint32_t) ccf->nack_window_max;
    ngx_rtc_shm_tunables.eagain_streak_max = (uint32_t) ccf->eagain_streak_max;
    ngx_rtc_shm_tunables.gop_ring_slots = (uint32_t) ccf->gop_ring_slots;

    ngx_rtc_core_set_tunables(&ngx_rtc_shm_tunables);

    return NGX_CONF_OK;
}


/*
 * rtc_gop_ring_slots needs a custom setter rather than ngx_conf_set_num_slot:
 * ngx_rtc_rtp_ring_reserve rejects a non-power-of-two capacity, so a typo would
 * leave the GOP ring permanently unallocated (no retransmit cache) instead of
 * failing the configuration.
 */
static char *
ngx_rtc_core_set_gop_ring_slots(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_rtc_core_conf_t *ccf = conf;
    ngx_str_t           *value;
    ngx_int_t            n;

    value = cf->args->elts;

    n = ngx_atoi(value[1].data, value[1].len);
    if (NGX_ERROR == n || 0 == n || 0 != (n & (n - 1))) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid %V \"%V\": must be a power of two",
                           &cmd->name, &value[1]);
        return NGX_CONF_ERROR;
    }

    ccf->gop_ring_slots = (ngx_uint_t) n;

    return NGX_CONF_OK;
}


static char *
ngx_rtc_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_rtc_core_conf_t *ccf = conf;
    ngx_shm_zone_t      *shm_zone;
    ngx_str_t           *value;
    ssize_t              size;

    value = cf->args->elts;

    size = ngx_parse_size(&value[2]);
    if (NGX_ERROR == size) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid zone size \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }

    if (size < (ssize_t)(8 * ngx_pagesize)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "zone \"%V\" is too small", &value[1]);
        return NGX_CONF_ERROR;
    }

    shm_zone = ngx_shared_memory_add(cf, &value[1], (size_t)size,
                                     &ngx_rtc_core_module);
    if (NULL == shm_zone) {
        return NGX_CONF_ERROR;
    }

    if (NULL != shm_zone->data) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "%V \"%V\" is already bound",
                           &cmd->name, &value[1]);
        return NGX_CONF_ERROR;
    }

    shm_zone->init = ngx_rtc_core_init_zone;
    shm_zone->data = ccf;

    ccf->shm_zone = shm_zone;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_rtc_core_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_rtc_core_conf_t *octx = data;
    ngx_rtc_core_conf_t *ctx = shm_zone->data;
    size_t               len;
    ngx_uint_t           w;

    if (NULL != octx) {
        /* Reload: keep the old cycle's root table and slab pool. */
        ctx->sh = octx->sh;
        ctx->shpool = octx->shpool;
        return ngx_rtc_shm_layout_check(ctx->sh, shm_zone->shm.log);
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        /* Master crashed/restarted but the zone survived. */
        ctx->sh = ctx->shpool->data;
        return ngx_rtc_shm_layout_check(ctx->sh, shm_zone->shm.log);
    }

    ctx->sh = ngx_slab_alloc(ctx->shpool, sizeof(ngx_rtc_shm_ctx_t));
    if (NULL == ctx->sh) {
        return NGX_ERROR;
    }

    ngx_memzero(ctx->sh, sizeof(*ctx->sh));
    ctx->sh->layout = NGX_RTC_SHM_LAYOUT;
    ctx->shpool->data = ctx->sh;

    ctx->sh->pool = ctx->shpool;
    ngx_rbtree_init(&ctx->sh->source_tree, &ctx->sh->source_sentinel,
                    ngx_str_rbtree_insert_value);
    ngx_queue_init(&ctx->sh->source_list);
    ngx_queue_init(&ctx->sh->session_list);
    ctx->sh->nworkers = ctx->nworkers;
    ctx->sh->ring_slots = ctx->ring_slots;
    ctx->sh->next_session_id = 1;

    for (w = 0; w < NGX_MAX_PROCESSES; w++) {
        ctx->sh->notify_fd[w] = -1;
    }

    for (w = 0; w < ctx->nworkers && w < NGX_MAX_PROCESSES; w++) {
        ctx->sh->rings[w] = ngx_rtc_shm_ring_init(ctx->shpool, ctx->ring_slots);
        if (NULL == ctx->sh->rings[w]) {
            ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                          "ngx_rtc: cannot allocate media ring %ui", w);
            return NGX_ERROR;
        }
    }

    len = sizeof(" in rtc zone \"\"") + shm_zone->shm.name.len;

    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, len);
    if (NULL == ctx->shpool->log_ctx) {
        return NGX_ERROR;
    }

    ngx_snprintf(ctx->shpool->log_ctx, len, " in rtc zone \"%V\"%Z",
                 &shm_zone->shm.name);

    ctx->shpool->log_nomem = 0;

    return NGX_OK;
}


ngx_rtc_core_conf_t *
ngx_rtc_core_get_conf(ngx_cycle_t *cycle)
{
    /* conf_ctx[index] already IS the per-cycle conf pointer (nginx stores the
     * core-module conf directly, cf. ngx_cycle.c init_conf / ngx_get_conf).
     * A single cast suffices; the old double dereference read ccf->shm_zone
     * instead and returned the shm_zone pointer, corrupting every ccf->sh /
     * ccf->nworkers access across workers. */
    return (ngx_rtc_core_conf_t *)
               ngx_get_conf(cycle->conf_ctx, ngx_rtc_core_module);
}


static ngx_int_t
ngx_rtc_core_init_module(ngx_cycle_t *cycle)
{
    ngx_rtc_core_conf_t *ccf;
    ngx_uint_t           w;

    /* Create one eventfd per worker BEFORE ngx_spawn_process() so every worker
     * inherits the write side and can be woken by the RTMP producer. Must run
     * at init_module (pre-fork) and not init_process (post-fork, worker-only):
     * init_process never runs in the master, so the old code left notify_fd[]
     * at -1 and the cross-worker wakeup was a no-op. */
    ccf = ngx_rtc_core_get_conf(cycle);
    if (NULL == ccf || NULL == ccf->sh) {
        return NGX_OK;
    }

#ifndef _WIN32
    for (w = 0; w < ccf->sh->nworkers && w < NGX_MAX_PROCESSES; w++) {
        ngx_fd_t fd;

        /* The zone outlives a reload (the root table and slab pool are reused),
         * so this slot still holds the previous cycle's eventfd. init_module
         * runs once per cycle in the master, and the master does not exit on a
         * reload, so overwriting without closing leaks one fd per worker per
         * reload until the process runs out of descriptors.
         *
         * Closing only drops this process's handle: the old workers keep their
         * own descriptor table entries and stay attached to the old open file
         * description until they exit, which is what the graceful handover
         * needs. */
        if (ccf->sh->notify_fd[w] != -1) {
            (void) close(ccf->sh->notify_fd[w]);
            ccf->sh->notify_fd[w] = -1;
        }

        fd = eventfd(0, EFD_NONBLOCK);
        if (fd == -1) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "ngx_rtc: eventfd() failed for worker %ui", w);
            return NGX_ERROR;
        }
        ccf->sh->notify_fd[w] = fd;
    }
#endif

    return NGX_OK;
}


#ifndef _WIN32
/*
 * Worker crash backtrace to error.log.
 *
 * Registers async-signal-safe handlers for the fatal signals in each worker.
 * On a crash the handler resets the default disposition, writes the signal +
 * faulting address plus a backtrace_symbols_fd() stack walk straight to
 * error.log's fd (inherited from master, opened O_APPEND, no userspace
 * buffering), then re-raises the signal so the master still logs "exited on
 * signal N" and respawns the worker.
 *
 * Async-safety: no malloc/stdio/locks in the handler; the fd is pre-opened and
 * backtrace()+backtrace_symbols_fd() are warmed up here to force the lazy libgcc
 * load before any crash. Symbol names resolve because openresty links nginx
 * with -Wl,-E (dynamic symbol export).
 */

static ngx_fd_t  ngx_rtc_bt_fd = NGX_INVALID_FILE;


static void
ngx_rtc_bt_handler(int signo, siginfo_t *si, void *uc)
{
    u_char            buf[NGX_RTC_BT_BUF];
    u_char           *p;
    void             *frames[NGX_RTC_BT_MAX_DEPTH];
    int               n;
    struct sigaction  sa;
    ngx_int_t         nw;

    (void) uc;

    /* Reset to default before anything else: never re-enter this handler. */
    ngx_memzero(&sa, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    (void) sigaction(signo, &sa, NULL);

    p = ngx_slprintf(buf, buf + sizeof(buf),
                     "\nngx_rtc_backtrace: worker %P got signal %d",
                     ngx_pid, signo);
    if (NULL != si) {
        p = ngx_slprintf(p, buf + sizeof(buf), " addr=%p", si->si_addr);
    }
    p = ngx_slprintf(p, buf + sizeof(buf), "\n");

    if (NGX_INVALID_FILE != ngx_rtc_bt_fd) {
        /* write() is marked warn_unused_result: assign then discard. */
        nw = write(ngx_rtc_bt_fd, buf, (size_t) (p - buf));
        (void) nw;

        n = backtrace(frames, NGX_RTC_BT_MAX_DEPTH);
        backtrace_symbols_fd(frames, n, ngx_rtc_bt_fd);
    }

    /* Re-raise so master logs "exited on signal N" and respawns us. */
    (void) kill(getpid(), signo);
    _exit(128 + signo);
}


static ngx_int_t
ngx_rtc_bt_init_process(ngx_cycle_t *cycle)
{
    ngx_log_t        *log;
    struct sigaction  sa;
    int               sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL };
    ngx_uint_t        i;
    void             *warm[1];

    /* error.log fd: worker inherits it from master and it stays open. */
    log = ngx_log_get_file_log(cycle->log);
    if (NULL == log || NULL == log->file) {
        ngx_rtc_bt_fd = NGX_INVALID_FILE;
    } else {
        ngx_rtc_bt_fd = log->file->fd;
    }

    /* Warm up execinfo so the first in-crash call never lazy-loads libgcc. */
    (void) backtrace(warm, 1);
    if (NGX_INVALID_FILE != ngx_rtc_bt_fd) {
        backtrace_symbols_fd(warm, 1, ngx_rtc_bt_fd);
    }

    ngx_memzero(&sa, sizeof(sa));
    sa.sa_sigaction = ngx_rtc_bt_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        if (sigaction(sigs[i], &sa, NULL) == -1) {
            ngx_log_error(NGX_LOG_WARN, cycle->log, ngx_errno,
                          "ngx_rtc_backtrace: sigaction(%d) failed", sigs[i]);
        }
    }

    return NGX_OK;
}

#else

static ngx_int_t
ngx_rtc_bt_init_process(ngx_cycle_t *cycle)
{
    (void) cycle;
    return NGX_OK;
}

#endif
