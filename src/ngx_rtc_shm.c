/*
 * ngx_rtc_shm.c - slab-backed source/session registry.
 *
 * Phase 0/1 of multi-worker-shm-design.md, which lives in the deploy repo at
 * nginx-rtc-example/docs/ -- this repo has no docs/ of its own, so the bare
 * filename that used to be here named nothing. Pure data-structure code: it
 * owns no nginx configuration and defines no nginx module. The `rtc_zone`
 * directive and the module identity live in ngx_rtc_core_module.c, which is
 * what lets this file be compiled by the host test suite with plain slab/shmtx
 * stubs (see test/test_shm.c).
 *
 * Lists reuse nginx's intrusive ngx_queue_t (doubly-linked, O(1) remove) rather
 * than hand-rolled singly-linked lists; the source name index reuses
 * ngx_str_node_t / ngx_rbtree. All mutations are serialised by shpool->mutex.
 */

#include "ngx_rtc_shm.h"
#include "ngx_rtc_rtp.h"
#include "ngx_rtc_core.h"

#include <string.h>

static ngx_uint_t ngx_rtc_shm_ring_next_pow2(ngx_uint_t v);

static ngx_rtc_shm_source_t *
ngx_rtc_shm_source_locked_lookup(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                 size_t len);
static ngx_rtc_shm_session_t *
ngx_rtc_shm_session_locked_lookup(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag,
                                  size_t len);
static void
ngx_rtc_shm_expire_locked(ngx_rtc_shm_ctx_t *ctx, ngx_uint_t forced);
static ngx_uint_t
ngx_rtc_shm_source_referenced_locked(ngx_rtc_shm_ctx_t *ctx,
                                     const ngx_rtc_shm_source_t *src);

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
    if (0 == ngx_queue_empty(&src->subscribers)) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return;
    }

    /* Never free a source that is still publishing: a live publisher holds a
     * cached shm_src pointer that would dangle. release_publish clears the flag
     * before the normal remove path; this guard is defense-in-depth against any
     * caller that removes a publishing source. */
    if (0 != src->publishing) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return;
    }

    /* And never one a skeleton still points at. The session keeps sess->source
     * and writes through it after this returns -- session_free_locked arms
     * expires through it, session_activate inserts into its subscribers. The
     * grace in `expires` cannot stand in for this check: publish_release()
     * calls release_publish() (which arms it) and then this function, on the
     * very next line. ngx_rtc_shm_expire_locked() applies the same test; it is
     * the only path that may free. */
    if (0 != ngx_rtc_shm_source_referenced_locked(ctx, src)) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return;
    }

    ngx_queue_remove(&src->queue);
    ngx_rbtree_delete(&ctx->source_tree, &src->sn.node);
    if (NULL != src->retransmit) {
        ngx_slab_free_locked(ctx->pool, src->retransmit);
    }
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
    /* The creator is the signaling worker, and no worker owns the UDP session
     * yet, so NEW is the one state it can honestly publish. Every later write
     * comes from whichever worker ends up owning the media path. */
    sess->state = NGX_RTC_SESSION_STATE_NEW;
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


/* Unlink and free one skeleton under the pool mutex. When the source is left
 * empty and no longer publishing it is NOT freed here: see the grace arm below.
 * (Reclaiming the half-open play case, where no RTMP publisher ever marked the
 * source publishing, now happens through ngx_rtc_shm_expire_locked.) */
static void
ngx_rtc_shm_session_free_locked(ngx_rtc_shm_ctx_t *ctx,
                                ngx_rtc_shm_session_t *sess)
{
    ngx_rtc_shm_source_t *src;

    src = sess->source;
    if (NULL != src) {
        if (sess->sub_queue.next != &sess->sub_queue) {
            /* Was subscribed: unlink and drop the cross-worker count. */
            ngx_queue_remove(&sess->sub_queue);
            if (sess->owner_slot != src->publisher_slot
                    && src->remote_subscribers > 0) {
                (void) ngx_atomic_fetch_add(&src->remote_subscribers,
                                            (ngx_atomic_uint_t) -1);
            }
            src->subscribers_version++;
        }
    }

    ngx_queue_remove(&sess->queue);
    ngx_slab_free_locked(ctx->pool, sess);

    /* The source just lost a session. Even when it is now empty and no longer
     * publishing it must survive the grace period: another worker may hold the
     * shm_src pointer it resolved up to NGX_RTC_SHM_SYNC_MS ago and keeps
     * writing through it (stats counters, retransmit ring) until its next
     * re-claim. Freeing here bypassed the src->expires grace that
     * ngx_rtc_shm_source_release_publish armed, so a release on one worker
     * could free the source under a publisher still running on another.
     * Leaving the free to ngx_rtc_shm_expire_locked keeps one path and one
     * grace; it requires
     *     NGX_RTC_SHM_SOURCE_EXPIRE_MS > NGX_RTC_SHM_SYNC_MS
     * so the grace always outlives the pointer cache. */
    if (NULL != src && ngx_queue_empty(&src->subscribers)
            && 0 == src->publishing && 0 == src->expires) {
        src->expires = ngx_current_msec + NGX_RTC_SHM_SOURCE_EXPIRE_MS;
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

    if (-1 == sess->owner_slot) {
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
    out->srtp_ready = (0 != sess->srtp_ready) ? 1 : 0;

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

    if (-1 == sess->owner_slot) {
        sess->owner_slot = (ngx_int_t) slot;
    }

    sess->srtp_ready = 1;
    sess->state = NGX_RTC_SESSION_STATE_SRTP_READY;
    sess->expires = 0; /* bound and ready: no longer a half-open skeleton */

    /* Subscribe exactly once: a self-linked sub_queue means "not yet linked".
     * A WHIP publisher is the media source, not a subscriber, so it must not
     * receive its own broadcast. */
    if (0 == sess->publishing && NULL != sess->source
            && sess->sub_queue.next == &sess->sub_queue) {
        ngx_queue_insert_head(&sess->source->subscribers, &sess->sub_queue);
        sess->source->subscribers_version++;
        sess->source->expires = 0; /* has at least one viewer */
        if (sess->owner_slot != sess->source->publisher_slot) {
            (void) ngx_atomic_fetch_add(&sess->source->remote_subscribers, 1);
        }
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
ngx_rtc_shm_session_set_stats(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag, size_t len,
                              ngx_uint_t lost, ngx_uint_t received,
                              ngx_uint_t pacer_bps, ngx_uint_t drop_pacer,
                              ngx_uint_t drop_gop)
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
        sess->pacer_bps = pacer_bps;
        sess->drop_pacer = drop_pacer;
        sess->drop_gop = drop_gop;
    }
    ngx_shmtx_unlock(&ctx->pool->mutex);
}


void
ngx_rtc_shm_session_set_state(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag, size_t len,
                              ngx_uint_t slot, uint8_t state)
{
    ngx_rtc_shm_session_t *sess;

    if (NULL == ctx || NULL == ufrag || 0 == len
            || len >= NGX_RTC_SHM_UFRAG_MAX) {
        return;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);
    sess = ngx_rtc_shm_session_locked_lookup(ctx, ufrag, len);
    if (NULL != sess
            && (-1 == sess->owner_slot
                || sess->owner_slot == (ngx_int_t) slot)) {
        sess->state = state;
    }
    ngx_shmtx_unlock(&ctx->pool->mutex);
}


ngx_int_t
ngx_rtc_shm_layout_check(const ngx_rtc_shm_ctx_t *sh, ngx_log_t *log)
{
    if (NULL != sh && NGX_RTC_SHM_LAYOUT == sh->layout) {
        return NGX_OK;
    }

    ngx_log_error(NGX_LOG_EMERG, log, 0,
                  "ngx_rtc: the rtc_zone was built by a different build "
                  "(layout %uD, this binary expects %uD); a reload cannot reuse "
                  "it, stop and start the server instead",
                  (NULL != sh) ? sh->layout : (uint32_t) 0,
                  (uint32_t) NGX_RTC_SHM_LAYOUT);

    return NGX_ERROR;
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


ngx_int_t
ngx_rtc_shm_source_try_publish(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                               size_t len, ngx_uint_t kind)
{
    ngx_rtc_shm_source_t *src;

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

    /* A source has exactly one publisher. Reject a different kind; allow the
     * same kind (idempotent re-claim on every media packet). */
    if (0 != src->publisher_kind && src->publisher_kind != kind) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return NGX_BUSY;
    }

    src->publisher_kind = kind;
    src->publishing = 1;
    src->expires = 0; /* active: publishing */
    /* Arm the liveness heartbeat. The publish path refreshes it per packet
     * (ngx_rtmp_rtc_shm_stats); if this worker dies without releasing, it stops
     * advancing and the reaper reclaims the source. */
    src->publisher_seen_ms = (ngx_atomic_t) ngx_current_msec;

    ngx_shmtx_unlock(&ctx->pool->mutex);

    return NGX_OK;
}


void
ngx_rtc_shm_source_release_publish(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                   size_t len, ngx_uint_t kind)
{
    ngx_rtc_shm_source_t *src;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX) {
        return;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);
    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL == src || src->publisher_kind != kind) {
        /* Not ours: a stale close must not clear a live publisher's flag. */
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return;
    }

    src->publisher_kind = NGX_RTC_PUBLISHER_NONE;
    src->publishing = 0;
    if (0 == ngx_queue_empty(&src->subscribers)) {
        src->expires = 0; /* still has viewers: keep alive */
    } else if (0 == src->expires) {
        src->expires = ngx_current_msec + NGX_RTC_SHM_SOURCE_EXPIRE_MS;
    }

    ngx_shmtx_unlock(&ctx->pool->mutex);
}


/*
 * Publish ownership: the only writers of a local source's ownership pair. See
 * the contract in ngx_rtc_shm.h -- briefly, the shm source is the authority and
 * ngx_rtc_source_t carries a lock-free mirror for the per-packet media path.
 * Keeping every write in these three functions is what stops the two copies
 * from drifting, which is where both known ownership defects came from.
 */

ngx_int_t
ngx_rtc_publish_claim(ngx_rtc_source_t *src, ngx_uint_t kind)
{
    ngx_rtc_core_conf_t  *ccf;
    ngx_rtc_shm_source_t *shm_src;
    ngx_int_t             rc;

    if (NULL == src) {
        return NGX_ERROR;
    }

    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);

    if (NULL == ccf || NULL == ccf->sh) {
        /* No rtc_zone: one worker, so there is nothing to arbitrate against. */
        src->publisher_kind = kind;
        src->publishing = 1;
        src->shm_src = NULL;
        return NGX_OK;
    }

    rc = ngx_rtc_shm_source_try_publish(ccf->sh, (u_char *) src->name,
                                        ngx_strlen(src->name), kind);

    if (NGX_BUSY == rc) {
        /* Another protocol holds the name. Clear the mirror rather than keep
         * claiming a right this process does not have: a stale tag here is what
         * made an RTMP takeover of a dead WHIP name impossible until the whole
         * process was restarted. */
        src->publisher_kind = NGX_RTC_PUBLISHER_NONE;
        src->publishing = 0;
        src->shm_src = NULL;
        return NGX_BUSY;
    }

    src->publisher_kind = kind;
    src->publishing = 1;

    if (NGX_OK == rc) {
        /* Safe to cache: try_publish held the mutex and found this source to
         * claim, and publishing == 1 now keeps ngx_rtc_shm_source_remove() from
         * freeing it under us. NGX_ERROR instead means the source vanished
         * between the caller's lookup and the claim -- degrade to no mirror
         * rather than caching a dangling pointer. */
        shm_src = ngx_rtc_shm_source_get(ccf->sh, (u_char *) src->name,
                                         ngx_strlen(src->name));
    } else {
        shm_src = NULL;
    }

    src->shm_src = shm_src;
    if (NULL != shm_src) {
        src->shm_sync_ms = ngx_current_msec;
    }

    return NGX_OK;
}


void
ngx_rtc_publish_release(ngx_rtc_source_t *src)
{
    ngx_rtc_core_conf_t *ccf;
    ngx_uint_t           kind;

    if (NULL == src) {
        return;
    }

    kind = src->publisher_kind;
    if (NGX_RTC_PUBLISHER_NONE == kind) {
        /* Not ours (or already released): a stale close must not clear a live
         * publisher's flag. */
        return;
    }

    /* Clear the mirror first. The shm half below is about to make the source
     * reapable, and nothing in this process may keep reading a cached pointer
     * into it afterwards. Clearing it also forces the next publish in this
     * process through the claim path instead of the cached-pointer fast path. */
    src->publisher_kind = NGX_RTC_PUBLISHER_NONE;
    src->publishing = 0;
    src->shm_src = NULL;

    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
    if (NULL == ccf || NULL == ccf->sh) {
        return;
    }

    ngx_rtc_shm_source_release_publish(ccf->sh, (u_char *) src->name,
                                       ngx_strlen(src->name), kind);

    /* Hand the shm source back too. remove() is the one that decides: it
     * refuses while subscribers remain or the source is still published, so a
     * name with viewers survives and the last unsubscribe retries. Doing it
     * here keeps both protocols from having to know that ordering. */
    ngx_rtc_shm_source_remove(ccf->sh, (u_char *) src->name,
                              ngx_strlen(src->name));
}


void
ngx_rtc_publish_mirror(ngx_rtc_source_t *src, ngx_uint_t kind)
{
    if (NULL == src) {
        return;
    }

    /* The shm claim is already held by another worker; adopting it here must
     * not touch the shm, or this process would become a second claimant of a
     * right it does not own. */
    src->publisher_kind = kind;
    src->publishing = 1;
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


/*
 * True when some session skeleton still points at `src`.
 *
 * A source may only be freed once nothing references it. `subscribers` is not
 * that test: a session that has not finished its handshake is not a subscriber
 * yet, and a WHIP publisher never becomes one, so the reaper used to free a
 * source out from under a live `sess->source`. session_activate() then inserts
 * into `src->subscribers` -- a write into freed slab, through a pointer the
 * NULL check there cannot catch. Only the intrusive list links are read here;
 * comparing two slab pointers never dereferences one.
 */
static ngx_uint_t
ngx_rtc_shm_source_referenced_locked(ngx_rtc_shm_ctx_t *ctx,
                                     const ngx_rtc_shm_source_t *src)
{
    ngx_queue_t           *q;
    ngx_rtc_shm_session_t *sess;

    for (q = ngx_queue_head(&ctx->session_list);
         q != ngx_queue_sentinel(&ctx->session_list);
         q = ngx_queue_next(q)) {
        sess = ngx_queue_data(q, ngx_rtc_shm_session_t, queue);
        if (sess->source == src) {
            return 1;
        }
    }

    return 0;
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
        if (-1 != sess->owner_slot) {
            continue;
        }
        if (0 != sess->expires
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

        if (0 != src->publishing) {
            /* A clean teardown clears this flag; a worker that died takes it to
             * the grave, and the source -- with its ~1.5 MB retransmit ring --
             * would then stay pinned for the life of the zone, permanently
             * refusing any later publish of the same name as a cross-protocol
             * conflict. The heartbeat written per packet by
             * ngx_rtmp_rtc_shm_stats() is the liveness proof: while it keeps
             * advancing the publisher is alive and the source is left alone;
             * once it has been silent past the grace, fall through and let the
             * normal rules below decide (subscribers still hold it back). */
            if (0 != src->publisher_seen_ms
                    && (ngx_msec_int_t) (now
                           - (ngx_msec_t) src->publisher_seen_ms)
                           < (ngx_msec_int_t) NGX_RTC_SHM_PUBLISH_GRACE_MS) {
                continue;
            }

            src->publishing = 0;
            src->publisher_kind = NGX_RTC_PUBLISHER_NONE;
        }

        if (0 == ngx_queue_empty(&src->subscribers)) {
            continue;
        }
        /* An empty subscriber list is not enough -- see
         * ngx_rtc_shm_source_referenced_locked(). A source held back this way
         * keeps its elapsed `expires`, so it is collected on the first pass
         * after the last session pointing at it goes away. */
        if (0 != src->expires
                && (forced || now >= (ngx_msec_t) src->expires)
                && 0 == ngx_rtc_shm_source_referenced_locked(ctx, src)) {
            ngx_queue_remove(&src->queue);
            ngx_rbtree_delete(&ctx->source_tree, &src->sn.node);
            if (NULL != src->retransmit) {
                ngx_slab_free_locked(ctx->pool, src->retransmit);
            }
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


/* Lazily allocate the per-source retransmit ring. Caller holds pool mutex. */
static ngx_rtc_shm_retransmit_t *
ngx_rtc_shm_retransmit_alloc_locked(ngx_rtc_shm_ctx_t *ctx,
                                    ngx_rtc_shm_source_t *src)
{
    ngx_rtc_shm_retransmit_t *r;
    size_t                    size;

    size = sizeof(ngx_rtc_shm_retransmit_t)
         + (NGX_RTC_SHM_RETX_RING_CAP - 1u)
           * sizeof(ngx_rtc_shm_retransmit_slot_t);
    r = ngx_slab_alloc_locked(ctx->pool, size);
    if (NULL == r) {
        return NULL;
    }

    ngx_memzero(r, size);
    r->cap = NGX_RTC_SHM_RETX_RING_CAP;

    src->retransmit = r;
    return r;
}


/* 16-bit RTP sequence number carried in a plaintext RTP header. */
static uint16_t
ngx_rtc_shm_retx_seq(const u_char *data)
{
    return (uint16_t)(((uint16_t)data[2] << 8) | (uint16_t)data[3]);
}


void
ngx_rtc_shm_retransmit_reset(ngx_rtc_shm_ctx_t *ctx, u_char *name, size_t len)
{
    ngx_rtc_shm_source_t      *src;
    ngx_rtc_shm_retransmit_t  *r;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX) {
        return;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);
    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL == src || NULL == src->retransmit) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return;
    }
    r = src->retransmit;
    r->head = 0;      /* new generation: indices restart, not just count */
    r->count = 0;
    r->gop_start = 0;
    ngx_shmtx_unlock(&ctx->pool->mutex);
}


/*
 * Microsecond clock for the retransmit-ring instrumentation below. nginx's
 * ngx_current_msec is a cached millisecond and far too coarse for sections that
 * run in tens of microseconds; ngx_gettimeofday is the portable one (nginx
 * core provides it for unix and win32 alike) and carries the microsecond field
 * those sections need. Deliberately not clock_gettime: this module also builds
 * for Windows and the cross build has no such symbol.
 */
static ngx_uint_t
ngx_rtc_shm_now_us(void)
{
    struct timeval  tv;

    ngx_gettimeofday(&tv);
    return (ngx_uint_t) tv.tv_sec * 1000000u + (ngx_uint_t) tv.tv_usec;
}


void
ngx_rtc_shm_retransmit_append(ngx_rtc_shm_ctx_t *ctx,
                              ngx_rtc_shm_source_t *src,
                              const uint8_t *rtp, uint32_t rtp_len,
                              uint8_t is_gop_start)
{
    ngx_rtc_shm_retransmit_t      *r;
    ngx_rtc_shm_retransmit_slot_t *slot;
    ngx_uint_t                     idx;
    ngx_uint_t                     t0;

    if (NULL == ctx || NULL == src || NULL == rtp || 0 == rtp_len
            || rtp_len > NGX_RTC_RING_RTP_MAX) {
        return;
    }

    /* Same-worker viewers read the in-process GOP ring; the shm mirror only
     * serves cross-worker viewers. Skip the pool mutex, rbtree lookup, copy and
     * lazy alloc until a viewer on another worker subscribes. The publisher's
     * cached src stays valid while publishing == 1 (the same invariant the
     * broadcast path relies on for its lock-free subscribers_version read). */
    if (0 == src->remote_subscribers) {
        return;
    }

    t0 = ngx_rtc_shm_now_us();
    ngx_shmtx_lock(&ctx->pool->mutex);
    src->retx_append_locked++;
    if (0 == src->remote_subscribers) {
        /* Re-check under the lock: a cross-worker viewer may have unsubscribed
         * since the lock-free fast-path read above. */
        src->retx_append_us += ngx_rtc_shm_now_us() - t0;
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return;
    }
    r = src->retransmit;
    if (NULL == r) {
        r = ngx_rtc_shm_retransmit_alloc_locked(ctx, src);
        if (NULL == r) {
            src->retransmit_alloc_failed++;
            /* Log the first failure only: without the guard a zone-starved
             * source re-logs at packet rate. Retries stay (zone space frees up
             * when other streams end, and the ring self-heals from then on). */
            if (1 == src->retransmit_alloc_failed) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                              "ngx_rtc: retransmit ring alloc failed for "
                              "source \"%V\"", &src->sn.str);
            }
            src->retx_append_us += ngx_rtc_shm_now_us() - t0;
            ngx_shmtx_unlock(&ctx->pool->mutex);
            return; /* retransmit unavailable; live media path unaffected */
        }
    }

    idx = r->head & (r->cap - 1u);
    slot = &r->slots[idx];
    ngx_memcpy(slot->data, rtp, rtp_len);
    slot->len = (uint16_t) rtp_len;
    slot->is_gop_start = (uint8_t)(0 != is_gop_start ? 1 : 0);
    if (0 != is_gop_start) {
        r->gop_start = r->head;
    }
    r->head++;
    if (r->count < r->cap) {
        r->count++;
    }
    src->retx_append_us += ngx_rtc_shm_now_us() - t0;
    ngx_shmtx_unlock(&ctx->pool->mutex);
}


ngx_int_t
ngx_rtc_shm_retransmit_get(ngx_rtc_shm_ctx_t *ctx, u_char *name, size_t len,
                           uint16_t rtp_seq, u_char *out, size_t out_cap,
                           uint16_t *out_len)
{
    ngx_rtc_shm_source_t      *src;
    ngx_rtc_shm_retransmit_t  *r;
    ngx_rtc_shm_retransmit_slot_t *slot;
    ngx_uint_t                 start;
    ngx_uint_t                 idx;
    uint16_t                   start_seq;
    uint16_t                   dist;
    ngx_int_t                  rc;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX
            || NULL == out || NULL == out_len) {
        return NGX_DECLINED;
    }

    ngx_shmtx_lock(&ctx->pool->mutex);
    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL == src || NULL == src->retransmit) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        return NGX_DECLINED;
    }
    r = src->retransmit;

    rc = NGX_DECLINED;
    if (0 != r->count) {
        start = r->head - r->count;
        idx = start & (r->cap - 1u);
        start_seq = ngx_rtc_shm_retx_seq(r->slots[idx].data);
        dist = (uint16_t)(rtp_seq - start_seq);
        if ((uint32_t)dist < r->count) {
            idx = (start + (uint32_t)dist) & (r->cap - 1u);
            slot = &r->slots[idx];
            if (ngx_rtc_shm_retx_seq(slot->data) == rtp_seq
                    && (size_t)slot->len <= out_cap) {
                ngx_memcpy(out, slot->data, slot->len);
                *out_len = slot->len;
                rc = NGX_OK;
            }
        }
    }
    ngx_shmtx_unlock(&ctx->pool->mutex);

    return rc;
}


ngx_int_t
ngx_rtc_shm_retransmit_replay_gop(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                  size_t len, ngx_rtc_shm_retransmit_cb cb,
                                  void *opaque)
{
    ngx_rtc_shm_source_t      *src;
    ngx_rtc_shm_retransmit_t  *r;
    ngx_rtc_shm_retransmit_slot_t *buf;
    ngx_uint_t                 start;
    ngx_uint_t                 i;
    ngx_uint_t                 idx;
    ngx_uint_t                 n;
    ngx_uint_t                 t0;
    ngx_uint_t                 r_head;
    ngx_uint_t                 r_count;
    ngx_uint_t                 r_gop_start;
    ngx_uint_t                 first_seq;
    ngx_uint_t                 first_gop;
    ngx_uint_t                 last_seq;
    ngx_int_t                  rc;

    if (NULL == ctx || NULL == name || 0 == len
            || len >= NGX_RTC_SHM_SOURCE_NAME_MAX || NULL == cb) {
        return NGX_ERROR;
    }

    /* Allocate the replay buffer outside the lock: the ring capacity is a
     * compile-time constant, so the ~1.5 MB heap allocation never holds the slab
     * pool mutex. */
    buf = ngx_alloc((size_t) NGX_RTC_SHM_RETX_RING_CAP * sizeof(*buf),
                    ngx_cycle->log);
    if (NULL == buf) {
        return NGX_ERROR;
    }

    t0 = ngx_rtc_shm_now_us();
    ngx_shmtx_lock(&ctx->pool->mutex);
    src = ngx_rtc_shm_source_locked_lookup(ctx, name, len);
    if (NULL == src || NULL == src->retransmit) {
        ngx_shmtx_unlock(&ctx->pool->mutex);
        ngx_free(buf);
        return NGX_ERROR;
    }
    r = src->retransmit;

    /* Copy the GOP out under the pool mutex, then send outside the lock so a
     * slow viewer's replay burst (blocking sendto) never stalls the publisher's
     * append. */
    n = 0;
    start = 0;
    first_seq = 0;
    first_gop = 0;
    last_seq = 0;
    if (0 != r->count) {
        start = r->head - r->count;
        if (r->gop_start > start) {
            start = r->gop_start; /* clamp to the latest IDR access unit */
        }
        for (i = start; i < r->head; i++) {
            idx = i & (r->cap - 1u);
            buf[n++] = r->slots[idx];
        }
    }
    if (0 != n) {
        first_seq = (ngx_uint_t) (((ngx_uint_t) buf[0].data[2] << 8)
                                  | (ngx_uint_t) buf[0].data[3]);
        first_gop = (ngx_uint_t) buf[0].is_gop_start;
        last_seq = (ngx_uint_t) (((ngx_uint_t) buf[n - 1u].data[2] << 8)
                                 | (ngx_uint_t) buf[n - 1u].data[3]);
    }
    r_head = r->head;
    r_count = r->count;
    r_gop_start = r->gop_start;

    /* Updated before the unlock so the writes land under the mutex, exactly
     * like the append-side ones: the stats handler reads all of them holding
     * it, which is why they need no atomics. */
    src->retx_replay_count++;
    src->retx_replay_slots += n;
    src->retx_replay_us += ngx_rtc_shm_now_us() - t0;
    ngx_shmtx_unlock(&ctx->pool->mutex);

    /* Replay-shape trace. retx_replay_slots only says a replay happened, not
     * what it was made of, and the two failures look identical from the
     * counters: a replay that does not begin at a keyframe, and one that begins
     * at a keyframe the viewer has already moved past. first_gop separates them,
     * and first_seq against the live head says how stale the replay is.
     * Enable with `error_log ... debug;`. */
    ngx_log_error(NGX_LOG_DEBUG, ngx_cycle->log, 0,
                  "ngx_rtc: shm replay head=%ui retained=%ui gop_start=%ui "
                  "start=%ui sent=%ui first_seq=%ui first_gop=%ui "
                  "last_seq=%ui",
                  r_head, r_count, r_gop_start, start, n, first_seq, first_gop,
                  last_seq);

    rc = NGX_OK;
    for (i = 0; i < n; i++) {
        rc = cb(opaque, buf[i].data, buf[i].len, buf[i].is_gop_start);
        if (NGX_OK != rc) {
            break;
        }
        /* Stop once the keyframe access unit is out: the rest of the GOP is a
         * burst a warming-up receiver is likely to lose, and the live frames
         * that follow carry on from the keyframe anyway. Same rule as the
         * same-worker replay in ngx_rtc_core.c. */
        if (NGX_RTC_RTP_HEADER_SIZE < buf[i].len
                && 0 != (buf[i].data[1] & NGX_RTC_RTP_MARKER)) {
            break;
        }
    }

    ngx_free(buf);
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
                         uint8_t media, uint8_t gop,
                         const uint8_t *rtp, uint16_t rtp_len,
                         const ngx_uint_t *sess_ids, ngx_uint_t nsess)
{
    ngx_rtc_ring_entry_t *slot;
    ngx_uint_t            idx;

    if (NULL == ring || NULL == rtp || NULL == sess_ids
            || 0 == rtp_len || rtp_len > NGX_RTC_RING_RTP_MAX
            || 0 == nsess || nsess > NGX_RTC_RING_MAX_SESSIONS) {
        return NGX_ERROR;
    }

    ngx_shmtx_lock(&ring->mtx);

    if (ring->head - ring->tail >= ring->size) {
        ngx_shmtx_unlock(&ring->mtx);
        return NGX_ERROR;
    }

    idx = (ngx_uint_t) (ring->head & ring->mask);
    slot = &ring->entries[idx];
    slot->seq = ring->head;
    slot->media = media;
    slot->gop = gop;
    slot->len = rtp_len;
    slot->nsess = nsess;
    ngx_memcpy(slot->sess, sess_ids, nsess * sizeof(ngx_uint_t));
    ngx_memcpy(slot->rtp, rtp, rtp_len);
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

