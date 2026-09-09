/*
 * ngx_rtc_core.c - RTC source / session registry
 *                  + GOP ring (plaintext RTP cache) helpers.
 *
 * Registries reuse nginx data structures instead of hand-rolled singly-linked
 * lists:
 *   - sources:  ngx_rbtree keyed by the "app/stream" name (O(log n) lookup).
 *   - sessions: global ngx_queue.
 *   - subscribers: per-source ngx_queue.
 * Single worker, so every access here is lock-free.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ngx_rtc_core.h"
#include "ngx_rtc_srtp.h"


/* Global session list (head sentinel; self-referential static initializer). */
static ngx_queue_t  ngx_rtc_sessions = { &ngx_rtc_sessions, &ngx_rtc_sessions };

/*
 * Source registry: an rbtree whose nodes are ngx_str_node_t (node.key holds a
 * crc32 of the name; insert/lookup compare by length then bytes, see
 * ngx_str_rbtree_insert_value). The sentinel is a black zero-initialised node.
 */
static ngx_rbtree_node_t   ngx_rtc_source_sentinel;
static ngx_rbtree_t        ngx_rtc_source_tree = {
    &ngx_rtc_source_sentinel,            /* root */
    &ngx_rtc_source_sentinel,            /* sentinel */
    ngx_str_rbtree_insert_value          /* insert */
};

/*
 * Session registry: an rbtree keyed by the local ICE ufrag, so the STUN
 * BindingRequest lookup is O(log n) instead of scanning the global queue.
 */
static ngx_rbtree_node_t   ngx_rtc_session_sentinel;
static ngx_rbtree_t        ngx_rtc_session_tree = {
    &ngx_rtc_session_sentinel,
    &ngx_rtc_session_sentinel,
    ngx_str_rbtree_insert_value
};


/* Recover the owning source from an rbtree node embedded via ngx_str_node_t. */
static ngx_rtc_source_t *
ngx_rtc_source_from_node(ngx_rbtree_node_t *node)
{
    return (ngx_rtc_source_t *) ((u_char *) node
                                 - offsetof(ngx_rtc_source_t, sn.node));
}

static ngx_rtc_source_t *
ngx_rtc_source_lookup(const char *name)
{
    ngx_str_node_t    *sn;
    ngx_str_t          key;
    uint32_t           hash;
    size_t             len;

    if (NULL == name) {
        return NULL;
    }

    len = strlen(name);
    if (0 == len || len >= NGX_RTC_SOURCE_NAME_MAX) {
        return NULL;
    }

    key.data = (u_char *) name;
    key.len = len;
    hash = ngx_crc32_long(key.data, key.len);

    sn = ngx_str_rbtree_lookup(&ngx_rtc_source_tree, &key, hash);
    if (NULL == sn) {
        return NULL;
    }

    return ngx_rtc_source_from_node(&sn->node);
}

static ngx_rtc_session_t *
ngx_rtc_session_from_node(ngx_rbtree_node_t *node)
{
    return (ngx_rtc_session_t *) ((u_char *) node
                                  - offsetof(ngx_rtc_session_t, sn.node));
}

ngx_rtc_source_t *
ngx_rtc_source_get(const char *name)
{
    ngx_rtc_source_t  *src;
    uint32_t           hash;
    size_t             len;

    if (NULL == name) {
        return NULL;
    }

    len = strlen(name);
    if (0 == len || len >= NGX_RTC_SOURCE_NAME_MAX) {
        return NULL;
    }

    src = ngx_rtc_source_lookup(name);
    if (NULL != src) {
        return src;
    }

    hash = ngx_crc32_long((u_char *) name, len);

    src = calloc(1, sizeof(*src));
    if (NULL == src) {
        return NULL;
    }

    ngx_memcpy(src->name, name, len);
    src->name[len] = '\0';

    src->sn.str.data = (u_char *) src->name;
    src->sn.str.len = len;
    src->sn.node.key = hash;

    ngx_queue_init(&src->subscribers);

    ngx_rbtree_insert(&ngx_rtc_source_tree, &src->sn.node);

    /* The GOP ring slots are lazily allocated on the first RTP push. */
    src->gop.capacity = 0;
    src->gop.slots = NULL;

    return src;
}

ngx_rtc_source_t *
ngx_rtc_source_find(const char *name)
{
    return ngx_rtc_source_lookup(name);
}

void
ngx_rtc_source_remove(const char *name)
{
    ngx_rtc_source_t *src;

    src = ngx_rtc_source_lookup(name);
    if (NULL == src) {
        return;
    }

    /* Subscribers hold a source pointer for the lifetime of their session, so
     * a source with viewers cannot be freed yet. The last unsubscribe will
     * retry removal when the queue is empty. */
    if (!ngx_queue_empty(&src->subscribers)) {
        return;
    }

    ngx_rbtree_delete(&ngx_rtc_source_tree, &src->sn.node);

    if (NULL != src->gop.slots) {
        free(src->gop.slots);
        src->gop.slots = NULL;
    }

    free(src);
}

ngx_rtc_source_t *
ngx_rtc_source_first(void)
{
    ngx_rbtree_node_t *node;

    if (ngx_rtc_source_tree.root == ngx_rtc_source_tree.sentinel) {
        return NULL;
    }

    node = ngx_rbtree_min(ngx_rtc_source_tree.root, ngx_rtc_source_tree.sentinel);
    return ngx_rtc_source_from_node(node);
}

ngx_rtc_source_t *
ngx_rtc_source_next(ngx_rtc_source_t *src)
{
    ngx_rbtree_node_t *node;

    if (NULL == src) {
        return ngx_rtc_source_first();
    }

    node = ngx_rbtree_next(&ngx_rtc_source_tree, &src->sn.node);
    if (NULL == node) {
        return NULL;
    }

    return ngx_rtc_source_from_node(node);
}

ngx_rtc_session_t *
ngx_rtc_session_find(const char *ufrag)
{
    ngx_str_node_t     *sn;
    ngx_str_t           key;
    uint32_t            hash;
    size_t              len;

    if (NULL == ufrag) {
        return NULL;
    }

    len = ngx_strlen(ufrag);
    if (0 == len || len >= sizeof(((ngx_rtc_session_t *)0)->ice_ufrag)) {
        return NULL;
    }

    key.data = (u_char *) ufrag;
    key.len = len;
    hash = ngx_crc32_long(key.data, key.len);

    sn = ngx_str_rbtree_lookup(&ngx_rtc_session_tree, &key, hash);
    if (NULL == sn) {
        return NULL;
    }

    return ngx_rtc_session_from_node(&sn->node);
}

ngx_rtc_session_t *
ngx_rtc_session_first(void)
{
    if (ngx_queue_empty(&ngx_rtc_sessions)) {
        return NULL;
    }

    return ngx_queue_data(ngx_queue_head(&ngx_rtc_sessions),
                          ngx_rtc_session_t, queue);
}

ngx_rtc_session_t *
ngx_rtc_session_next(ngx_rtc_session_t *sess)
{
    ngx_queue_t *q;

    if (NULL == sess) {
        return ngx_rtc_session_first();
    }

    q = ngx_queue_next(&sess->queue);
    if (q == ngx_queue_sentinel(&ngx_rtc_sessions)) {
        return NULL;
    }

    return ngx_queue_data(q, ngx_rtc_session_t, queue);
}

void
ngx_rtc_session_add(ngx_rtc_session_t *sess)
{
    size_t  len;

    if (NULL == sess) {
        return;
    }

    len = ngx_strlen(sess->ice_ufrag);
    if (0 == len) {
        return;
    }

    ngx_queue_insert_head(&ngx_rtc_sessions, &sess->queue);

    sess->sn.str.data = (u_char *) sess->ice_ufrag;
    sess->sn.str.len = len;
    sess->sn.node.key = ngx_crc32_long(sess->sn.str.data, len);
    ngx_rbtree_insert(&ngx_rtc_session_tree, &sess->sn.node);
}

void
ngx_rtc_session_remove(ngx_rtc_session_t *sess)
{
    if (NULL == sess) {
        return;
    }

    if (0 != sess->sn.str.len) {
        ngx_rbtree_delete(&ngx_rtc_session_tree, &sess->sn.node);
        sess->sn.str.len = 0;
    }

    ngx_queue_remove(&sess->queue);
}

void
ngx_rtc_source_subscribe(ngx_rtc_source_t *src, ngx_rtc_session_t *sess)
{
    if (NULL == src || NULL == sess) {
        return;
    }

    sess->source = src;
    ngx_queue_insert_head(&src->subscribers, &sess->sub_queue);
}

void
ngx_rtc_source_unsubscribe(ngx_rtc_source_t *src, ngx_rtc_session_t *sess)
{
    if (NULL == src || NULL == sess) {
        return;
    }
    if (sess->source != src) {
        return;
    }

    ngx_queue_remove(&sess->sub_queue);
    sess->source = NULL;

    if (ngx_queue_empty(&src->subscribers) && 0 == src->publishing) {
        ngx_rtc_source_remove(src->name);
    }
}

ngx_rtc_session_t *
ngx_rtc_source_first_subscriber(ngx_rtc_source_t *src)
{
    if (NULL == src || ngx_queue_empty(&src->subscribers)) {
        return NULL;
    }

    return ngx_queue_data(ngx_queue_head(&src->subscribers),
                          ngx_rtc_session_t, sub_queue);
}

ngx_rtc_session_t *
ngx_rtc_source_next_subscriber(ngx_rtc_source_t *src, ngx_rtc_session_t *sess)
{
    ngx_queue_t *q;

    if (NULL == src || NULL == sess) {
        return NULL;
    }

    q = ngx_queue_next(&sess->sub_queue);
    if (q == ngx_queue_sentinel(&src->subscribers)) {
        return NULL;
    }

    return ngx_queue_data(q, ngx_rtc_session_t, sub_queue);
}

int
ngx_rtc_session_send_rtp(ngx_rtc_session_t *sess, const uint8_t *rtp, uint32_t len)
{
    ngx_connection_t *c;
    uint32_t          ssrc;
    uint8_t           session_pt;
    int               n;
    ssize_t           sent;

    if (NULL == sess || NULL == sess->conn) {
        return 0;
    }
    if (!ngx_rtc_session_fsm_is_ready(&sess->fsm)) {
        return 0;
    }
    if (0 == len || len > NGX_RTC_MAX_RTP_PKT) {
        return 0;
    }

    c = (ngx_connection_t *)sess->conn;
    ngx_memcpy(sess->cipher, rtp, len);

    /* Rewrite the RTP payload-type byte to the PT this session negotiated in
     * its offer (RFC 3264). The source broadcasts one plaintext stream tagged
     * with the first viewer's PT; applying each session's PT here, right before
     * SRTP, lets heterogeneous viewers negotiate their own PT (the 1-byte
     * equivalent of SRS rebuild_packet). SSRC is left as the source SSRC, which
     * is what RTCP feedback / sender reports key on. */
    if (NGX_RTC_RTP_HEADER_SIZE <= len && NULL != sess->source) {
        ssrc = ((uint32_t)sess->cipher[8] << 24)
             | ((uint32_t)sess->cipher[9] << 16)
             | ((uint32_t)sess->cipher[10] << 8)
             | (uint32_t)sess->cipher[11];

        session_pt = 0;
        if (ssrc == sess->source->video_ssrc) {
            session_pt = sess->video_pt;
        } else if (ssrc == sess->source->audio_ssrc) {
            session_pt = sess->audio_pt;
        }

        if (0 != session_pt) {
            sess->cipher[1] = (uint8_t)((sess->cipher[1] & 0x80u)
                                        | (session_pt & 0x7fu));
        }
    }

    /* transport-wide-cc header extension (RFC 8285 one-byte form, 8 bytes after
     * the 12-byte fixed header). Stamped on every real send of a medium the
     * session negotiated - retransmissions too, since each carries its own
     * fresh transport sequence (the RTP seq in bytes 2..3 is untouched). The
     * peer enables transport-cc only when the answer echoed the extmap id. */
    if ((NULL != sess->source) && (NGX_RTC_RTP_HEADER_SIZE <= len)) {
        uint32_t ssrc2 = ((uint32_t)sess->cipher[8] << 24)
                       | ((uint32_t)sess->cipher[9] << 16)
                       | ((uint32_t)sess->cipher[10] << 8)
                       | (uint32_t)sess->cipher[11];
        uint8_t  ext_id = 0;
        uint32_t idx;
        uint16_t tseq;

        if (ssrc2 == sess->source->video_ssrc) {
            ext_id = sess->twcc_video_ext;
        } else if (ssrc2 == sess->source->audio_ssrc) {
            ext_id = sess->twcc_audio_ext;
        }

        if ((0 != ext_id)
                && ((uint32_t)len + 8u + NGX_RTC_SRTP_TAG_LEN
                    <= NGX_RTC_CIPHER_CAP)) {
            tseq = sess->twcc_seq++;

            /* Shift the payload right by 8 (back-to-front so overlap is safe). */
            for (idx = len; idx > NGX_RTC_RTP_HEADER_SIZE; idx--) {
                sess->cipher[idx + 7u] = sess->cipher[idx - 1u];
            }

            sess->cipher[0] = (uint8_t)(sess->cipher[0] | 0x10u); /* X = 1 */
            sess->cipher[12] = 0xBE;
            sess->cipher[13] = 0xDE;
            sess->cipher[14] = 0x00;
            sess->cipher[15] = 0x01;               /* one 32-bit word */
            sess->cipher[16] = (uint8_t)((uint8_t)(ext_id << 4) | 0x01u);
            sess->cipher[17] = (uint8_t)(tseq >> 8);
            sess->cipher[18] = (uint8_t)(tseq & 0xFFu);
            sess->cipher[19] = 0x00;               /* pad to 32-bit alignment */

            len += 8u;
        }
    }

    n = (int)len;
    if (ngx_rtc_srtp_protect_rtp(&sess->srtp, sess->cipher, &n) != 0) {
        return 0;
    }

    sent = c->send(c, sess->cipher, (size_t)n);
    if (sent == NGX_ERROR) {
        sess->send_failed++;
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, ngx_socket_errno,
                       "ngx_rtc: RTP send failed");
        return 0;
    } else if (sent == NGX_AGAIN) {
        /* UDP socket send buffer is full: drop the datagram and count it.
         * Queueing real-time media would only add latency and then still have
         * to drop it, so let the client recover through NACK/PLI. */
        sess->send_eagain++;
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, ngx_socket_errno,
                       "ngx_rtc: RTP send would block");
        return 0;
    } else if (sent != (ssize_t)n) {
        sess->send_failed++;
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, ngx_socket_errno,
                       "ngx_rtc: RTP send short write");
        return 0;
    }

    return 1;
}

/* Return the 16-bit RTP sequence number carried in a plaintext RTP header. */
static uint16_t
ngx_rtc_rtp_seq_from_data(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[2] << 8) | (uint16_t)data[3]);
}

void
ngx_rtc_rtp_ring_push(ngx_rtc_rtp_ring_t *r, const uint8_t *rtp, uint32_t len,
                      uint8_t is_gop_start)
{
    ngx_rtc_rtp_cache_slot_t *slot;
    uint32_t                  idx;

    if (NULL == r || NULL == rtp || 0 == len || len > NGX_RTC_MAX_RTP_PKT) {
        return;
    }

    if (0 == ngx_rtc_rtp_ring_reserve(r, NGX_RTC_GOP_RING_CAP)) {
        return; /* cache unavailable; live broadcast still works */
    }

    idx = r->head & (r->capacity - 1u);
    slot = &r->slots[idx];

    ngx_memcpy(slot->data, rtp, len);
    slot->len = (uint16_t)len;
    slot->seq = r->head;
    slot->is_gop_start = (uint8_t)(0 != is_gop_start ? 1 : 0);
    slot->rtx_gen = 0; /* a fresh packet has never been retransmitted */

    if (0 != is_gop_start) {
        r->gop_start = r->head;
    }

    r->head++;
    if (r->count < r->capacity) {
        r->count++;
    }
}

int
ngx_rtc_rtp_ring_reserve(ngx_rtc_rtp_ring_t *r, uint32_t capacity)
{
    if (NULL == r || 0 == capacity
            || 0 != (capacity & (capacity - 1u))) {
        return 0; /* invalid args or non power-of-two capacity */
    }

    if (NULL != r->slots) {
        return 1; /* already sized (capacity stays as first reserved) */
    }

    r->slots = calloc(capacity, sizeof(*r->slots));
    if (NULL == r->slots) {
        return 0;
    }

    r->capacity = capacity;
    return 1;
}

void
ngx_rtc_rtp_ring_replay(ngx_rtc_rtp_ring_t *r, ngx_rtc_session_t *sess)
{
    uint32_t i;
    uint32_t start;
    uint32_t idx;

    if (NULL == r || NULL == sess || 0 == r->count || NULL == r->slots) {
        return;
    }

    start = r->head - r->count; /* oldest retained */
    if (r->gop_start > start) {
        start = r->gop_start;   /* clamp to the latest IDR access unit */
    }

    /* Replay synchronously. A paced (timer-driven) replay interleaves old GOP
     * packets with live packets because the bridge keeps broadcasting while the
     * timer ticks, which delivers RTP sequence numbers out of order and corrupts
     * the decoder (frame_num jump). The GOP is small on a LAN, so the synchronous
     * burst is acceptable. */
    for (i = start; i < r->head; i++) {
        idx = i & (r->capacity - 1u);
        if (ngx_rtc_session_send_rtp(sess, r->slots[idx].data, r->slots[idx].len) == 0) {
            /* Stop bursting into a full socket: the client is not draining fast
             * enough and the rest of the GOP would just be dropped. The next
             * NACK/PLI recovers what was lost. */
            break;
        }
    }
}

int32_t
ngx_rtc_rtp_ring_get(ngx_rtc_rtp_ring_t *r, uint16_t rtp_seq,
                     const uint8_t **out, uint16_t *out_len)
{
    uint32_t  start;
    uint16_t  start_seq;
    uint16_t  dist;
    uint32_t  idx;

    if (NULL == r || NULL == out || NULL == out_len) {
        return NGX_RTC_ERR_INVALID;
    }
    if (0 == r->count || NULL == r->slots) {
        return NGX_RTC_ERR_PARSE;
    }

    start = r->head - r->count;
    idx = start & (r->capacity - 1u);
    start_seq = ngx_rtc_rtp_seq_from_data(r->slots[idx].data);

    /* Modular 16-bit distance from the oldest retained packet. */
    dist = (uint16_t)(rtp_seq - start_seq);
    if ((uint32_t)dist >= r->count) {
        return NGX_RTC_ERR_PARSE; /* outside the retained window */
    }

    idx = (start + (uint32_t)dist) & (r->capacity - 1u);
    if (ngx_rtc_rtp_seq_from_data(r->slots[idx].data) != rtp_seq) {
        return NGX_RTC_ERR_PARSE; /* safety: slot does not match */
    }

    *out = r->slots[idx].data;
    *out_len = r->slots[idx].len;
    return NGX_RTC_OK;
}


/* Internal: locate a ring slot by its 16-bit RTP sequence number. NULL when the
 * ring is empty or the sequence is outside the retained window. Shared by the
 * ring_get NACK lookup and the per-session RTX retransmitter. */
static ngx_rtc_rtp_cache_slot_t *
ngx_rtc_rtp_ring_locate(ngx_rtc_rtp_ring_t *r, uint16_t rtp_seq)
{
    ngx_rtc_rtp_cache_slot_t *slot;
    uint32_t                  start;
    uint16_t                  start_seq;
    uint16_t                  dist;
    uint32_t                  idx;

    if (NULL == r || 0 == r->count || NULL == r->slots) {
        return NULL;
    }

    start = r->head - r->count;
    idx = start & (r->capacity - 1u);
    start_seq = ngx_rtc_rtp_seq_from_data(r->slots[idx].data);

    dist = (uint16_t)(rtp_seq - start_seq);
    if ((uint32_t)dist >= r->count) {
        return NULL; /* outside the retained window */
    }

    idx = (start + (uint32_t)dist) & (r->capacity - 1u);
    slot = &r->slots[idx];
    if (ngx_rtc_rtp_seq_from_data(slot->data) != rtp_seq) {
        return NULL; /* safety: slot does not match */
    }

    return slot;
}


void
ngx_rtc_session_rtx_push(ngx_rtc_session_t *sess, const uint8_t *rtp,
                         uint32_t len, uint8_t is_gop_start)
{
    if (NULL == sess || NULL == rtp || 0 == len
            || len > NGX_RTC_MAX_RTP_PKT) {
        return;
    }

    if (0 == ngx_rtc_rtp_ring_reserve(&sess->rtx, NGX_RTC_RTX_RING_CAP)) {
        return; /* cache unavailable; live send still proceeds */
    }

    ngx_rtc_rtp_ring_push(&sess->rtx, rtp, len, is_gop_start);
}


int32_t
ngx_rtc_session_rtx_retransmit(ngx_rtc_session_t *sess, uint16_t seq)
{
    ngx_rtc_rtp_cache_slot_t *slot;

    if (NULL == sess) {
        return NGX_RTC_ERR_INVALID;
    }

    slot = ngx_rtc_rtp_ring_locate(&sess->rtx, seq);
    if (NULL == slot) {
        return NGX_RTC_ERR_PARSE; /* outside this session's cache window */
    }
    if (slot->rtx_gen == sess->rtx_gen) {
        return NGX_RTC_ERR_PARSE; /* already retransmitted this NACK window */
    }

    if (0 == ngx_rtc_session_send_rtp(sess, slot->data, slot->len)) {
        return NGX_RTC_ERR_PARSE; /* socket not draining; leave the budget */
    }

    slot->rtx_gen = sess->rtx_gen;
    return NGX_RTC_OK;
}


/* PLI fallback: replay the latest decodable GOP from this session's own RTX
 * ring. Cross-worker safe (the producer-side source GOP ring may be empty on
 * this worker). */
void
ngx_rtc_session_rtx_replay_gop(ngx_rtc_session_t *sess)
{
    ngx_rtc_rtp_ring_t *r;
    uint32_t            i;
    uint32_t            start;
    uint32_t            idx;

    if (NULL == sess) {
        return;
    }

    r = &sess->rtx;
    if (0 == r->count || NULL == r->slots) {
        return;
    }

    start = r->head - r->count;
    if (r->gop_start > start) {
        start = r->gop_start; /* clamp to the latest IDR access unit */
    }

    for (i = start; i < r->head; i++) {
        idx = i & (r->capacity - 1u);
        if (0 == ngx_rtc_session_send_rtp(sess, r->slots[idx].data,
                                          r->slots[idx].len)) {
            break; /* socket full: stop, the next PLI/NACK recovers */
        }
    }
}


/* Fast-start on subscribe. If the producer lives in this worker, replay the
 * source GOP ring and warm the session RTX ring on the way so an immediate NACK
 * is answerable; otherwise replay this session's own RTX ring if it has data. */
void
ngx_rtc_session_rtx_faststart(ngx_rtc_session_t *sess)
{
    ngx_rtc_rtp_ring_t *g;
    uint32_t            i;
    uint32_t            start;
    uint32_t            idx;

    if (NULL == sess || NULL == sess->source) {
        return;
    }

    g = &sess->source->gop;
    if (0 == sess->rtx.count && 0 != g->count && NULL != g->slots) {
        start = g->head - g->count;
        if (g->gop_start > start) {
            start = g->gop_start;
        }

        for (i = start; i < g->head; i++) {
            idx = i & (g->capacity - 1u);
            ngx_rtc_session_rtx_push(sess, g->slots[idx].data,
                                     g->slots[idx].len,
                                     g->slots[idx].is_gop_start);
            if (0 == ngx_rtc_session_send_rtp(sess, g->slots[idx].data,
                                              g->slots[idx].len)) {
                break;
            }
        }
        return;
    }

    ngx_rtc_session_rtx_replay_gop(sess);
}


void
ngx_rtc_session_rtx_free(ngx_rtc_session_t *sess)
{
    if (NULL == sess || NULL == sess->rtx.slots) {
        return;
    }

    free(sess->rtx.slots);
    sess->rtx.slots = NULL;
    sess->rtx.capacity = 0;
    sess->rtx.count = 0;
}
