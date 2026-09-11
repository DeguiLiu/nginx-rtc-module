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


/*
 * Compile-time defaults for the runtime tunables (see ngx_rtc_core.h). The
 * nginx side replaces this snapshot from the rtc_* directives in
 * ngx_rtc_core_init_conf() before the workers fork; the host unit tests never
 * install one, so they keep seeing exactly these values.
 */
static const ngx_rtc_tunables_t  ngx_rtc_default_tunables = {
    NGX_RTC_JITTER_TIMEOUT_MS,
    NGX_RTC_NACK_WINDOW_MS,
    NGX_RTC_NACK_WINDOW_MAX_MS,
    NGX_RTC_EAGAIN_STREAK_MAX,
    NGX_RTC_GOP_RING_CAP
};

static const ngx_rtc_tunables_t *ngx_rtc_tunables = &ngx_rtc_default_tunables;

static ngx_log_t *ngx_rtc_session_log(const ngx_rtc_session_t *sess);


void
ngx_rtc_core_set_tunables(const ngx_rtc_tunables_t *t)
{
    /* NULL restores the compile-time defaults; the tests rely on that to undo
     * an override. The caller owns the storage for a non-NULL snapshot
     * (config-time, cycle lifetime), which is why this must not be called
     * after the workers fork. */
    ngx_rtc_tunables = (NULL != t) ? t : &ngx_rtc_default_tunables;
}


const ngx_rtc_tunables_t *
ngx_rtc_core_tunables(void)
{
    return ngx_rtc_tunables;
}


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

    len = ngx_strlen(name);
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

    len = ngx_strlen(name);
    if (0 == len || len >= NGX_RTC_SOURCE_NAME_MAX) {
        return NULL;
    }

    src = ngx_rtc_source_lookup(name);
    if (NULL != src) {
        return src;
    }

    hash = ngx_crc32_long((u_char *) name, len);

    src = ngx_calloc(sizeof(*src), ngx_cycle->log);
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
    if (0 == ngx_queue_empty(&src->subscribers)) {
        return;
    }

    /* A live publisher holds a session->source pointer that would dangle; never
     * free a source that is still publishing. The publisher's teardown clears
     * publishing before removal (defense-in-depth against a stale close). */
    if (0 != src->publishing) {
        return;
    }

    /* Any session still pointing here keeps the struct alive, whether or not it
     * is in the subscriber queue. A WHIP publisher assigns sess->source
     * directly, and the reaper closes sessions one at a time: without this
     * check the first close frees the source and the next close in the same
     * pass dereferences sess->source (SIGSEGV in ngx_rtc_stream_session_close).
     * Checked here rather than at each caller so no path can bypass it. */
    if (0 != ngx_rtc_source_has_holder(src)) {
        return;
    }

    /* A live AAC->Opus transcode context belongs to the bridge module, which
     * destroys it on publisher teardown (ngx_rtmp_rtc_release_publish). Freeing
     * the source with one still attached would strand the transcoder thread and
     * its queues with no owner left to stop them, so refuse and say so. This is
     * unreachable while that teardown stays unconditional; it exists so a later
     * path that frees a source cannot quietly reintroduce the leak.
     * ngx_log_stderr, not ngx_log_error: this unit owns no ngx_log_t (and is
     * also linked into the host unit tests, which stub the logging out). */
    if (NULL != src->audio_ctx) {
        ngx_log_stderr(0, "ngx_rtc_core: source=%s still owns an audio worker, "
                          "refusing to free it", src->name);
        return;
    }

    ngx_rbtree_delete(&ngx_rtc_source_tree, &src->sn.node);

    if (NULL != src->gop.slots) {
        ngx_free(src->gop.slots);
        src->gop.slots = NULL;
    }

    ngx_free(src);
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

ngx_rtc_session_t *
ngx_rtc_session_find_by_id(ngx_uint_t id)
{
    ngx_rtc_session_t *sess;

    for (sess = ngx_rtc_session_first(); NULL != sess;
         sess = ngx_rtc_session_next(sess)) {
        if (sess->id == id) {
            return sess;
        }
    }

    return NULL;
}

void
ngx_rtc_session_fsm_report_unhandled(ngx_rtc_hsm_t *sm,
                                     const ngx_rtc_hsm_event_t *event)
{
    /* Both are unused in a build without NGX_DEBUG: ngx_log_debug2 compiles
     * away, so keep the parameters explicitly consumed for -Wunused-parameter. */
    (void) sm;
    (void) event;

    /* Debug level: an unhandled event is usually benign (a keepalive class the
     * state deliberately ignores), but a silent no-op hides the ones that are
     * not, and a stalled session is exactly what this traces. */
    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ngx_cycle->log, 0,
                   "ngx_rtc: unhandled session event id=%ui state=%s",
                   (ngx_uint_t) event->id,
                   ngx_rtc_session_fsm_get_state_name(sm));
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

/*
 * Does any session still point at this source? Sessions reach a source two
 * ways: ngx_rtc_source_subscribe links them into src->subscribers, and a
 * publisher assignment (sess->source = src, e.g. a WHIP session) does not.
 * The second kind is invisible to the subscriber queue, so freeing on "queue
 * empty" alone strands it. Returns 1 when a holder exists.
 */
ngx_uint_t
ngx_rtc_source_has_holder(const ngx_rtc_source_t *src)
{
    ngx_rtc_session_t *sess;

    if (NULL == src) {
        return 0;
    }

    for (sess = ngx_rtc_session_first(); NULL != sess;
         sess = ngx_rtc_session_next(sess)) {
        if (sess->source == src) {
            return 1;
        }
    }

    return 0;
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

    /*
     * Idempotent unlink. Two teardown paths can reach here for one session
     * (the reaper's close and the RTCP timer's subscriber walk), and
     * ngx_queue_remove only rewires the neighbours: in a release build
     * (NGX_DEBUG off) it leaves the removed node's own next/prev dangling, so
     * a second remove dereferences stale memory and faults. A self-linked
     * sub_queue means "not on any list"; restore that after unlinking so the
     * operation can be repeated safely. The same guard covers a session that
     * was never subscribed (e.g. a WHIP publisher, which sets sess->source
     * without calling subscribe).
     *
     * The NULL test is not redundant. Every session construction path today
     * calls ngx_queue_init straight after ngx_calloc, so a zeroed link does not
     * reach here -- but "self-linked" is a claim about an initialised link, and
     * a zeroed one is neither self-linked nor enqueued. The comparison against
     * &sess->sub_queue accepts it and ngx_queue_remove then dereferences its
     * NULL next on the first statement.
     */
    if (NULL != sess->sub_queue.next
            && sess->sub_queue.next != &sess->sub_queue) {
        ngx_queue_remove(&sess->sub_queue);
    }
    ngx_queue_init(&sess->sub_queue);
    sess->source = NULL;

    /*
     * Only the empty subscriber queue is not enough to free: a session that
     * never subscribed still holds ->source (a WHIP publisher sets it directly)
     * and would read the freed struct on its own close. The reaper walks
     * sessions and closes them one by one, so the first close can free the
     * source out from under a later one in the same pass. Scan for any other
     * holder and let the last one out do the free.
     */
    if (ngx_queue_empty(&src->subscribers) && 0 == src->publishing
            && 0 == ngx_rtc_source_has_holder(src)) {
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
ngx_rtc_session_send_rtp(ngx_rtc_session_t *sess, const uint8_t *rtp, uint32_t len,
                         uint8_t is_gop_start)
{
    ngx_connection_t *c;
    uint32_t          ssrc;
    uint8_t           is_video;
    uint8_t           is_gop_idr;
    uint8_t           is_keyframe;
    uint8_t           session_pt;
    uint8_t           twcc_ext_id;
    uint16_t          tseq;
    uint8_t           twcc_stamped;
    int32_t           admit_rc;
    int               n;
    ssize_t           sent;

    if (NULL == sess || NULL == sess->conn) {
        return 0;
    }
    if (0 == ngx_rtc_session_fsm_is_ready(&sess->fsm)) {
        return 0;
    }
    if (0 == len || len > NGX_RTC_MAX_RTP_PKT) {
        return 0;
    }

    /* Classify video by SSRC. Video-only backpressure must never drop audio:
     * the AAC->Opus transcoder needs a gapless input, so a slow audio consumer
     * is left to the vring byte-capacity backpressure instead of this flag. */
    is_video = 0;
    if (NGX_RTC_RTP_HEADER_SIZE <= len && NULL != sess->source) {
        ssrc = ((uint32_t)rtp[8] << 24) | ((uint32_t)rtp[9] << 16)
             | ((uint32_t)rtp[10] << 8) | (uint32_t)rtp[11];
        if (ssrc == sess->source->video_ssrc) {
            is_video = 1;
        }
    }
    is_gop_idr = (0 != is_video && 0 != is_gop_start) ? 1 : 0;

    /* A keyframe access unit is exempt from the pacer as a whole. is_gop_start
     * marks only the SPS/PPS packet that opens it, so exempting that packet
     * alone still lets the pacer refuse the IDR fragments carrying the picture
     * -- a parameter set with no image, the one state the viewer cannot leave on
     * its own. The RTP marker bit closes the unit; the packet cap bounds a
     * source that never sets one. */
    is_keyframe = is_gop_idr;
    if (0 == is_keyframe && 0 != is_video && 0 != sess->keyframe_open
            && sess->keyframe_pkts < NGX_RTC_KEYFRAME_MAX_PKTS) {
        is_keyframe = 1;
    }

    /* Video backpressure: after a socket send failure, drop video packets until
     * the next IDR access unit so a slow client recovers to a clean keyframe
     * instead of decoding a torn GOP. The flag is cleared only once a GOP start
     * is actually sent (not merely attempted), so a keyframe paced out of a full
     * socket cannot resume the stream mid-GOP. */
    if (is_video && sess->drop_until_gop && 0 == is_gop_start) {
        sess->drop_gop++;
        return 0;
    }

    /* Send pacing: charge the token bucket, drop on overrun so the client
     * recovers via NACK/PLI rather than queueing (queueing adds latency, the
     * opposite of a low-latency live stream). Live, replay, and RTX sends all
     * funnel through here and spend the same link budget. pacer_target_bps == 0
     * means the pacer was never armed (pacer_init not called), so pass through
     * unpaced — this keeps host tests that drive the send path working.
     *
     * A keyframe access unit is admitted even when the budget is spent. The
     * encoder is an external RTMP source that cannot be told to slow down, so
     * refusing packets never relieves congestion — it only decides what the
     * viewer loses. A refused keyframe is the one loss the viewer cannot
     * recover from on its own: it decodes nothing until a later IDR, asks for
     * one with a PLI, and that reply is refused too. The unit is bounded by
     * NGX_RTC_KEYFRAME_MAX_PKTS, so a weak link is not handed an unbounded
     * burst; when the bucket covers it the packet is charged normally. */
    if (0 != sess->pacer_target_bps) {
        admit_rc = ngx_rtc_session_pacer_admit(sess, len,
                                               (uint64_t) ngx_current_msec);
        if (NGX_RTC_OK != admit_rc && 0 == is_keyframe) {
            sess->drop_pacer++;
            return 0;
        }
    }

    c = (ngx_connection_t *)sess->conn;

    /* Resolve the session PT and the transport-cc extension id up front so the
     * plaintext is copied straight to its final offset: with a TWCC extension
     * the RTP header (12 B) stays at 0, the 8-byte extension lands at 12 and the
     * payload moves to 20. This replaces the old copy-then-shift, which rewrote
     * the whole payload byte by byte on every packet. SSRC is left as the source
     * SSRC (RTCP feedback / sender reports key on it). */
    session_pt = 0;
    twcc_ext_id = 0;
    twcc_stamped = 0;
    if (NGX_RTC_RTP_HEADER_SIZE <= len && NULL != sess->source) {
        ssrc = ((uint32_t)rtp[8] << 24) | ((uint32_t)rtp[9] << 16)
             | ((uint32_t)rtp[10] << 8) | (uint32_t)rtp[11];

        if (ssrc == sess->source->video_ssrc) {
            session_pt = sess->video_pt;
            twcc_ext_id = sess->twcc_video_ext;
        } else if (ssrc == sess->source->audio_ssrc) {
            session_pt = sess->audio_pt;
            twcc_ext_id = sess->twcc_audio_ext;
        }
    }

    /* transport-wide-cc header extension (RFC 8285 one-byte form, 8 bytes after
     * the 12-byte fixed header). Stamped on every real send of a medium the
     * session negotiated - retransmissions too, since each carries its own
     * fresh transport sequence (the RTP seq in bytes 2..3 is untouched). The
     * peer enables transport-cc only when the answer echoed the extmap id. */
    if (0 != twcc_ext_id
            && (uint32_t)len + 8u + NGX_RTC_SRTP_TAG_LEN > NGX_RTC_CIPHER_CAP) {
        twcc_ext_id = 0; /* no room for the extension: fall back to a plain copy */
    }

    if (0 != twcc_ext_id) {
        ngx_memcpy(sess->cipher, rtp, NGX_RTC_RTP_HEADER_SIZE);
        ngx_memcpy(sess->cipher + NGX_RTC_RTP_HEADER_SIZE + 8u,
                   rtp + NGX_RTC_RTP_HEADER_SIZE,
                   len - NGX_RTC_RTP_HEADER_SIZE);

        tseq = sess->twcc_seq++;
        twcc_stamped = 1;
        sess->cipher[0] = (uint8_t)(sess->cipher[0] | 0x10u); /* X = 1 */
        sess->cipher[12] = 0xBE;
        sess->cipher[13] = 0xDE;
        sess->cipher[14] = 0x00;
        sess->cipher[15] = 0x01;               /* one 32-bit word */
        sess->cipher[16] = (uint8_t)((uint8_t)(twcc_ext_id << 4) | 0x01u);
        sess->cipher[17] = (uint8_t)(tseq >> 8);
        sess->cipher[18] = (uint8_t)(tseq & 0xFFu);
        sess->cipher[19] = 0x00;               /* pad to 32-bit alignment */

        len += 8u;
    } else {
        ngx_memcpy(sess->cipher, rtp, len);
    }

    /* Rewrite the RTP payload-type byte to the PT this session negotiated in
     * its offer (RFC 3264). The source broadcasts one plaintext stream tagged
     * with the first viewer's PT; applying each session's PT here, right before
     * SRTP, lets heterogeneous viewers negotiate their own PT (the 1-byte
     * equivalent of SRS rebuild_packet). */
    if (0 != session_pt) {
        sess->cipher[1] = (uint8_t)((sess->cipher[1] & 0x80u)
                                    | (session_pt & 0x7fu));
    }

    n = (int)len;
    if (ngx_rtc_srtp_protect_rtp(&sess->srtp, sess->cipher, &n) != 0) {
        /* Nothing reached the socket, so give the transport-wide sequence back
         * (see the send branch below for why). Counted separately from the
         * socket failures: this is the one abandon point that used to be silent,
         * and a silent one here reads to the peer as pure path loss. */
        sess->srtp_failed++;
        if (0 != twcc_stamped) {
            sess->twcc_seq--;
        }
        return 0;
    }

    sent = c->send(c, sess->cipher, (size_t)n);
    if (sent != (ssize_t)n) {
        /* The packet was abandoned by this process, not by the path, so it must
         * not keep the transport-wide sequence it was stamped with. The
         * receiver would report the resulting gap as loss, and the loss-driven
         * rate controller would then cut the send rate because of a packet that
         * never left — the sender reporting its own failures as congestion. */
        if (0 != twcc_stamped) {
            sess->twcc_seq--;
        }
    }
    if (sent == NGX_ERROR) {
        sess->send_failed++;
        if (is_video) {
            sess->drop_until_gop = 1;
        }
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, ngx_socket_errno,
                       "ngx_rtc: RTP send failed");
        return 0;
    } else if (sent == NGX_AGAIN) {
        /* UDP socket send buffer is full: drop the datagram and count it.
         * Queueing real-time media would only add latency and then still have
         * to drop it, so let the client recover through NACK/PLI. A single
         * full buffer is transient, so video only falls back to the next IDR
         * once the EAGAINs are consecutive and numerous enough to mean the
         * client is genuinely behind. A run interrupted by an idle gap (a
         * still picture sends nothing) restarts: it would otherwise be
         * inherited by a later, unrelated burst. */
        sess->send_eagain++;
        if (is_video) {
            if (sess->send_eagain_streak > 0
                    && (ngx_msec_t) (ngx_current_msec - sess->send_eagain_ms)
                           >= (ngx_msec_t) NGX_RTC_EAGAIN_STREAK_IDLE_MS) {
                sess->send_eagain_streak = 0;
            }
            sess->send_eagain_ms = ngx_current_msec;
            if (++sess->send_eagain_streak
                    >= ngx_rtc_core_tunables()->eagain_streak_max) {
                sess->send_eagain_streak = 0; /* armed: the next run starts over */
                sess->drop_until_gop = 1;
            }
        }
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, ngx_socket_errno,
                       "ngx_rtc: RTP send would block");
        return 0;
    } else if (sent != (ssize_t)n) {
        sess->send_failed++;
        if (is_video) {
            sess->drop_until_gop = 1;
        }
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, ngx_socket_errno,
                       "ngx_rtc: RTP send short write");
        return 0;
    }

    /* A video GOP start that actually reached the socket resumes delivery, and
     * opens its keyframe access unit; the RTP marker bit closes it. */
    if (is_video) {
        sess->send_eagain_streak = 0;
        if (0 != is_gop_start) {
            sess->drop_until_gop = 0;
            sess->keyframe_open = 1;
            sess->keyframe_pkts = 0;
        }
        if (0 != sess->keyframe_open) {
            sess->keyframe_pkts++;
            if (NGX_RTC_RTP_HEADER_SIZE <= len
                    && 0 != (rtp[1] & NGX_RTC_RTP_MARKER)) {
                sess->keyframe_open = 0;
            }
        }
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

    if (0 == ngx_rtc_rtp_ring_reserve(r,
                                      ngx_rtc_core_tunables()->gop_ring_slots)) {
        return; /* cache unavailable; live broadcast still works */
    }

    idx = r->head & (r->capacity - 1u);
    slot = &r->slots[idx];

    ngx_memcpy(slot->data, rtp, len);
    slot->len = (uint16_t)len;
    slot->is_gop_start = (uint8_t)(0 != is_gop_start ? 1 : 0);

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

    /* ngx_calloc takes one size, so the element count is folded in here;
     * keep the overflow check calloc used to do for us. */
    if (capacity > (uint32_t) (SIZE_MAX / sizeof(*r->slots))) {
        return 0;
    }

    r->slots = ngx_calloc((size_t) capacity * sizeof(*r->slots),
                          ngx_cycle->log);
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
    uint32_t sent;

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
     * the decoder (frame_num jump).
     *
     * Stop once the keyframe access unit has been sent. The ring retains every
     * frame of the current GOP, but only the keyframe is needed to start
     * decoding: the live frames that follow carry on from it. Sending the whole
     * GOP instead is a burst of several hundred packets arriving while a new
     * viewer's receive path is still warming up, so the packets most likely to
     * be lost are exactly the ones the first frame needs. The RTP marker bit
     * closes the access unit. */
    sent = 0;
    for (i = start; i < r->head; i++) {
        idx = i & (r->capacity - 1u);
        if (ngx_rtc_session_send_rtp(sess, r->slots[idx].data,
                                     r->slots[idx].len,
                                     r->slots[idx].is_gop_start) == 0) {
            /* Stop bursting into a full socket: the client is not draining fast
             * enough and the rest of the keyframe would just be dropped. The
             * next NACK/PLI recovers what was lost. */
            break;
        }
        sent++;
        if (NGX_RTC_RTP_HEADER_SIZE < r->slots[idx].len
                && 0 != (r->slots[idx].data[1] & NGX_RTC_RTP_MARKER)) {
            break;
        }
    }

    /* The same-worker replay has no counter of its own, so this trace is the
     * only way to tell whether a late subscriber was handed a keyframe at all,
     * and how much of the ring it took. Enable with `error_log ... debug;`. */
    ngx_log_error(NGX_LOG_DEBUG, ngx_rtc_session_log(sess), 0,
                  "ngx_rtc: replay head=%uD retained=%uD start=%uD sent=%uD "
                  "gop_start=%uD",
                  r->head, r->count, start, sent, r->gop_start);
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


int
ngx_rtc_source_gop_ready(const ngx_rtc_source_t *src)
{
    return (NULL != src && NULL != src->gop.slots && 0 != src->gop.count) ? 1 : 0;
}

void
ngx_rtc_session_nack_reset(ngx_rtc_session_t *sess)
{
    if (NULL == sess) {
        return;
    }

    sess->nack_seen_count = 0;
}

int
ngx_rtc_session_nack_budget_take(ngx_rtc_session_t *sess)
{
    if (NULL == sess) {
        return 0;
    }

    if (sess->nack_retransmitted >= NGX_RTC_NACK_BUDGET) {
        return 0;
    }

    sess->nack_retransmitted++;

    return 1;
}

int
ngx_rtc_session_nack_window_step(ngx_rtc_session_t *sess, ngx_msec_t now)
{
    if (NULL == sess) {
        return 0;
    }

    /* Lazy-init the backoff-adjusted window on the first NACK. */
    if (0 == sess->nack_window_ms) {
        sess->nack_window_ms =
            (ngx_msec_t) ngx_rtc_core_tunables()->nack_window_ms;
    }

    if (now - sess->nack_window_start < sess->nack_window_ms) {
        return 0;
    }

    /* Sender-side backoff: a saturated window doubles the next one so a NACK
     * storm retransmits the same budget over a longer span; a quiet window
     * resets to the base cadence. */
    if (sess->nack_retransmitted >= NGX_RTC_NACK_BUDGET) {
        sess->nack_window_ms =
            ngx_min(sess->nack_window_ms * 2,
                    (ngx_msec_t) ngx_rtc_core_tunables()->nack_window_max_ms);
    } else if (0 == sess->nack_retransmitted) {
        sess->nack_window_ms =
            (ngx_msec_t) ngx_rtc_core_tunables()->nack_window_ms;
    }

    sess->nack_window_start = now;
    sess->nack_retransmitted = 0;

    return 1;
}

int32_t
ngx_rtc_session_retransmit_send(ngx_rtc_session_t *sess, uint16_t seq,
                                const uint8_t *data, uint32_t len)
{
    ngx_uint_t i;

    if (NULL == sess || NULL == data || 0 == len) {
        return NGX_RTC_ERR_INVALID;
    }

    /* Per-window dedup: a seq already retransmitted this window is skipped. */
    for (i = 0; i < sess->nack_seen_count; i++) {
        if (sess->nack_seen[i] == seq) {
            return NGX_RTC_ERR_PARSE;
        }
    }

    if (0 == ngx_rtc_session_send_rtp(sess, data, len, 0)) {
        return NGX_RTC_ERR_PARSE; /* socket not draining; leave the budget */
    }

    /* Record the seq for dedup. Bounded by NGX_RTC_NACK_BUDGET: the only caller
     * (the NACK handler) caps retransmits per window at the same budget, so one
     * entry per successful retransmit keeps count <= budget. Beyond the budget
     * no further retransmit can occur in the window, so a full set is inert. */
    if (sess->nack_seen_count < NGX_RTC_NACK_BUDGET) {
        sess->nack_seen[sess->nack_seen_count++] = seq;
    }

    return NGX_RTC_OK;
}

int32_t
ngx_rtc_session_retransmit(ngx_rtc_session_t *sess, uint16_t seq)
{
    const uint8_t *out;
    uint16_t       out_len;
    int32_t        rc;

    if (NULL == sess || NULL == sess->source) {
        return NGX_RTC_ERR_INVALID;
    }

    if (0 == ngx_rtc_source_gop_ready(sess->source)) {
        return NGX_RTC_ERR_PARSE; /* no local cache; caller uses the shm ring */
    }

    rc = ngx_rtc_rtp_ring_get(&sess->source->gop, seq, &out, &out_len);
    if (rc != NGX_RTC_OK) {
        return rc; /* outside the retained window */
    }

    return ngx_rtc_session_retransmit_send(sess, seq, out, out_len);
}


/* ============================================================================
 * Send pacing (token bucket) + TWCC loss-driven bitrate adaptation (GCC-lite).
 *
 * Pure C and host-testable: time is passed explicitly as now_ms, so this does
 * not depend on nginx's event loop or ngx_current_msec. Tokens are counted in
 * bytes with integer math; one burst window of bytes at the current target rate
 * is the bucket capacity.
 * ============================================================================ */

/* One burst window worth of bytes at the current target rate (bps -> B/ms). */
static uint64_t
ngx_rtc_pacer_bucket_bytes(const ngx_rtc_session_t *sess)
{
    uint64_t bytes;

    bytes = sess->pacer_target_bps * NGX_RTC_PACER_BURST_MS / 8000u;

    /* A bucket smaller than one packet can never admit that packet: the refill
     * is capped at the bucket size, so once tokens saturate below the packet
     * length every later admit is refused too. 100 ms of the 64 kbps floor is
     * 800 B while an H264 RTP packet reaches NGX_RTC_MAX_RTP_PKT (1214 B), so
     * without this floor a session the controller had driven down would stop
     * sending video permanently, even after the path cleared. */
    if (bytes < (uint64_t) NGX_RTC_MAX_RTP_PKT) {
        bytes = (uint64_t) NGX_RTC_MAX_RTP_PKT;
    }

    return bytes;
}

void
ngx_rtc_session_pacer_init(ngx_rtc_session_t *sess, uint64_t start_bps)
{
    if (NULL == sess) {
        return;
    }

    if (start_bps < NGX_RTC_PACER_MIN_BPS) {
        start_bps = NGX_RTC_PACER_MIN_BPS;
    } else if (start_bps > NGX_RTC_PACER_MAX_BPS) {
        start_bps = NGX_RTC_PACER_MAX_BPS;
    }

    sess->pacer_target_bps = start_bps;
    sess->pacer_tokens = ngx_rtc_pacer_bucket_bytes(sess);
    sess->pacer_last_ms = 0;   /* first admit anchors the clock */
    sess->pacer_started = 0;   /* distinguish "never anchored" from now_ms == 0 */
}

int32_t
ngx_rtc_session_pacer_admit(ngx_rtc_session_t *sess, uint32_t pkt_bytes,
                            uint64_t now_ms)
{
    uint64_t capacity;
    uint64_t delta_ms;
    uint64_t refill;

    if (NULL == sess) {
        return NGX_RTC_ERR_INVALID;
    }

    capacity = ngx_rtc_pacer_bucket_bytes(sess);

    if (0 == sess->pacer_started) {
        /* First packet: anchor the clock; the bucket starts full from init. */
        sess->pacer_started = 1;
        sess->pacer_last_ms = now_ms;
    } else if (now_ms >= sess->pacer_last_ms) {
        delta_ms = now_ms - sess->pacer_last_ms;
        if (delta_ms >= NGX_RTC_PACER_BURST_MS) {
            /* A full burst window or more has elapsed: fill to capacity without
             * the delta_ms * rate multiply (guards against uint64 overflow). */
            refill = capacity;
        } else {
            refill = delta_ms * sess->pacer_target_bps / 8000u;
        }
        sess->pacer_last_ms = now_ms;
        sess->pacer_tokens += refill;
        if (sess->pacer_tokens > capacity) {
            sess->pacer_tokens = capacity;
        }
    } else {
        /* Monotonic clock ran backwards: never refill, just re-anchor. */
        sess->pacer_last_ms = now_ms;
    }

    if (sess->pacer_tokens < (uint64_t)pkt_bytes) {
        return NGX_RTC_AGAIN;
    }

    sess->pacer_tokens -= (uint64_t)pkt_bytes;
    return NGX_RTC_OK;
}

void
ngx_rtc_session_pacer_cap(ngx_rtc_session_t *sess, uint64_t bps)
{
    uint64_t capacity;

    if (NULL == sess) {
        return;
    }

    if (bps < NGX_RTC_PACER_MIN_BPS) {
        bps = NGX_RTC_PACER_MIN_BPS;
    } else if (bps > NGX_RTC_PACER_MAX_BPS) {
        bps = NGX_RTC_PACER_MAX_BPS;
    }

    if (bps >= sess->pacer_target_bps) {
        return; /* an upper bound above the current target changes nothing */
    }

    sess->pacer_target_bps = bps;

    /* A cap lowers the burst the link may absorb with it; keeping a surplus
     * earned at the higher rate would let the next packets ignore the cap. */
    capacity = ngx_rtc_pacer_bucket_bytes(sess);
    if (sess->pacer_tokens > capacity) {
        sess->pacer_tokens = capacity;
    }
}

/* The session owns no ngx_log_t; its UDP connection does. A session without a
 * connection never reaches the control loop, so there is nothing to log. */
static ngx_log_t *
ngx_rtc_session_log(const ngx_rtc_session_t *sess)
{
    if (NULL == sess->conn) {
        return NULL;
    }

    return ((ngx_connection_t *) sess->conn)->log;
}

void
ngx_rtc_session_on_twcc(ngx_rtc_session_t *sess, uint32_t lost,
                        uint32_t received, uint64_t now_ms)
{
    uint64_t total;
    uint64_t loss_permille;
    uint64_t target;
    uint64_t delta_ms;
    uint64_t capacity;

    if (NULL == sess) {
        return;
    }

    /* Keep the cumulative counters (existing observability) plus the window. */
    sess->twcc_lost += lost;
    sess->twcc_received += received;
    sess->twcc_win_lost += lost;
    sess->twcc_win_received += received;

    total = (uint64_t)sess->twcc_win_lost + (uint64_t)sess->twcc_win_received;
    if (0 == total) {
        return;
    }

    if (0 == sess->twcc_win_started) {
        /* First feedback opens the window; wait for a second sample so the
         * elapsed-time anchor is meaningful. */
        sess->twcc_win_started = 1;
        sess->twcc_win_start_ms = now_ms;
        return;
    }

    delta_ms = (now_ms >= sess->twcc_win_start_ms)
             ? (now_ms - sess->twcc_win_start_ms) : 0u;

    /* Re-evaluate once the window has enough packets OR enough elapsed time,
     * whichever comes first; sparse flows still adapt via the time bound. */
    if (delta_ms < NGX_RTC_TWCC_WINDOW_MS && total < NGX_RTC_TWCC_MIN_PKTS) {
        return;
    }

    target = sess->pacer_target_bps;
    if (0 == target) {
        /* Pacer never initialised: nothing sensible to adapt, drop the window. */
        sess->twcc_win_lost = 0;
        sess->twcc_win_received = 0;
        sess->twcc_win_start_ms = now_ms;
        return;
    }

    /* Loss in per-mille (integer math, no float on the data path). */
    loss_permille = (uint64_t)sess->twcc_win_lost * 1000u / total;

    if (loss_permille > NGX_RTC_TWCC_LOSS_HIGH_PM) {
        /* AIMD multiplicative decrease: x0.85 on >5% loss. */
        target = target * NGX_RTC_TWCC_DECREASE_NUM / NGX_RTC_TWCC_DECREASE_DEN;
    } else if (loss_permille < NGX_RTC_TWCC_LOSS_LOW_PM) {
        /* AIMD additive increase: +8% of the current rate on <2% loss. A fixed
         * byte step would starve 64 kbps and crawl at 8 Mbps, so scale by 8%. */
        target = target + target * NGX_RTC_TWCC_INCREASE_NUM
                        / NGX_RTC_TWCC_INCREASE_DEN;
    }

    if (target < NGX_RTC_PACER_MIN_BPS) {
        target = NGX_RTC_PACER_MIN_BPS;
    } else if (target > NGX_RTC_PACER_MAX_BPS) {
        target = NGX_RTC_PACER_MAX_BPS;
    }

    /* Control-loop trace. Kept at debug level: it fires once per TWCC window
     * (twice a second per session) and is only wanted while diagnosing the
     * rate controller. Enable with `error_log ... debug;`. */
    ngx_log_error(NGX_LOG_DEBUG, ngx_rtc_session_log(sess), 0,
                  "ngx_rtc: pacer twcc lost=%uD recv=%uD total=%uL loss=%uLpm "
                  "target=%uL->%uL drop_pacer=%ui srtp_failed=%ui "
                  "send_failed=%ui send_eagain=%ui twcc_seq=%ui",
                  sess->twcc_win_lost, sess->twcc_win_received, total,
                  loss_permille, sess->pacer_target_bps, target,
                  sess->drop_pacer, sess->srtp_failed, sess->send_failed,
                  sess->send_eagain, (ngx_uint_t) sess->twcc_seq);

    sess->pacer_target_bps = target;

    /* A lower target shrinks the burst bucket; never keep a surplus above it. */
    capacity = ngx_rtc_pacer_bucket_bytes(sess);
    if (sess->pacer_tokens > capacity) {
        sess->pacer_tokens = capacity;
    }

    sess->twcc_win_lost = 0;
    sess->twcc_win_received = 0;
    sess->twcc_win_start_ms = now_ms;
}
