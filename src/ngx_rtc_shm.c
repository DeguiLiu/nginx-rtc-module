/*
 * ngx_rtc_shm.c - `rtc_zone` directive + slab-backed source/session registry.
 *
 * Phase 0/1 of docs/multi-worker-shm-design.md. The zone-init handshake follows
 * ngx_http_limit_req_init_zone (reload reuses the old cycle's pointers, an
 * existing shm reuses shpool->data, otherwise the root table is allocated).
 *
 * Lists reuse nginx's intrusive ngx_queue_t (doubly-linked, O(1) remove) rather
 * than hand-rolled singly-linked lists; the source name index reuses
 * ngx_str_node_t / ngx_rbtree. All mutations are serialised by shpool->mutex.
 */

#include "ngx_rtc_shm.h"
#include "ngx_rtc_rtp.h"

#include <sys/eventfd.h>

#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#define NGX_RTC_BT_MAX_DEPTH  64
#define NGX_RTC_BT_BUF        512

/* Worker crash backtrace (implemented at the end of this file), wired to the
 * core module's init_process so every worker registers the handlers. */
static void       ngx_rtc_bt_handler(int signo, siginfo_t *si, void *uc);
static ngx_int_t  ngx_rtc_bt_init_process(ngx_cycle_t *cycle);

static ngx_int_t ngx_rtc_core_init_zone(ngx_shm_zone_t *shm_zone, void *data);
static void     *ngx_rtc_core_create_conf(ngx_cycle_t *cycle);
static char     *ngx_rtc_core_init_conf(ngx_cycle_t *cycle, void *conf);
static char     *ngx_rtc_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_rtc_core_init_module(ngx_cycle_t *cycle);
static ngx_uint_t ngx_rtc_shm_ring_next_pow2(ngx_uint_t v);

static ngx_rtc_shm_source_t *
ngx_rtc_shm_source_locked_lookup(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                 size_t len);
static ngx_rtc_shm_session_t *
ngx_rtc_shm_session_locked_lookup(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag,
                                  size_t len);
static void
ngx_rtc_shm_expire_locked(ngx_rtc_shm_ctx_t *ctx, ngx_uint_t forced);

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

    cccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
    ccf->nworkers = (NULL != cccf && cccf->worker_processes > 0)
                    ? (ngx_uint_t) cccf->worker_processes : 1;

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
        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        /* Master crashed/restarted but the zone survived. */
        ctx->sh = ctx->shpool->data;
        return NGX_OK;
    }

    ctx->sh = ngx_slab_alloc(ctx->shpool, sizeof(ngx_rtc_shm_ctx_t));
    if (NULL == ctx->sh) {
        return NGX_ERROR;
    }

    ngx_memzero(ctx->sh, sizeof(*ctx->sh));
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

    ngx_sprintf(ctx->shpool->log_ctx, " in rtc zone \"%V\"%Z",
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

    for (w = 0; w < ccf->sh->nworkers && w < NGX_MAX_PROCESSES; w++) {
        ccf->sh->notify_fd[w] = eventfd(0, EFD_NONBLOCK);
        if (ccf->sh->notify_fd[w] == -1) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "ngx_rtc: eventfd() failed for worker %ui", w);
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static ngx_rtc_shm_source_t *
ngx_rtc_shm_source_locked_lookup(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                 size_t len)
{
    ngx_str_node_t *sn;
    ngx_str_t       key;
    uint32_t        hash;

    key.data = name;
    key.len = len;
    hash = ngx_crc32_long(name, len);

    sn = ngx_str_rbtree_lookup(&ctx->source_tree, &key, hash);
    if (NULL == sn) {
        return NULL;
    }

    return (ngx_rtc_shm_source_t *) ((u_char *) sn
                                     - offsetof(ngx_rtc_shm_source_t, sn));
}


ngx_rtc_shm_source_t *
ngx_rtc_shm_source_get(ngx_rtc_shm_ctx_t *ctx, u_char *name, size_t len)
{
    ngx_rtc_shm_source_t *src;
    uint32_t              hash;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX) {
        return NULL;
    }

    hash = ngx_crc32_long(name, len);

    ngx_shmtx_lock(&ctx->pool->mutex);

    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL != src) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return src;
    }

    src = ngx_slab_alloc_locked(ctx->pool, sizeof(ngx_rtc_shm_source_t));
    if (NULL == src) {
        ngx_rtc_shm_expire_locked(ctx, 1); /* allocation pressure: reap, retry */
        src = ngx_slab_alloc_locked(ctx->pool, sizeof(ngx_rtc_shm_source_t));
        if (NULL == src) {
            ngx_shmtx_unlock(&ctx->pool->mutex);
            return NULL;
        }
    }

    ngx_memzero(src, sizeof(*src));
    ngx_memcpy(src->name, name, len);
    src->name[len] = '\0';
    src->publisher_slot = -1;
    src->expires = ngx_current_msec + NGX_RTC_SHM_SOURCE_EXPIRE_MS;
    src->video_ssrc = (uint32_t) ngx_random();
    src->audio_ssrc = (uint32_t) ngx_random();
    src->video_pt = NGX_RTC_PAYLOAD_TYPE_H264;
    src->audio_pt = NGX_RTC_PAYLOAD_TYPE_OPUS;

    src->snapshot = ngx_slab_alloc_locked(ctx->pool,
            NGX_RTC_SHM_GOP_SNAPSHOT_MAX
            * sizeof(ngx_rtc_shm_gop_snapshot_pkt_t));
    if (NULL != src->snapshot) {
        src->snapshot_cap = NGX_RTC_SHM_GOP_SNAPSHOT_MAX;
    }

    src->sn.str.data = src->name;
    src->sn.str.len = len;
    src->sn.node.key = hash;

    ngx_rbtree_insert(&ctx->source_tree, &src->sn.node);

    ngx_queue_init(&src->queue);
    ngx_queue_init(&src->subscribers);
    ngx_queue_insert_head(&ctx->source_list, &src->queue);

    ngx_shmtx_unlock(&ctx->pool->mutex);

    return src;
}


void
ngx_rtc_shm_source_remove(ngx_rtc_shm_ctx_t *ctx, u_char *name, size_t len)
{
    ngx_rtc_shm_source_t *src;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX) {
        return;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);

    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL == src) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return;
    }

    /* A source with viewers cannot be freed yet; the last unsubscribe retries. */
    if (!ngx_queue_empty(&src->subscribers)) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return;
    }

    ngx_queue_remove(&src->queue);
    ngx_rbtree_delete(&ctx->source_tree, &src->sn.node);
    ngx_slab_free_locked(ctx->pool, src);

    ngx_shmtx_unlock(&ctx->pool->mutex);
}


void
ngx_rtc_shm_source_add_send_stats(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                  size_t len, ngx_uint_t failed,
                                  ngx_uint_t eagain)
{
    ngx_rtc_shm_source_t *src;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX) {
        return;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);

    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL != src) {
        src->send_failed += failed;
        src->send_eagain += eagain;
    }

    ngx_shmtx_unlock(&ctx->pool->mutex);
}


ngx_rtc_shm_session_t *
ngx_rtc_shm_session_add(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag, size_t ufrag_len,
                        u_char *pwd, size_t pwd_len,
                        u_char *name, size_t name_len, uint8_t video_pt,
                        uint8_t audio_pt, uint8_t twcc_video_ext,
                        uint8_t twcc_audio_ext, uint8_t publishing)
{
    ngx_rtc_shm_session_t *sess;
    ngx_rtc_shm_source_t  *src;

    if (NULL == ctx || NULL == ufrag || 0 == ufrag_len
            || ufrag_len >= NGX_RTC_SHM_UFRAG_MAX
            || NULL == pwd || 0 == pwd_len || pwd_len >= NGX_RTC_SHM_PWD_MAX
            || NULL == name || 0 == name_len
            || name_len >= NGX_RTC_SHM_SOURCE_NAME_MAX) {
        return NULL;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);

    src = ngx_rtc_shm_source_locked_lookup(ctx, name, name_len);
    if (NULL == src) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return NULL;
    }

    sess = ngx_slab_alloc_locked(ctx->pool, sizeof(ngx_rtc_shm_session_t));
    if (NULL == sess) {
        ngx_rtc_shm_expire_locked(ctx, 1); /* allocation pressure: reap, retry */
        sess = ngx_slab_alloc_locked(ctx->pool, sizeof(ngx_rtc_shm_session_t));
        if (NULL == sess) {
            ngx_shmtx_unlock(&ctx->pool->mutex);
            return NULL;
        }
    }

    ngx_memzero(sess, sizeof(*sess));
    ngx_memcpy(sess->ice_ufrag, ufrag, ufrag_len);
    sess->ice_ufrag[ufrag_len] = '\0';
    ngx_memcpy(sess->ice_pwd, pwd, pwd_len);
    sess->ice_pwd[pwd_len] = '\0';

    sess->source = src;
    sess->id = ctx->next_session_id++;
    sess->video_pt = video_pt;
    sess->audio_pt = audio_pt;
    sess->twcc_video_ext = twcc_video_ext;
    sess->twcc_audio_ext = twcc_audio_ext;
    sess->publishing = publishing;
    sess->srtp_ready = 0;
    sess->owner_slot = -1;
    sess->expires = ngx_current_msec + NGX_RTC_SHM_SESSION_EXPIRE_MS;

    ngx_queue_init(&sess->queue);
    ngx_queue_init(&sess->sub_queue);
    ngx_queue_insert_head(&ctx->session_list, &sess->queue);

    ngx_shmtx_unlock(&ctx->pool->mutex);

    return sess;
}


static ngx_rtc_shm_session_t *
ngx_rtc_shm_session_locked_lookup(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag,
                                  size_t len)
{
    ngx_queue_t           *q;
    ngx_rtc_shm_session_t *sess;

    if (NULL == ctx || NULL == ufrag || 0 == len) {
        return NULL;
    }

    for (q = ngx_queue_head(&ctx->session_list);
         q != ngx_queue_sentinel(&ctx->session_list);
         q = ngx_queue_next(q)) {
        sess = ngx_queue_data(q, ngx_rtc_shm_session_t, queue);
        if (len == ngx_strlen(sess->ice_ufrag)
                && 0 == ngx_memcmp(sess->ice_ufrag, ufrag, len)) {
            return sess;
        }
    }

    return NULL;
}


/* Unlink and free one skeleton under the pool mutex, then reap its source when
 * the source became empty and is no longer publishing (reclaim the half-open
 * play case where no RTMP publisher ever marked it publishing). */
static void
ngx_rtc_shm_session_free_locked(ngx_rtc_shm_ctx_t *ctx,
                                ngx_rtc_shm_session_t *sess)
{
    ngx_rtc_shm_source_t *src;

    src = sess->source;
    if (NULL != src) {
        ngx_queue_remove(&sess->sub_queue);
    }

    ngx_queue_remove(&sess->queue);
    ngx_slab_free_locked(ctx->pool, sess);

    if (NULL != src && ngx_queue_empty(&src->subscribers)
            && 0 == src->publishing) {
        ngx_queue_remove(&src->queue);
        ngx_rbtree_delete(&ctx->source_tree, &src->sn.node);
        ngx_slab_free_locked(ctx->pool, src);
    }
}


ngx_int_t
ngx_rtc_shm_session_bind(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag, size_t len,
                         ngx_uint_t slot, ngx_rtc_shm_session_snapshot_t *out)
{
    ngx_rtc_shm_session_t *sess;
    ngx_rtc_shm_source_t  *src;

    if (NULL != out) {
        ngx_memzero(out, sizeof(*out));
    }

    if (NULL == ctx || NULL == ufrag || 0 == len
            || len >= NGX_RTC_SHM_UFRAG_MAX || NULL == out) {
        return NGX_ERROR;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);

    sess = ngx_rtc_shm_session_locked_lookup(ctx, ufrag, len);
    if (NULL == sess) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return NGX_ERROR;
    }

    if (sess->owner_slot != -1 && sess->owner_slot != (ngx_int_t) slot) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return NGX_DECLINED;
    }

    if (sess->owner_slot == -1) {
        sess->owner_slot = (ngx_int_t) slot;
    }

    out->id = sess->id;
    ngx_memcpy(out->ice_ufrag, sess->ice_ufrag, sizeof(sess->ice_ufrag));
    ngx_memcpy(out->ice_pwd, sess->ice_pwd, sizeof(sess->ice_pwd));
    out->video_pt = sess->video_pt;
    out->audio_pt = sess->audio_pt;
    out->twcc_video_ext = sess->twcc_video_ext;
    out->twcc_audio_ext = sess->twcc_audio_ext;
    out->publishing = sess->publishing;

    src = sess->source;
    if (NULL != src) {
        ngx_memcpy(out->source_name, src->name, sizeof(src->name));
        out->source_video_ssrc = src->video_ssrc;
        out->source_audio_ssrc = src->audio_ssrc;
        out->source_video_pt = src->video_pt;
        out->source_audio_pt = src->audio_pt;
    }

    ngx_shmtx_unlock(&ctx->pool->mutex);

    return NGX_OK;
}


ngx_int_t
ngx_rtc_shm_session_activate(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag, size_t len,
                             ngx_uint_t slot)
{
    ngx_rtc_shm_session_t *sess;

    if (NULL == ctx || NULL == ufrag || 0 == len
            || len >= NGX_RTC_SHM_UFRAG_MAX) {
        return NGX_ERROR;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);

    sess = ngx_rtc_shm_session_locked_lookup(ctx, ufrag, len);
    if (NULL == sess) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return NGX_ERROR;
    }

    if (sess->owner_slot != -1 && sess->owner_slot != (ngx_int_t) slot) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return NGX_DECLINED;
    }

    if (sess->owner_slot == -1) {
        sess->owner_slot = (ngx_int_t) slot;
    }

    sess->srtp_ready = 1;
    sess->expires = 0; /* bound and ready: no longer a half-open skeleton */

    /* Subscribe exactly once: a self-linked sub_queue means "not yet linked".
     * A WHIP publisher is the media source, not a subscriber, so it must not
     * receive its own broadcast. */
    if (0 == sess->publishing && NULL != sess->source
            && sess->sub_queue.next == &sess->sub_queue) {
        ngx_queue_insert_head(&sess->source->subscribers, &sess->sub_queue);
        sess->source->expires = 0; /* has at least one viewer */
    }

    ngx_shmtx_unlock(&ctx->pool->mutex);

    return NGX_OK;
}


void
ngx_rtc_shm_session_remove_if_owner(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag,
                                    size_t len, ngx_uint_t slot)
{
    ngx_rtc_shm_session_t *sess;

    if (NULL == ctx || NULL == ufrag || 0 == len
            || len >= NGX_RTC_SHM_UFRAG_MAX) {
        return;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);

    sess = ngx_rtc_shm_session_locked_lookup(ctx, ufrag, len);
    if (NULL == sess) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return;
    }

    /* Half-open skeletons (owner_slot == -1) are reclaimed by whoever closed
     * the corresponding per-process session; a bound skeleton is only freed by
     * its owning worker. */
    if (sess->owner_slot != -1 && sess->owner_slot != (ngx_int_t) slot) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return;
    }

    ngx_rtc_shm_session_free_locked(ctx, sess);

    ngx_shmtx_unlock(&ctx->pool->mutex);
}


ngx_int_t
ngx_rtc_shm_session_request_close(ngx_rtc_shm_ctx_t *ctx, ngx_uint_t id)
{
    ngx_queue_t           *q;
    ngx_rtc_shm_session_t *sess;

    if (NULL == ctx || 0 == id) {
        return NGX_ERROR;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);

    for (q = ngx_queue_head(&ctx->session_list);
         q != ngx_queue_sentinel(&ctx->session_list);
         q = ngx_queue_next(q)) {
        sess = ngx_queue_data(q, ngx_rtc_shm_session_t, queue);
        if (sess->id == id) {
            sess->close_requested = 1;
            ngx_shmtx_unlock(&ctx->pool->mutex);
            return NGX_OK;
        }
    }

    ngx_shmtx_unlock(&ctx->pool->mutex);

    return NGX_ERROR;
}


ngx_uint_t
ngx_rtc_shm_session_is_close_requested(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag,
                                       size_t len)
{
    ngx_rtc_shm_session_t *sess;
    ngx_uint_t             requested;

    if (NULL == ctx || NULL == ufrag || 0 == len
            || len >= NGX_RTC_SHM_UFRAG_MAX) {
        return 0;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);
    sess = ngx_rtc_shm_session_locked_lookup(ctx, ufrag, len);
    requested = (NULL != sess && 0 != sess->close_requested) ? 1 : 0;
    ngx_shmtx_unlock(&ctx->pool->mutex);

    return requested;
}


void
ngx_rtc_shm_session_set_twcc(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag, size_t len,
                             ngx_uint_t lost, ngx_uint_t received)
{
    ngx_rtc_shm_session_t *sess;

    if (NULL == ctx || NULL == ufrag || 0 == len
            || len >= NGX_RTC_SHM_UFRAG_MAX) {
        return;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);
    sess = ngx_rtc_shm_session_locked_lookup(ctx, ufrag, len);
    if (NULL != sess) {
        sess->twcc_lost = lost;
        sess->twcc_received = received;
    }
    ngx_shmtx_unlock(&ctx->pool->mutex);
}


ngx_int_t
ngx_rtc_shm_source_request_close(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                 size_t len)
{
    ngx_rtc_shm_source_t  *src;
    ngx_rtc_shm_session_t *sess;
    ngx_queue_t           *q;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX) {
        return NGX_ERROR;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);

    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL == src) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return NGX_ERROR;
    }

    for (q = ngx_queue_head(&src->subscribers);
         q != ngx_queue_sentinel(&src->subscribers);
         q = ngx_queue_next(q)) {
        sess = ngx_queue_data(q, ngx_rtc_shm_session_t, sub_queue);
        sess->close_requested = 1;
    }

    ngx_shmtx_unlock(&ctx->pool->mutex);

    return NGX_OK;
}


void
ngx_rtc_shm_source_set_publishing(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                  size_t len, ngx_uint_t publishing)
{
    ngx_rtc_shm_source_t *src;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX) {
        return;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);
    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL != src) {
        src->publishing = publishing;
        if (0 != publishing || !ngx_queue_empty(&src->subscribers)) {
            src->expires = 0; /* active: publishing or has viewers */
        } else if (0 == src->expires) {
            src->expires = ngx_current_msec + NGX_RTC_SHM_SOURCE_EXPIRE_MS;
        }
    }
    ngx_shmtx_unlock(&ctx->pool->mutex);
}


void
ngx_rtc_shm_source_set_pt(ngx_rtc_shm_ctx_t *ctx, u_char *name, size_t len,
                          uint8_t video_pt, uint8_t audio_pt)
{
    ngx_rtc_shm_source_t *src;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX) {
        return;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);
    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL != src) {
        if (0 != video_pt) {
            src->video_pt = video_pt;
        }
        if (0 != audio_pt) {
            src->audio_pt = audio_pt;
        }
    }
    ngx_shmtx_unlock(&ctx->pool->mutex);
}


void
ngx_rtc_shm_source_set_ssrc(ngx_rtc_shm_ctx_t *ctx, u_char *name, size_t len,
                            uint32_t video_ssrc, uint32_t audio_ssrc)
{
    ngx_rtc_shm_source_t *src;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX) {
        return;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);
    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL != src) {
        if (0 != video_ssrc) {
            src->video_ssrc = video_ssrc;
        }
        if (0 != audio_ssrc) {
            src->audio_ssrc = audio_ssrc;
        }
    }
    ngx_shmtx_unlock(&ctx->pool->mutex);
}


void
ngx_rtc_shm_source_set_media_stats(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                   size_t len, ngx_uint_t video_pkts,
                                   ngx_uint_t video_octets,
                                   ngx_uint_t audio_pkts,
                                   ngx_uint_t audio_octets)
{
    ngx_rtc_shm_source_t *src;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX) {
        return;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);
    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL != src) {
        src->video_pkts = video_pkts;
        src->video_octets = video_octets;
        src->audio_pkts = audio_pkts;
        src->audio_octets = audio_octets;
    }
    ngx_shmtx_unlock(&ctx->pool->mutex);
}


static void
ngx_rtc_shm_expire_locked(ngx_rtc_shm_ctx_t *ctx, ngx_uint_t forced)
{
    ngx_queue_t           *q;
    ngx_queue_t           *next;
    ngx_rtc_shm_session_t *sess;
    ngx_rtc_shm_source_t  *src;
    ngx_msec_t             now;

    now = ngx_current_msec;

    /* Reap half-open sessions (never bound to an owner worker) whose grace has
     * elapsed. Bound sessions are reclaimed by the owner worker's close path. */
    for (q = ngx_queue_head(&ctx->session_list);
         q != ngx_queue_sentinel(&ctx->session_list);
         q = next) {
        next = ngx_queue_next(q);
        sess = ngx_queue_data(q, ngx_rtc_shm_session_t, queue);
        if (sess->owner_slot != -1) {
            continue;
        }
        if (sess->expires != 0
                && (forced || now >= (ngx_msec_t) sess->expires)) {
            ngx_rtc_shm_session_free_locked(ctx, sess);
        }
    }

    /* Reap empty non-publishing sources whose grace has elapsed. */
    for (q = ngx_queue_head(&ctx->source_list);
         q != ngx_queue_sentinel(&ctx->source_list);
         q = next) {
        next = ngx_queue_next(q);
        src = ngx_queue_data(q, ngx_rtc_shm_source_t, queue);
        if (src->publishing || !ngx_queue_empty(&src->subscribers)) {
            continue;
        }
        if (src->expires != 0
                && (forced || now >= (ngx_msec_t) src->expires)) {
            ngx_queue_remove(&src->queue);
            ngx_rbtree_delete(&ctx->source_tree, &src->sn.node);
            ngx_slab_free_locked(ctx->pool, src);
        }
    }
}


void
ngx_rtc_shm_expire(ngx_rtc_shm_ctx_t *ctx, ngx_uint_t forced)
{
    if (NULL == ctx) {
        return;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);
    ngx_rtc_shm_expire_locked(ctx, forced);
    ngx_shmtx_unlock(&ctx->pool->mutex);
}


ngx_uint_t
ngx_rtc_shm_source_snapshot(ngx_rtc_shm_ctx_t *ctx, u_char *name, size_t len,
                            ngx_uint_t *ids, ngx_int_t *slots, ngx_uint_t max)
{
    ngx_rtc_shm_source_t  *src;
    ngx_rtc_shm_session_t *sess;
    ngx_queue_t           *q;
    ngx_uint_t             n;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX
            || NULL == ids || NULL == slots || 0 == max) {
        return 0;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);

    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL == src) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return 0;
    }

    n = 0;
    for (q = ngx_queue_head(&src->subscribers);
         q != ngx_queue_sentinel(&src->subscribers) && n < max;
         q = ngx_queue_next(q)) {
        sess = ngx_queue_data(q, ngx_rtc_shm_session_t, sub_queue);
        if (sess->srtp_ready && sess->owner_slot >= 0
                && sess->owner_slot < (ngx_int_t) ctx->nworkers) {
            ids[n] = sess->id;
            slots[n] = sess->owner_slot;
            n++;
        }
    }

    ngx_shmtx_unlock(&ctx->pool->mutex);

    return n;
}


void
ngx_rtc_shm_gop_snapshot_reset(ngx_rtc_shm_ctx_t *ctx, u_char *name, size_t len)
{
    ngx_rtc_shm_source_t *src;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX) {
        return;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);
    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL != src) {
        src->snapshot_count = 0;
    }
    ngx_shmtx_unlock(&ctx->pool->mutex);
}


void
ngx_rtc_shm_gop_snapshot_append(ngx_rtc_shm_ctx_t *ctx, u_char *name, size_t len,
                                const uint8_t *rtp, uint32_t rtp_len)
{
    ngx_rtc_shm_source_t         *src;
    ngx_rtc_shm_gop_snapshot_pkt_t *pkt;
    ngx_uint_t                    n;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX
            || NULL == rtp || 0 == rtp_len
            || rtp_len > NGX_RTC_RING_RTP_MAX) {
        return;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);
    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL == src || NULL == src->snapshot || 0 == src->snapshot_cap) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return;
    }

    n = (ngx_uint_t) src->snapshot_count;
    if (n < src->snapshot_cap) {
        pkt = &src->snapshot[n];
        ngx_memcpy(pkt->data, rtp, rtp_len);
        pkt->len = rtp_len;
        src->snapshot_count = n + 1;
    } else if (n == src->snapshot_cap) {
        /* IDR larger than the snapshot: mark it invalid and skip. */
        src->snapshot_count = src->snapshot_cap + 1;
    }

    ngx_shmtx_unlock(&ctx->pool->mutex);
}


ngx_int_t
ngx_rtc_shm_gop_snapshot_replay(ngx_rtc_shm_ctx_t *ctx, u_char *name, size_t len,
                                ngx_rtc_shm_gop_cb cb, void *opaque)
{
    ngx_rtc_shm_source_t         *src;
    ngx_rtc_shm_gop_snapshot_pkt_t *pkt;
    ngx_uint_t                    i;
    ngx_uint_t                    n;
    ngx_int_t                     rc;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX || NULL == cb) {
        return NGX_ERROR;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);
    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL == src || NULL == src->snapshot || 0 == src->snapshot_cap) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return NGX_ERROR;
    }

    n = (ngx_uint_t) src->snapshot_count;
    if (n > src->snapshot_cap) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return NGX_OK; /* overflowed snapshot, no valid keyframe to replay */
    }

    rc = NGX_OK;
    for (i = 0; i < n; i++) {
        pkt = &src->snapshot[i];
        rc = cb(opaque, pkt->data, pkt->len, (0 == i) ? 1 : 0);
        if (NGX_OK != rc) {
            break;
        }
    }

    ngx_shmtx_unlock(&ctx->pool->mutex);

    return rc;
}


static ngx_uint_t
ngx_rtc_shm_ring_next_pow2(ngx_uint_t v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}


ngx_rtc_shm_ring_t *
ngx_rtc_shm_ring_init(ngx_slab_pool_t *pool, ngx_uint_t slots)
{
    ngx_rtc_shm_ring_t *ring;
    ngx_uint_t          cap;
    size_t              size;

    if (NULL == pool || 0 == slots) {
        return NULL;
    }

    cap = ngx_rtc_shm_ring_next_pow2(slots);
    size = offsetof(ngx_rtc_shm_ring_t, entries)
           + (size_t) cap * sizeof(ngx_rtc_ring_entry_t);

    ngx_shmtx_lock(&pool->mutex);

    ring = ngx_slab_alloc_locked(pool, size);
    if (NULL != ring) {
        ngx_memzero(ring, size);
        (void) ngx_shmtx_create(&ring->mtx, &ring->mtx_sh,
                                (u_char *) "rtc_ring");
        ring->head = 0;
        ring->tail = 0;
        ring->size = cap;
        ring->mask = cap - 1;
    }

    ngx_shmtx_unlock(&pool->mutex);

    return ring;
}


ngx_int_t
ngx_rtc_shm_ring_enqueue(ngx_rtc_shm_ring_t *ring,
                         const ngx_rtc_ring_entry_t *entry)
{
    ngx_uint_t idx;

    if (NULL == ring || NULL == entry) {
        return NGX_ERROR;
    }

    ngx_shmtx_lock(&ring->mtx);

    if (ring->head - ring->tail >= ring->size) {
        ngx_shmtx_unlock(&ring->mtx);
        return NGX_ERROR;
    }

    idx = (ngx_uint_t) (ring->head & ring->mask);
    ring->entries[idx] = *entry;
    ring->entries[idx].seq = ring->head;
    ring->head++;

    ngx_shmtx_unlock(&ring->mtx);

    return NGX_OK;
}


ngx_int_t
ngx_rtc_shm_ring_dequeue(ngx_rtc_shm_ring_t *ring,
                         ngx_rtc_ring_entry_t *entry)
{
    ngx_uint_t idx;

    if (NULL == ring || NULL == entry) {
        return NGX_ERROR;
    }

    ngx_shmtx_lock(&ring->mtx);

    if (ring->head == ring->tail) {
        ngx_shmtx_unlock(&ring->mtx);
        return NGX_ERROR;
    }

    idx = (ngx_uint_t) (ring->tail & ring->mask);
    *entry = ring->entries[idx];
    ring->tail++;

    ngx_shmtx_unlock(&ring->mtx);

    return NGX_OK;
}


ngx_int_t
ngx_rtc_shm_ring_full(const ngx_rtc_shm_ring_t *ring)
{
    if (NULL == ring) {
        return 1;
    }

    return (ring->head - ring->tail) >= ring->size;
}


ngx_int_t
ngx_rtc_shm_ring_empty(const ngx_rtc_shm_ring_t *ring)
{
    if (NULL == ring) {
        return 1;
    }

    return ring->head == ring->tail;
}


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
