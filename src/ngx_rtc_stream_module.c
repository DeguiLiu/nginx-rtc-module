/*
 * ngx_rtc_stream_module.c - nginx stream (UDP) module: STUN + DTLS + SRTP.
 *
 * Handles the media plane of WebRTC playback over UDP 8000:
 *   - STUN BindingRequest -> BindingResponse (matches session by ICE ufrag)
 *   - DTLS handshake -> SRTP key export -> subscribe session to its source
 * The RTP broadcast itself is driven by the bridge module; each session's SRTP
 * context protects the plaintext RTP before it is sent.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_stream.h>

#include <time.h>
#include <arpa/inet.h>

#include "ngx_rtc_stun.h"
#include "ngx_rtc_dtls.h"
#include "ngx_rtc_srtp.h"
#include "ngx_rtc_core.h"
#include "ngx_rtc_shm.h"
#include "ngx_rtc_rtcp.h"

/* Broadcast one plaintext RTP packet to every subscribed player (defined in
 * the RTMP bridge; the WHIP publisher reuse it from this module). */
extern void ngx_rtc_broadcast_rtp(ngx_rtc_source_t *src, const uint8_t *rtp,
                                  uint32_t len, uint8_t is_video,
                                  uint8_t is_gop_start);

typedef struct {
    ngx_flag_t  rtc;
    ngx_msec_t  handshake_timeout;
    ngx_msec_t  ready_timeout;
    ngx_msec_t  reap_interval;
} ngx_rtc_stream_srv_conf_t;

/* One server{} in this MVP; the reaper timer needs these values without a
 * session handle. */
static ngx_rtc_stream_srv_conf_t *ngx_rtc_stream_srv_conf;


/* Context carried through an RTCP compound walk (one received datagram). */
typedef struct {
    ngx_rtc_session_t *sess;
    ngx_uint_t         bye;   /* an RTCP BYE was seen: close after the walk */
} ngx_rtc_stream_rtcp_ctx_t;

/*
 * Deferred close of the underlying nginx stream (UDP) session. A session close
 * must not call ngx_stream_finalize_session() synchronously from the receive
 * path: ngx_event_recvmsg() touches c->udp after the stream content handler
 * returns, so destroying the connection there is a use-after-free. Instead the
 * finalize is posted to the event queue, which runs after the current I/O batch
 * is fully unwound.
 */
typedef struct {
    ngx_event_t           ev;
    ngx_stream_session_t *s;
} ngx_rtc_stream_close_ev_t;


/*
 * Idle reaper thresholds: a session stuck before SRTP_READY (handshake not
 * finished) is reaped after NGX_RTC_SESSION_HANDSHAKE_TIMEOUT; a ready
 * session that stopped sending uplink packets (player vanished) is reaped
 * after the shorter ready timeout. Well-behaved WebRTC clients keep the
 * session alive with periodic RTCP RR, so the ready timeout never fires for
 * a live receiver.
 */
#define NGX_RTC_SESSION_REAP_INTERVAL_MS   2000


static void       ngx_rtc_stream_handler(ngx_stream_session_t *s);
static void       ngx_rtc_stream_on_stun(ngx_stream_session_t *s,
                     u_char *data, size_t len);
static ngx_rtc_session_t *ngx_rtc_stream_attach_from_shm(ngx_connection_t *c,
                     const char *ufrag);
static void       ngx_rtc_stream_on_dtls(ngx_stream_session_t *s,
                     ngx_rtc_session_t *sess, u_char *data, size_t len);
static void       ngx_rtc_stream_on_rtcp(ngx_stream_session_t *s,
                     u_char *data, size_t len);
static void       ngx_rtc_stream_on_srtp(ngx_stream_session_t *s,
                     ngx_rtc_session_t *sess, u_char *data, size_t len);
static int32_t    ngx_rtc_stream_rtcp_cb(const ngx_rtc_rtcp_pkt_t *pkt,
                     void *opaque);
static void       ngx_rtc_stream_dtls_send(void *user,
                     const uint8_t *data, size_t len);
static void       ngx_rtc_stream_dtls_done(void *user);
static void       ngx_rtc_stream_dtls_schedule(ngx_rtc_session_t *sess);
static void       ngx_rtc_stream_dtls_cancel(ngx_rtc_session_t *sess);
static void       ngx_rtc_stream_dtls_timer(ngx_event_t *ev);
static void       ngx_rtc_stream_session_close(ngx_rtc_session_t *sess,
                     ngx_rtc_session_event_t ev);
static void       ngx_rtc_stream_close_ev_handler(ngx_event_t *ev);
static void       ngx_rtc_stream_reap_timer(ngx_event_t *ev);
static void       ngx_rtc_stream_drain_ring(void);
static void       ngx_rtc_stream_notify_handler(ngx_event_t *ev);
static ngx_int_t  ngx_rtc_stream_shm_gop_send(void *opaque,
                     const uint8_t *rtp, uint32_t len, uint8_t is_gop_start);
static int32_t    ngx_rtc_stream_retransmit(ngx_rtc_session_t *sess,
                     uint16_t seq);
static void       ngx_rtc_stream_replay_gop(ngx_rtc_session_t *sess);
static char      *ngx_rtc_stream_rtc(ngx_conf_t *cf, ngx_command_t *cmd,
                     void *conf);
static void      *ngx_rtc_stream_create_srv_conf(ngx_conf_t *cf);
static char      *ngx_rtc_stream_merge_srv_conf(ngx_conf_t *cf, void *prev,
                     void *conf);
static ngx_int_t  ngx_rtc_stream_init_module(ngx_cycle_t *cycle);
static ngx_int_t  ngx_rtc_stream_init_process(ngx_cycle_t *cycle);

/* Worker-wide idle-session reaper timer. */
static ngx_event_t  ngx_rtc_stream_reap_timer_ev;
/* Cross-worker wakeup eventfd (this worker's own read side). */
static ngx_event_t       ngx_rtc_stream_notify_event;
static ngx_event_t       ngx_rtc_stream_notify_write_event;
static ngx_connection_t  ngx_rtc_stream_notify_conn;


static ngx_command_t ngx_rtc_stream_commands[] = {

    { ngx_string("rtc"),
      NGX_STREAM_SRV_CONF | NGX_CONF_NOARGS,
      ngx_rtc_stream_rtc,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("rtc_handshake_timeout"),
      NGX_STREAM_SRV_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_rtc_stream_srv_conf_t, handshake_timeout),
      NULL },

    { ngx_string("rtc_ready_timeout"),
      NGX_STREAM_SRV_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_rtc_stream_srv_conf_t, ready_timeout),
      NULL },

    /* Idle/half-open session reaper cadence. */
    { ngx_string("rtc_reap_interval"),
      NGX_STREAM_SRV_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_rtc_stream_srv_conf_t, reap_interval),
      NULL },

      ngx_null_command
};


static ngx_stream_module_t ngx_rtc_stream_module_ctx = {
    NULL,                                /* preconfiguration */
    NULL,                                /* postconfiguration */
    NULL,                                /* create main configuration */
    NULL,                                /* init main configuration */
    ngx_rtc_stream_create_srv_conf,      /* create server configuration */
    ngx_rtc_stream_merge_srv_conf        /* merge server configuration */
};


ngx_module_t ngx_rtc_stream_module = {
    NGX_MODULE_V1,
    &ngx_rtc_stream_module_ctx,          /* module context */
    ngx_rtc_stream_commands,             /* module directives */
    NGX_STREAM_MODULE,                   /* module type */
    NULL,                                /* init master */
    ngx_rtc_stream_init_module,          /* init module */
    ngx_rtc_stream_init_process,         /* init process */
    NULL,                                /* init thread */
    NULL,                                /* exit thread */
    NULL,                                /* exit process */
    NULL,                                /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_rtc_stream_create_srv_conf(ngx_conf_t *cf)
{
    ngx_rtc_stream_srv_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_rtc_stream_srv_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->rtc = NGX_CONF_UNSET;
    conf->handshake_timeout = NGX_CONF_UNSET_MSEC;
    conf->ready_timeout = NGX_CONF_UNSET_MSEC;
    conf->reap_interval = NGX_CONF_UNSET_MSEC;
    return conf;
}


static char *
ngx_rtc_stream_rtc(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_rtc_stream_srv_conf_t   *rscf = conf;
    ngx_stream_core_srv_conf_t  *cscf;

    rscf->rtc = 1;

    cscf = ngx_stream_conf_get_module_srv_conf(cf, ngx_stream_core_module);
    cscf->handler = ngx_rtc_stream_handler;

    return NGX_CONF_OK;
}


static char *
ngx_rtc_stream_merge_srv_conf(ngx_conf_t *cf, void *prev, void *conf)
{
    ngx_rtc_stream_srv_conf_t *pscf = prev;
    ngx_rtc_stream_srv_conf_t *scf = conf;

    ngx_conf_merge_msec_value(scf->handshake_timeout,
                              pscf->handshake_timeout, 10000);
    ngx_conf_merge_msec_value(scf->ready_timeout,
                              pscf->ready_timeout, 30000);
    ngx_conf_merge_msec_value(scf->reap_interval,
                              pscf->reap_interval, NGX_RTC_SESSION_REAP_INTERVAL_MS);
    ngx_conf_merge_value(scf->rtc, pscf->rtc, 0);

    ngx_rtc_stream_srv_conf = scf;

    return NGX_CONF_OK;
}


/* Reaper cadence, falling back to the compile-time default when the directive
 * was never set (no rtc server, or a prev cycle that left NGX_CONF_UNSET).
 * Mirrors ngx_rtc_rtcp_sr_interval() in the bridge. */
static ngx_msec_t
ngx_rtc_stream_reap_interval(void)
{
    ngx_msec_t  v;

    if (NULL == ngx_rtc_stream_srv_conf) {
        return NGX_RTC_SESSION_REAP_INTERVAL_MS;
    }

    v = ngx_rtc_stream_srv_conf->reap_interval;

    return (NGX_CONF_UNSET_MSEC == v || 0 == v)
           ? NGX_RTC_SESSION_REAP_INTERVAL_MS : v;
}


/* Pre-fork: create the DTLS cert/SSL_CTX and SRTP state ONCE in the master so
 * every worker inherits the SAME self-signed certificate. The http worker
 * renders its fingerprint into the SDP answer; a per-worker random cert makes
 * the udp worker's handshake cert mismatch that fingerprint (the intermittent
 * cross-worker DTLS failures). */
static ngx_int_t
ngx_rtc_stream_init_module(ngx_cycle_t *cycle)
{
    if (ngx_rtc_dtls_global_init() != 0) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "ngx_rtc_stream: DTLS init failed");
        return NGX_ERROR;
    }

    if (ngx_rtc_srtp_global_init() != 0) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "ngx_rtc_stream: SRTP init failed");
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_rtc_stream_init_process(ngx_cycle_t *cycle)
{
    /* nginx runs init_process in cache manager/loader helpers too, and those
     * inherit ngx_worker == 0 from the master. Registering the eventfd there
     * would race the real worker for the same fd and steal media wakeups. */
    if (ngx_process != NGX_PROCESS_WORKER) {
        return NGX_OK;
    }

    /* Arm the idle-session reaper (leak fix for sessions that never close). */
    ngx_memzero(&ngx_rtc_stream_reap_timer_ev, sizeof(ngx_rtc_stream_reap_timer_ev));
    ngx_rtc_stream_reap_timer_ev.handler = ngx_rtc_stream_reap_timer;
    ngx_rtc_stream_reap_timer_ev.log = cycle->log;
    ngx_rtc_stream_reap_timer_ev.data = NULL;
    ngx_add_timer(&ngx_rtc_stream_reap_timer_ev,
                  ngx_rtc_stream_reap_interval());

    /* Register this worker's eventfd read side. The bridge writes one byte after
     * enqueueing, so the ring is drained immediately (event-driven) instead of
     * by a polling timer. */
    {
        ngx_rtc_core_conf_t *ccf = ngx_rtc_core_get_conf(cycle);
        ngx_fd_t             fd;

        if (NULL != ccf && NULL != ccf->sh) {
            fd = ccf->sh->notify_fd[ngx_worker];
            if (fd != -1) {
                ngx_memzero(&ngx_rtc_stream_notify_event,
                            sizeof(ngx_rtc_stream_notify_event));
                ngx_memzero(&ngx_rtc_stream_notify_write_event,
                            sizeof(ngx_rtc_stream_notify_write_event));
                ngx_memzero(&ngx_rtc_stream_notify_conn,
                            sizeof(ngx_rtc_stream_notify_conn));

                ngx_rtc_stream_notify_event.data =
                        &ngx_rtc_stream_notify_conn;
                ngx_rtc_stream_notify_event.handler =
                        ngx_rtc_stream_notify_handler;
                ngx_rtc_stream_notify_event.log = cycle->log;
                ngx_rtc_stream_notify_event.index = NGX_INVALID_INDEX;

                /* epoll_add_event(NGX_READ_EVENT) reads c->write (nginx's
                 * symmetric read/write active-flag design); both sides must be
                 * valid, not just c->read, or e->active dereferences NULL. */
                ngx_rtc_stream_notify_write_event.log = cycle->log;
                ngx_rtc_stream_notify_write_event.index = NGX_INVALID_INDEX;

                ngx_rtc_stream_notify_conn.fd = fd;
                ngx_rtc_stream_notify_conn.read = &ngx_rtc_stream_notify_event;
                ngx_rtc_stream_notify_conn.write =
                        &ngx_rtc_stream_notify_write_event;
                ngx_rtc_stream_notify_conn.log = cycle->log;

                if (ngx_add_event(&ngx_rtc_stream_notify_event,
                                  NGX_READ_EVENT, 0) == NGX_ERROR) {
                    ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                                  "ngx_rtc_stream: cannot register eventfd");
                    return NGX_ERROR;
                }
            }
        }
    }

    return NGX_OK;
}


static void
ngx_rtc_stream_handler(ngx_stream_session_t *s)
{
    ngx_connection_t    *c;
    ngx_rtc_session_t   *sess;
    u_char               buf[1500];
    u_char              *data;
    size_t               len;
    ssize_t              n;

    c = s->connection;
    sess = ngx_stream_get_module_ctx(s, ngx_rtc_stream_module);

    /* First datagram arrives in c->buffer; later ones via c->recv(). */
    if (c->buffer != NULL && c->buffer->pos < c->buffer->last) {
        data = c->buffer->pos;
        len = c->buffer->last - c->buffer->pos;
        c->buffer->pos = c->buffer->last;
    } else {
        n = c->recv(c, buf, sizeof(buf));
        if (n == NGX_AGAIN || n == NGX_ERROR) {
            return;
        }
        data = buf;
        len = (size_t)n;
    }

    if (len < 1) {
        return;
    }

    /* STUN: magic cookie at offset 4. DTLS: content type 20..63 as first byte. */
    if (len >= 20 && data[4] == 0x21 && data[5] == 0x12
            && data[6] == 0xA4 && data[7] == 0x42) {
        ngx_rtc_stream_on_stun(s, data, len);
    } else if (data[0] >= 20 && data[0] <= 63) {
        ngx_rtc_stream_on_dtls(s, sess, data, len);
    } else if (len >= 2 && (data[0] & 0xc0) == 0x80
               && data[1] >= 200 && data[1] <= 207) {
        /* SRTCP: an RTCP payload type 200..207 (SR/RR/SDES/BYE/RTPFB/PSFB). */
        ngx_rtc_stream_on_rtcp(s, data, len);
    } else {
        /* SRTP media from the client (WHIP publisher). */
        ngx_rtc_stream_on_srtp(s, sess, data, len);
    }
}


/*
 * Rebuild a per-process session from its shm skeleton when the signaling request
 * landed on a different worker. Identity (ufrag/pwd/PT) and the source SSRC/PT
 * come from shm; DTLS/SRTP/connection state is created here exactly as it is in
 * the same-worker path. Returns NULL when there is no skeleton to attach.
 */
static ngx_rtc_session_t *
ngx_rtc_stream_attach_from_shm(ngx_connection_t *c, const char *ufrag)
{
    ngx_rtc_core_conf_t          *ccf;
    ngx_rtc_shm_session_snapshot_t snap;
    ngx_rtc_session_t            *sess;
    ngx_rtc_source_t             *src;
    ngx_int_t                     rc;

    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
    if (NULL == ccf || NULL == ccf->sh) {
        return NULL;
    }

    /* Allocate the process session before binding so a failed allocation
     * cannot leave the skeleton bound to this worker with no session. */
    sess = ngx_calloc(sizeof(ngx_rtc_session_t), c->log);
    if (NULL == sess) {
        return NULL;
    }

    /* Bind before reading: the snapshot is copied under the pool mutex, so the
     * skeleton cannot be freed by a concurrent half-open reap between the
     * lookup and the per-process session rebuild. */
    rc = ngx_rtc_shm_session_bind(ccf->sh, (u_char *) ufrag, ngx_strlen(ufrag),
                                  (ngx_uint_t) ngx_worker, &snap);
    if (NGX_OK != rc) {
        ngx_free(sess);
        return NULL;
    }

    ngx_queue_init(&sess->queue);
    ngx_queue_init(&sess->sub_queue);
    ngx_rtc_session_fsm_init(&sess->fsm, sess->fsm_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, sess);
    ngx_rtc_session_fsm_set_reporter(&sess->fsm,
                                     ngx_rtc_session_fsm_report_unhandled);

    /* The owning worker already finished DTLS/SRTP; this one never will, because
     * handshake packets only reach the worker that owns the UDP session. Adopt
     * the state from the shm skeleton, or is_ready() stays false here: the send
     * gate in ngx_rtc_core.c drops every packet, RTCP is ignored in
     * ngx_rtc_stream_on_rtcp, and the reaper uses handshake_timeout instead of
     * ready_timeout, tearing down a session that is in fact live. */
    if (0 != snap.srtp_ready) {
        (void) ngx_rtc_session_fsm_restore(&sess->fsm,
                                           NGX_RTC_SESSION_STATE_SRTP_READY);
    }

    sess->last_active = ngx_current_msec;

    sess->id = snap.id;
    ngx_memcpy(sess->ice_ufrag, snap.ice_ufrag, sizeof(sess->ice_ufrag));
    ngx_memcpy(sess->ice_pwd, snap.ice_pwd, sizeof(sess->ice_pwd));
    sess->video_pt = snap.video_pt;
    sess->audio_pt = snap.audio_pt;
    sess->twcc_video_ext = snap.twcc_video_ext;
    sess->twcc_audio_ext = snap.twcc_audio_ext;
    sess->publishing = snap.publishing;

    /* Rebuild the per-process source from the shm source identity so the media
     * plane (replay / NACK / sender report) can reach it in this worker. */
    if (0 != snap.source_name[0]) {
        src = ngx_rtc_source_get((const char *) snap.source_name);
        if (NULL != src) {
            src->video_ssrc = snap.source_video_ssrc;
            src->audio_ssrc = snap.source_audio_ssrc;
            src->video_pt = snap.source_video_pt;
            src->audio_pt = snap.source_audio_pt;
            sess->source = src;
            if (0 != snap.publishing) {
                /* WHIP publisher: cache the shm source pointer (already created
                 * and marked publishing by the signaling worker) so the media
                 * plane mirrors video without a per-packet rbtree lookup. */
                src->shm_src = ngx_rtc_shm_source_get(
                        ccf->sh, snap.source_name,
                        ngx_strlen(snap.source_name));
                src->shm_sync_ms = ngx_current_msec;

                /* Tag process-local ownership too: this media worker is the one
                 * that closes the session, so its release must see the WHIP
                 * kind or the shm source stays publishing (leak). Adopt rather
                 * than claim -- the signaling worker already holds the right,
                 * and claiming here would make this process a second claimant.
                 * This also mirrors publishing=1, which the shm snapshot
                 * asserts: leaving it 0 let the local source be reaped while
                 * the shm claim was still live. */
                ngx_rtc_publish_mirror(src, NGX_RTC_PUBLISHER_WHIP);

                /* New (or republished) stream: clear the uplink reorder buffer
                 * on this media worker too. The HTTP worker only resets its own
                 * per-process copy, which never carries SRTP media. */
                ngx_rtc_jitter_reset(&src->jitter);
                src->jitter.gap_timeout_ms =
                    ngx_rtc_core_tunables()->jitter_timeout_ms;
            }
        }
    }

    ngx_rtc_session_add(sess);

    ngx_log_error(NGX_LOG_INFO, c->log, 0,
                  "ngx_rtc_stream: cross-worker attach ufrag=%s", ufrag);

    return sess;
}


/*
 * Publish this session's lifecycle state into its shm skeleton, where the stats
 * renderer picks it up for Lua. Called only from the UDP handlers below, which
 * run solely on the worker that owns the session: the signaling worker holds a
 * second, process-local copy of the same session whose machine stays in NEW for
 * its whole life, and letting that one write would mark every live session NEW.
 *
 * There is deliberately no call on any close path. A closed skeleton is freed
 * outright rather than kept around as CLOSED, so there is nothing to publish --
 * and the close paths are exactly the ones reachable from the signaling worker,
 * where the local state is stale.
 */
static void
ngx_rtc_stream_publish_state(ngx_rtc_session_t *sess)
{
    ngx_rtc_core_conf_t *ccf;

    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
    if (NULL != ccf && NULL != ccf->sh) {
        ngx_rtc_shm_session_set_state(ccf->sh,
                (u_char *) sess->ice_ufrag, ngx_strlen(sess->ice_ufrag),
                (ngx_uint_t) ngx_worker,
                (uint8_t) ngx_rtc_session_fsm_get_state(&sess->fsm));
    }
}


static void
ngx_rtc_stream_on_stun(ngx_stream_session_t *s, u_char *data, size_t len)
{
    ngx_connection_t     *c;
    ngx_rtc_session_t    *sess;
    ngx_rtc_stun_t        stun;
    struct sockaddr_in   *sin;
    uint32_t              mapped_addr;
    uint16_t              mapped_port;
    uint8_t               resp[256];
    int                   n;

    c = s->connection;

    if (ngx_rtc_stun_decode(&stun, data, len) != 0 || 0 == stun.has_username) {
        return;
    }

    /* werift sends the STUN USERNAME as "server_ufrag:client_ufrag", so the
     * server ufrag (which we generated in the SDP answer) is the first half. */
    sess = ngx_rtc_session_find(stun.local_ufrag);
    if (NULL == sess) {
        /* The signaling request landed on a different worker: rebuild the
         * per-process session from its shm skeleton. */
        sess = ngx_rtc_stream_attach_from_shm(c, stun.local_ufrag);
        if (NULL == sess) {
            ngx_log_error(NGX_LOG_WARN, c->log, 0,
                          "ngx_rtc_stream: unknown ICE ufrag remote=\"%s\" local=\"%s\"",
                          stun.remote_ufrag, stun.local_ufrag);
            return;
        }
    }

    {
        int rc = ngx_rtc_stun_verify_request(&stun, data, len, sess->ice_pwd);
        if (0 != rc) {
            ngx_log_error(NGX_LOG_WARN, c->log, 0,
                          "ngx_rtc_stream: STUN integrity check failed rc=%d ufrag=\"%s\"",
                          rc, sess->ice_ufrag);
            return;
        }
    }

    /* Bind the session to this UDP connection + peer address. */
    sess->conn = c;
    sess->peer_addr = c->sockaddr;
    sess->peer_len = c->socklen;
    sess->last_active = ngx_current_msec;

    {
        ngx_rtc_session_fsm_ctx_t fctx;

        fctx.conn = c;
        fctx.data = NULL;
        fctx.len = 0;

        /* NEW -> ICE_BOUND, and a no-op for the binding retransmits and ICE
         * consent checks that keep arriving for the life of the session. The
         * transition guard checks the connection carried in fctx. */
        (void)ngx_rtc_session_fsm_dispatch_ctx(&sess->fsm,
                                               NGX_RTC_SESSION_EVT_STUN_BINDING,
                                               &fctx);
    }

    /* ICE_BOUND when this was the transition, unchanged (and re-published) for
     * the binding retransmits and consent checks that follow. */
    ngx_rtc_stream_publish_state(sess);

    ngx_stream_set_ctx(s, sess, ngx_rtc_stream_module);

    sin = (struct sockaddr_in *)c->sockaddr;
    mapped_addr = ntohl(sin->sin_addr.s_addr);
    mapped_port = ntohs(sin->sin_port);

    n = ngx_rtc_stun_encode_binding_response(&stun, sess->ice_pwd,
            mapped_addr, mapped_port, resp, sizeof(resp));
    if (n > 0) {
        ssize_t sent;

        sent = c->send(c, resp, (size_t)n);
        if (sent == NGX_ERROR) {
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, ngx_socket_errno,
                       "ngx_rtc: STUN response send failed");
        }
    }
}


static void
ngx_rtc_stream_on_dtls(ngx_stream_session_t *s, ngx_rtc_session_t *sess,
        u_char *data, size_t len)
{
    ngx_connection_t *c;

    c = s->connection;

    if (NULL == sess) {
        /* No STUN binding yet (or unknown ufrag); ignore. */
        return;
    }

    /* The first DTLS record after ICE binding creates the DTLS context and
     * moves ICE_BOUND -> DTLS_HANDSHAKE. Later records are fed through: the
     * state no longer says "pending". Only a record the transition guard
     * accepts may start the handshake, so a datagram that merely looked like
     * DTLS in the UDP dispatch never gets a context built from it. */
    if (ngx_rtc_session_fsm_dtls_pending(&sess->fsm)) {
        ngx_rtc_session_fsm_ctx_t fctx;

        fctx.conn = c;
        fctx.data = data;
        fctx.len = len;

        if (false == ngx_rtc_session_fsm_dispatch_ctx(
                &sess->fsm, NGX_RTC_SESSION_EVT_DTLS_PACKET, &fctx)) {
            return;
        }

        /* Past the guard means the transition was accepted, so the machine is
         * now in DTLS_HANDSHAKE. Published here rather than in the DTLS-done
         * path because this is the state a viewer wedged mid-handshake is stuck
         * in, and it is the reason this state is exported at all. */
        ngx_rtc_stream_publish_state(sess);

        sess->conn = c;

        if (ngx_rtc_dtls_create(&sess->dtls,
                ngx_rtc_stream_dtls_send, ngx_rtc_stream_dtls_done, sess) != 0) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "ngx_rtc_stream: DTLS create failed");
            ngx_rtc_stream_session_close(sess, NGX_RTC_SESSION_EVT_CLOSE);
            return;
        }

        ngx_rtc_stream_dtls_schedule(sess);
    }

    sess->last_active = ngx_current_msec;

    if (ngx_rtc_dtls_on_data(&sess->dtls, data, len) != 0) {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "ngx_rtc_stream: DTLS processing failed");
        ngx_rtc_stream_session_close(sess, NGX_RTC_SESSION_EVT_CLOSE);
        return;
    }

    /* The completion callback runs inside on_data(); when it cannot finish the
     * handshake it flags instead of closing, so the teardown happens here
     * rather than under this frame. */
    if (0 != sess->close_pending) {
        ngx_rtc_stream_session_close(sess, NGX_RTC_SESSION_EVT_CLOSE);
        return;
    }

    ngx_rtc_stream_dtls_schedule(sess);
}


/*
 * Handle a received SRTCP datagram: unprotect it, walk the compound packet,
 * and act on RR/NACK/PLI/BYE. The decode callback never frees the session; a
 * BYE is deferred until the walk returns (ctx.bye) so no use-after-free.
 */
static void
ngx_rtc_stream_on_rtcp(ngx_stream_session_t *s, u_char *data, size_t len)
{
    ngx_rtc_session_t         *sess;
    ngx_rtc_stream_rtcp_ctx_t  ctx;
    int                        n;

    sess = ngx_stream_get_module_ctx(s, ngx_rtc_stream_module);
    if (NULL == sess || NULL == sess->conn) {
        return;
    }
    if (0 == ngx_rtc_session_fsm_is_ready(&sess->fsm)) {
        return;
    }

    sess->last_active = ngx_current_msec;

    n = (int)len;
    if (ngx_rtc_srtp_unprotect_rtcp(&sess->srtp, data, &n) != 0) {
        return;
    }

    ctx.sess = sess;
    ctx.bye = 0;
    ngx_rtc_rtcp_decode(data, (uint32_t)n, ngx_rtc_stream_rtcp_cb, &ctx);

    if (0 != ctx.bye) {
        ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                      "ngx_rtc_stream: RTCP BYE received, closing session=%s",
                      sess->ice_ufrag);
        ngx_rtc_stream_session_close(sess, NGX_RTC_SESSION_EVT_RTCP_BYE);
    }
}


/* Resolve the shm source this publisher mirrors into, re-checking it at most
 * once per NGX_RTC_SHM_SYNC_MS. The pointer is used lock-free on every media
 * packet, and a release on another worker (the signaling worker's stale-session
 * reaper, or this worker's own close) can make the shm source reapable while
 * media still flows, so it must not be trusted for a whole publish.
 *
 * Never create the shm source here. ngx_rtc_shm_source_get() allocates on miss,
 * and this runs once per media packet: after a teardown removed the source
 * (RTMP release, or the reaper once a WHIP session expired) the next buffered
 * or in-flight packet would resurrect it and re-claim publishing=1, leaving a
 * name that no protocol can ever reclaim. The claim is only ever renewed for a
 * source this worker reached through a real attach (attach_from_shm) or a WHIP
 * publish, both of which cache shm_src up front. */
static ngx_rtc_shm_source_t *
ngx_rtc_stream_shm_source(ngx_rtc_source_t *src)
{
    ngx_rtc_core_conf_t  *ccf;
    ngx_msec_t            now;

    if (NULL == src || NULL == src->shm_src) {
        return NULL;
    }

    now = ngx_current_msec;
    if ((ngx_msec_int_t) (now - src->shm_sync_ms)
            < (ngx_msec_int_t) NGX_RTC_SHM_SYNC_MS) {
        return (ngx_rtc_shm_source_t *) src->shm_src;
    }

    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
    if (NULL == ccf || NULL == ccf->sh) {
        return NULL;
    }

    if (NGX_OK != ngx_rtc_shm_source_try_publish(ccf->sh,
                                                 (u_char *) src->name,
                                                 ngx_strlen(src->name),
                                                 NGX_RTC_PUBLISHER_WHIP)) {
        /* The source is gone (reaped) or another protocol owns the name: stop
         * mirroring and drop the stale pointer so the next packet re-checks. */
        src->shm_src = NULL;
        src->shm_sync_ms = now;
        return NULL;
    }

    src->shm_sync_ms = now;

    return (ngx_rtc_shm_source_t *) src->shm_src;
}


/* Mirror the per-process counters into the resolved shm source without taking
 * the slab pool mutex: the source is kept alive by the claim above and the
 * counter words are naturally aligned, so a cross-worker reader never tears
 * one. The name-keyed setter this replaces locked the pool on every media
 * packet, on the same mutex the cross-worker media paths contend for. */
static void
ngx_rtc_stream_shm_stats(ngx_rtc_source_t *src, ngx_rtc_shm_source_t *shm_src)
{
    if (NULL == shm_src) {
        return;
    }

    shm_src->video_pkts = src->video_pkts;
    shm_src->video_octets = src->video_octets;
    shm_src->audio_pkts = src->audio_pkts;
    shm_src->audio_octets = src->audio_octets;
}


/* WHIP uplink reorder emit: called by ngx_rtc_jitter_push once video RTP
 * packets are in sequence. Mirrors the pre-jitter path so counters, GOP cache
 * and shm retransmit are updated exactly once per emitted packet. */
static int32_t
ngx_rtc_stream_whip_emit(void *opaque, const uint8_t *rtp, uint32_t len,
                         uint8_t is_gop_start)
{
    ngx_rtc_session_t    *sess = opaque;
    ngx_rtc_core_conf_t  *ccf;
    ngx_rtc_shm_source_t *shm_src;
    uint32_t              octets;

    if (NULL == sess || NULL == sess->source) {
        return NGX_RTC_ERR_INVALID;
    }

    ngx_rtc_broadcast_rtp(sess->source, rtp, len, 1 /* video */, is_gop_start);

    octets = (len > NGX_RTC_RTP_HEADER_SIZE)
             ? (len - NGX_RTC_RTP_HEADER_SIZE) : len;
    sess->source->video_pkts++;
    sess->source->video_octets += octets;

    shm_src = ngx_rtc_stream_shm_source(sess->source);
    ngx_rtc_stream_shm_stats(sess->source, shm_src);

    if (NULL != shm_src) {
        ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
        if (NULL != ccf && NULL != ccf->sh) {
            ngx_rtc_shm_retransmit_append(ccf->sh, shm_src, rtp, len,
                                          is_gop_start);
        }
    }

    ngx_rtc_rtp_ring_push(&sess->source->gop, rtp, len, is_gop_start);

    return NGX_RTC_OK;
}

static void
ngx_rtc_stream_on_srtp(ngx_stream_session_t *s, ngx_rtc_session_t *sess,
                       u_char *data, size_t len)
{
    ngx_rtc_core_conf_t *ccf;
    uint32_t             rtp_len;
    int                  n;
    uint8_t              pt;
    uint8_t              is_video;
    uint8_t              is_gop_start;

    if (NULL == sess || 0 == sess->publishing || NULL == sess->source
            || 0 == ngx_rtc_session_fsm_is_ready(&sess->fsm)) {
        return; /* only a ready WHIP publisher sends SRTP media */
    }

    n = (int) len;
    if (ngx_rtc_srtp_unprotect_rtp(&sess->srtp, data, &n) != 0) {
        return;
    }
    if (n < (int) NGX_RTC_RTP_HEADER_SIZE) {
        return;
    }

    /* Normalize the publisher's RTP header. The downlink re-stamps its own
     * transport-wide-cc extension at offset 12, so CSRCs and per-hop
     * extensions (a browser adds them whenever the answer echoed its extmap)
     * must be gone before the payload offset, the STAP-A keyframe check and
     * the GOP cache all assume the 12-byte fixed-header form. */
    rtp_len = (uint32_t) n;
    if (ngx_rtc_rtp_strip_header_ext(data, &rtp_len) != NGX_RTC_OK) {
        return; /* malformed header: drop the packet */
    }

    sess->last_active = ngx_current_msec;

    pt = data[1] & 0x7Fu;
    is_video = (pt == sess->video_pt) ? 1 : 0;
    if (0 == is_video && pt != sess->audio_pt) {
        return; /* unknown payload type */
    }

    /* The publisher's RTP SSRC becomes the authoritative media SSRC, so a
     * later broadcast rewrites per-session PT against the right source. */
    {
        uint32_t ssrc;

        ssrc = ((uint32_t) data[8] << 24) | ((uint32_t) data[9] << 16)
             | ((uint32_t) data[10] << 8) | (uint32_t) data[11];

        ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
        if (NULL != ccf && NULL != ccf->sh) {
            if (0 != is_video && sess->source->video_ssrc != ssrc) {
                sess->source->video_ssrc = ssrc;
                ngx_rtc_shm_source_set_ssrc(ccf->sh,
                        (u_char *) sess->source->name,
                        ngx_strlen(sess->source->name), ssrc, 0);
            } else if (0 == is_video && sess->source->audio_ssrc != ssrc) {
                sess->source->audio_ssrc = ssrc;
                ngx_rtc_shm_source_set_ssrc(ccf->sh,
                        (u_char *) sess->source->name,
                        ngx_strlen(sess->source->name), 0, ssrc);
            }
        }
    }

    /* Video goes through the reorder buffer so a lossy WAN cannot deliver H264
     * out of sequence; the emit callback handles broadcast, counters, GOP cache
     * and shm retransmit. Audio is gapless and bypasses the buffer entirely. */
    if (0 != is_video) {
        uint16_t seq;

        /* A H264 STAP-A (SPS/PPS) opens the keyframe access unit. The NALU
         * header byte carries forbidden-bit + NRI + the 5-bit type, and NRI is
         * normally 3 for a parameter set, so the byte is 0x78 rather than 0x18:
         * comparing the whole byte against the bare type missed every real
         * STAP-A. Mask the type field instead. */
        is_gop_start = 0;
        if (rtp_len > NGX_RTC_RTP_HEADER_SIZE
                && NGX_RTC_H264_STAP_A
                       == (data[NGX_RTC_RTP_HEADER_SIZE] & 0x1Fu)) {
            is_gop_start = 1;
        }

        seq = (uint16_t)(((uint16_t) data[2] << 8) | (uint16_t) data[3]);
        (void) ngx_rtc_jitter_push(&sess->source->jitter, data, rtp_len,
                                   seq, is_gop_start,
                                   (uint32_t) ngx_current_msec,
                                   ngx_rtc_stream_whip_emit, sess);

        /* Reorder-buffer counters, visible only in a --with-debug build: a
         * sustained n_skipped/n_stale rate is what turns into visible mosaic,
         * so keep them reachable without paying for a log line in production. */
        ngx_log_debug5(NGX_LOG_DEBUG_EVENT, s->connection->log, 0,
                       "ngx_rtc_stream: whip jitter pending=%ui skipped=%ui "
                       "stale=%ui reanchor=%ui next=%ui",
                       (ngx_uint_t) sess->source->jitter.pending,
                       (ngx_uint_t) sess->source->jitter.n_skipped,
                       (ngx_uint_t) sess->source->jitter.n_stale,
                       (ngx_uint_t) sess->source->jitter.n_reanchor,
                       (ngx_uint_t) sess->source->jitter.next_seq);
        return;
    }

    /* Audio: broadcast and accumulate counters directly (never reordered). */
    ngx_rtc_broadcast_rtp(sess->source, data, rtp_len, 0, 0);

    {
        uint32_t octets;

        octets = (rtp_len > NGX_RTC_RTP_HEADER_SIZE)
                 ? (rtp_len - NGX_RTC_RTP_HEADER_SIZE) : rtp_len;
        sess->source->audio_pkts++;
        sess->source->audio_octets += octets;

        ngx_rtc_stream_shm_stats(sess->source,
                                 ngx_rtc_stream_shm_source(sess->source));
    }
}


static int32_t
ngx_rtc_stream_rtcp_cb(const ngx_rtc_rtcp_pkt_t *pkt, void *opaque)
{
    ngx_rtc_stream_rtcp_ctx_t *ctx = opaque;
    ngx_rtc_session_t         *sess;
    ngx_log_t                 *log;
    uint64_t                   target_before;
    uint16_t                   seqs[512];
    uint16_t                   n;
    uint16_t                   i;

    sess = ctx->sess;
    if (NULL == sess || NULL == sess->source) {
        return NGX_RTC_OK;
    }

    log = (NULL != sess->conn) ? ((ngx_connection_t *) sess->conn)->log
                               : ngx_cycle->log;

    /* NACK/PLI feedback only ever concerns the video track. The GOP ring caches
     * video RTP only (audio seq is an independent counter), so answering an
     * audio NACK or a mismatched-SSRC feedback with ring data would retransmit
     * the wrong stream. Filter by the media_ssrc carried in the feedback. */
    if (NGX_RTC_RTCP_RTPFB == pkt->type && NGX_RTC_RTCP_FMT_NACK == pkt->fmt) {
        if (pkt->media_ssrc == sess->source->video_ssrc) {
            n = 0;
            if (ngx_rtc_rtcp_nack_expand(pkt, seqs, 512, &n) == NGX_RTC_OK) {
                ngx_msec_t now = ngx_current_msec;

                /* Roll the backoff-adjusted response window when it elapses; a
                 * rolled window also clears the dedup set so a packet answered
                 * in a previous window may be sent again. */
                if (0 != ngx_rtc_session_nack_window_step(sess, now)) {
                    ngx_rtc_session_nack_reset(sess);
                }

                for (i = 0; i < n; i++) {
                    /* Cap attempts per window so a NACK storm cannot burst the
                     * socket; the client re-NACKs the rest. An attempt counts
                     * against the budget whether or not it is delivered: when
                     * the pacer refuses every retransmit, a success-counted
                     * budget would never advance, this loop would never break,
                     * and one NACK could offer all 512 expanded sequences to
                     * the pacer. The window backoff reads the same counter. */
                    if (0 == ngx_rtc_session_nack_budget_take(sess)) {
                        break;
                    }

                    (void)ngx_rtc_stream_retransmit(sess, seqs[i]);
                }
            }
        }
    } else if (NGX_RTC_RTCP_PSFB == pkt->type
               && NGX_RTC_RTCP_FMT_PLI == pkt->fmt) {
        if (pkt->media_ssrc == sess->source->video_ssrc) {
            /* Re-send the most recent keyframe from the shared cache: the
             * source GOP ring same-worker, the shm retransmit ring cross-worker. */
            ngx_log_error(NGX_LOG_DEBUG, log, 0,
                          "ngx_rtc: pli ufrag=\"%s\" target=%uL drop_pacer=%ui "
                          "drop_gop=%ui twcc_seq=%ui",
                          (u_char *) sess->ice_ufrag,
                          sess->pacer_target_bps, sess->drop_pacer,
                          sess->drop_gop, (ngx_uint_t) sess->twcc_seq);
            ngx_rtc_stream_replay_gop(sess);
        }
    } else if (NGX_RTC_RTCP_RTPFB == pkt->type
               && NGX_RTC_RTCP_FMT_TWCC == pkt->fmt) {
        /* Transport-wide CC feedback: feed the loss-based rate controller.
         * on_twcc accumulates both the cumulative counters (twcc_lost/received,
         * mirrored below) and the AIMD window, and adjusts pacer_target_bps.
         * The raw fields are logged because the symbols alone cannot be trusted:
         * if the feedback's range starts far from our own transport sequence,
         * every symbol in it describes a packet this sender never stamped, and
         * the loss it reports is not loss. */
        ngx_log_error(NGX_LOG_DEBUG, log, 0,
                      "ngx_rtc: twccfb ufrag=\"%s\" base=%ui count=%ui "
                      "lost=%uD recv=%uD our_seq=%ui",
                      (u_char *) sess->ice_ufrag,
                      (ngx_uint_t) pkt->twcc_base_seq,
                      (ngx_uint_t) pkt->twcc_pkt_count,
                      pkt->twcc_lost, pkt->twcc_received,
                      (ngx_uint_t) sess->twcc_seq);

        ngx_rtc_session_on_twcc(sess, pkt->twcc_lost, pkt->twcc_received,
                                (uint64_t) ngx_current_msec);

        {
            ngx_rtc_core_conf_t *ccf;

            ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
            if (NULL != ccf && NULL != ccf->sh) {
                ngx_rtc_shm_session_set_stats(ccf->sh,
                        (u_char *) sess->ice_ufrag,
                        ngx_strlen(sess->ice_ufrag),
                        sess->twcc_lost, sess->twcc_received,
                        (ngx_uint_t) sess->pacer_target_bps,
                        (ngx_uint_t) sess->drop_pacer,
                        (ngx_uint_t) sess->drop_gop);
            }
        }
    } else if (pkt->has_remb) {
        /* Receiver Estimated Maximum Bitrate: an upper bound on what the viewer
         * believes the path can carry, so it may lower the pacer target but
         * never raise it. Applying it as the absolute target discarded the
         * loss controller's state on every report, and since the estimate is
         * derived from what the viewer received -- which the pacer's own drops
         * reduce -- the two drove each other to the floor and stayed there.
         * The loss-based controller owns pacer_target_bps; REMB only caps it. */
        target_before = sess->pacer_target_bps;
        ngx_rtc_session_pacer_cap(sess, (uint64_t) pkt->remb_bitrate_bps);
        ngx_log_error(NGX_LOG_DEBUG, log, 0,
                      "ngx_rtc: remb ufrag=\"%s\" remb=%uD target=%uL->%uL "
                      "drop_pacer=%ui",
                      (u_char *) sess->ice_ufrag, pkt->remb_bitrate_bps,
                      target_before, sess->pacer_target_bps,
                      sess->drop_pacer);
    } else if (NGX_RTC_RTCP_BYE == pkt->type) {
        ctx->bye = 1;
    }

    return NGX_RTC_OK;
}


static void
ngx_rtc_stream_dtls_send(void *user, const uint8_t *data, size_t len)
{
    ngx_rtc_session_t *sess = user;
    ngx_connection_t  *c;

    c = sess->conn;
    if (NULL != c && len > 0) {
        ssize_t sent;

        sent = c->send(c, (u_char *)data, len);
        if (sent == NGX_ERROR) {
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, ngx_socket_errno,
                       "ngx_rtc: DTLS handshake send failed");
        }
    }
}


static void
ngx_rtc_stream_dtls_schedule(ngx_rtc_session_t *sess)
{
    ngx_connection_t  *c;
    ngx_event_t       *ev;
    int64_t            timeout_ms;

    timeout_ms = ngx_rtc_dtls_next_timeout_ms(&sess->dtls);
    if (timeout_ms <= 0) {
        return;
    }

    ev = (ngx_event_t *)sess->dtls_timer;
    if (NULL == ev) {
        c = (ngx_connection_t *)sess->conn;
        ev = ngx_alloc(sizeof(ngx_event_t), (NULL != c) ? c->log : NULL);
        if (NULL == ev) {
            return;
        }
        sess->dtls_timer = ev;
        ev->timer_set = 0;
    }

    if (ev->timer_set) {
        ngx_del_timer(ev);
    }

    ngx_memzero(ev, sizeof(*ev));
    ev->handler = ngx_rtc_stream_dtls_timer;
    ev->data = sess;
    ev->log = (NULL != (ngx_connection_t *)sess->conn)
              ? ((ngx_connection_t *)sess->conn)->log : NULL;

    ngx_add_timer(ev, (ngx_msec_t)timeout_ms);
}


static void
ngx_rtc_stream_dtls_cancel(ngx_rtc_session_t *sess)
{
    ngx_event_t *ev;

    ev = (ngx_event_t *)sess->dtls_timer;
    if (NULL == ev) {
        return;
    }

    if (ev->timer_set) {
        ngx_del_timer(ev);
    }
    ngx_free(ev);
    sess->dtls_timer = NULL;
}


static void
ngx_rtc_stream_dtls_timer(ngx_event_t *ev)
{
    ngx_rtc_session_t *sess;

    sess = (ngx_rtc_session_t *)ev->data;
    if (NULL == sess || ngx_rtc_dtls_is_done(&sess->dtls)) {
        return;
    }

    if (ngx_rtc_dtls_handle_timeout(&sess->dtls) != 0) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                      "ngx_rtc_stream: DTLS timeout handling failed, "
                      "closing session=%s", sess->ice_ufrag);
        ngx_rtc_stream_session_close(sess, NGX_RTC_SESSION_EVT_TIMEOUT);
        return;
    }

    /* The completion callback runs inside handle_timeout() and, when it cannot
     * finish the handshake, flags rather than closes -- closing there would
     * free `sess` before ngx_rtc_dtls_is_done() below reads it. */
    if (0 != sess->close_pending) {
        ngx_rtc_stream_session_close(sess, NGX_RTC_SESSION_EVT_CLOSE);
        return;
    }

    if (ngx_rtc_dtls_is_done(&sess->dtls)) {
        ngx_rtc_stream_dtls_cancel(sess);
        return;
    }

    ngx_rtc_stream_dtls_schedule(sess);
}


static void
ngx_rtc_stream_dtls_done(void *user)
{
    ngx_rtc_session_t *sess = user;
    uint8_t            recv_key[NGX_RTC_SRTP_KEY_LEN + NGX_RTC_SRTP_SALT_LEN];
    uint8_t            send_key[NGX_RTC_SRTP_KEY_LEN + NGX_RTC_SRTP_SALT_LEN];

    if (ngx_rtc_dtls_get_srtp_key(&sess->dtls, recv_key, send_key) != 0) {
        /* Flag, do not close: this callback runs synchronously inside
         * ngx_rtc_dtls_on_data()/ngx_rtc_dtls_handle_timeout(), and both of
         * those callers use `sess` again after the call returns. Closing here
         * freed the session under them (use-after-free, then a possible
         * double free on the second teardown path). They close it instead, as
         * soon as they regain control. */
        sess->close_pending = 1;
        return;
    }

    if (ngx_rtc_srtp_create(&sess->srtp, recv_key, send_key) != 0) {
        sess->close_pending = 1;
        return;
    }

    /* DTLS_HANDSHAKE -> SRTP_READY. The transition action in the state
     * machine is a no-op stub; key export + subscribe stay here so the
     * already-verified DTLS flow is not disturbed. */
    (void)ngx_rtc_session_fsm_dispatch(&sess->fsm,
                                       NGX_RTC_SESSION_EVT_DTLS_HANDSHAKE_DONE);

    sess->last_active = ngx_current_msec;

    /* Media-ready: arm the send pacer. Start unpaced (max) so an existing
     * stream is not throttled before TWCC/REMB feedback establishes the real
     * link rate; the token bucket drops on overrun and the viewer re-NACKs. */
    ngx_rtc_session_pacer_init(sess, NGX_RTC_PACER_MAX_BPS);

    /* Subscribe the session to its source, then replay the current GOP so a
     * late subscriber decodes its first frame without waiting for an IDR. The
     * replay also warms the session RTX ring (same-worker publish) so an
     * immediate NACK is answerable. */
    if (NULL != sess->source && 0 == sess->publishing) {
        ngx_rtc_source_subscribe(sess->source, sess);

        /* Replay the latest GOP from the shared cache so a late subscriber
         * decodes its first frame immediately: the source GOP ring same-worker,
         * the shm retransmit ring cross-worker. */
        ngx_rtc_stream_replay_gop(sess);
    }

    /* Phase 1: mark the cross-worker session skeleton ready and link it into
     * the shm subscription table. The media plane still sends through the
     * per-process session above. */
    {
        ngx_rtc_core_conf_t *ccf;

        ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
        if (NULL != ccf && NULL != ccf->sh) {
            (void) ngx_rtc_shm_session_activate(ccf->sh,
                    (u_char *)sess->ice_ufrag, ngx_strlen(sess->ice_ufrag),
                    (ngx_uint_t) ngx_worker);
        }
    }
}


/*
 * Close a session and release everything it owns. `ev` is the FSM event that
 * describes why the session closed (TIMEOUT / CLOSE / RTCP_BYE); dispatching
 * it moves the session state machine to CLOSED before the teardown runs.
 *
 * Besides the session struct itself this also ends the underlying nginx stream
 * UDP session: nginx only destroys an idle UDP connection when its pool is torn
 * down (ngx_stream_finalize_session), so without this step every bound 4-tuple
 * permanently occupies an ngx_connection_t slot until worker_connections is
 * exhausted. The finalize is posted to the event queue (not run inline) so a
 * close from the receive path never frees the connection under ngx_event_recvmsg.
 *
 * Teardown is intentionally kept in this module (not in the CLOSED entry
 * action of the HSM): the state-machine engine never touches `sess->fsm`
 * after its entry actions return, but freeing the very struct the HSM lives
 * in while the HSM is executing its own transition is fragile. Dispatching
 * first and tearing down after the transition settles is the safe ordering.
 */
static void
ngx_rtc_stream_session_close(ngx_rtc_session_t *sess,
                             ngx_rtc_session_event_t ev)
{
    ngx_stream_session_t      *s;
    ngx_connection_t          *c;
    ngx_rtc_stream_close_ev_t *close_ev;

    if (NULL == sess) {
        return;
    }

    /* Detach the stream connection context before freeing, so a later
     * datagram is ignored instead of dereferencing the freed session.
     * c->data is the ngx_stream_session_t (set by ngx_stream_handler.c).
     * sess->conn is cleared first so no send path touches the connection
     * while its teardown is in flight. */
    c = (ngx_connection_t *)sess->conn;
    sess->conn = NULL;
    if (NULL != c) {
        s = c->data;
        if (NULL != s) {
            ngx_stream_set_ctx(s, NULL, ngx_rtc_stream_module);

            /* Release the nginx stream UDP session (connection slot) too. The
             * finalize must not run on this stack (see close_ev comment), so it
             * is posted; the event is heap-allocated because it outlives both
             * the session struct and (until finalize) the connection pool. */
            close_ev = ngx_calloc(sizeof(ngx_rtc_stream_close_ev_t), c->log);
            if (NULL != close_ev) {
                close_ev->s = s;
                close_ev->ev.handler = ngx_rtc_stream_close_ev_handler;
                close_ev->ev.log = c->log;
                ngx_post_event(&close_ev->ev, &ngx_posted_events);
            } else {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "ngx_rtc_stream: cannot schedule UDP session "
                              "finalize for session=%s", sess->ice_ufrag);
            }
        }
    }

    /* Drive the state machine to CLOSED, then release the publisher and drop
     * the session. The release must run BEFORE unsubscribe: once the source is
     * no longer publishing, unsubscribe may reap it, and unsubscribe itself
     * clears sess->source.
     *
     * The gate is the ownership tag alone, not sess->publishing: this worker
     * reaches here both for the WHIP publisher's own session and for the
     * media-worker mirror built by attach_from_shm, and only the latter carries
     * publishing. Requiring it would leave the tag set and the shm source
     * claimed, so a later RTMP publish on the same name would be refused as a
     * cross-protocol conflict until the shm grace expired. */
    (void)ngx_rtc_session_fsm_dispatch(&sess->fsm, ev);

    if (NULL != sess->source
            && NGX_RTC_PUBLISHER_WHIP == sess->source->publisher_kind) {
        /* Clears the local mirror first, then releases the shm claim and hands
         * the shm source back if nothing else needs it. Gated on the WHIP kind
         * on purpose: an RTMP-published name is released by its own ingest
         * worker, and this worker mirroring that tag must not steal the flag. */
        ngx_rtc_publish_release(sess->source);
    }

    if (NULL != sess->source) {
        ngx_rtc_source_unsubscribe(sess->source, sess);
    }
    ngx_rtc_session_remove(sess);

    /* Phase 1: drop the cross-worker session skeleton (also unlinks it from
     * the shm source's subscriber list). Only the worker that owns the UDP
     * connection removes it; a stale per-process session in the signaling
     * worker (which the reaper closes) must not destroy a live skeleton.
     * Half-open skeletons (owner_slot == -1) are reclaimed here too. */
    {
        ngx_rtc_core_conf_t *ccf;

        ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
        if (NULL != ccf && NULL != ccf->sh) {
            ngx_rtc_shm_session_remove_if_owner(ccf->sh,
                    (u_char *)sess->ice_ufrag, ngx_strlen(sess->ice_ufrag),
                    (ngx_uint_t) ngx_worker);
        }
    }

    ngx_rtc_stream_dtls_cancel(sess);
    ngx_rtc_srtp_destroy(&sess->srtp);
    ngx_rtc_dtls_destroy(&sess->dtls);

    ngx_free(sess);
}


static void
ngx_rtc_stream_close_ev_handler(ngx_event_t *ev)
{
    ngx_rtc_stream_close_ev_t *close_ev;

    close_ev = (ngx_rtc_stream_close_ev_t *)ev;

    if (NULL != close_ev->s) {
        /* ngx_stream_finalize_session destroys the connection pool; nothing in
         * this handler may touch close_ev->s or the connection after it. */
        ngx_stream_finalize_session(close_ev->s, NGX_STREAM_OK);
        close_ev->s = NULL;
    }

    ngx_free(close_ev);
}


static void
ngx_rtc_stream_drain_ring(void)
{
    ngx_rtc_core_conf_t   *ccf;
    ngx_rtc_shm_ring_t    *ring;
    ngx_rtc_ring_entry_t   entry;
    ngx_rtc_session_t     *sess;
    ngx_uint_t             i;

    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
    if (NULL == ccf || NULL == ccf->sh) {
        return;
    }

    ring = ccf->sh->rings[ngx_worker];
    if (NULL == ring) {
        return;
    }

    while (ngx_rtc_shm_ring_dequeue(ring, &entry) == NGX_OK) {
        for (i = 0; i < entry.nsess; i++) {
            sess = ngx_rtc_session_find_by_id(entry.sess[i]);
            if (NULL != sess) {
                /* No per-session cache: NACK/PLI for this session is answered
                 * from the shm retransmit ring (cross-worker). */
                (void) ngx_rtc_session_send_rtp(sess, entry.rtp, entry.len,
                                                (uint8_t) entry.gop);
            }
        }
    }
}


static void
ngx_rtc_stream_notify_handler(ngx_event_t *ev)
{
    ngx_connection_t *c;
    uint64_t          value;
    ssize_t           n;

    c = ev->data;
    if (NULL != c) {
        n = read(c->fd, &value, sizeof(value));
        if (n == -1 && ngx_errno != NGX_EAGAIN) {
            ngx_log_error(NGX_LOG_ERR, ev->log, ngx_errno,
                          "ngx_rtc_stream: eventfd read failed");
        }
    }

    ngx_rtc_stream_drain_ring();
}


static ngx_int_t
ngx_rtc_stream_shm_gop_send(void *opaque, const uint8_t *rtp, uint32_t len,
                            uint8_t is_gop_start)
{
    ngx_rtc_session_t *sess = opaque;

    if (0 == ngx_rtc_session_send_rtp(sess, rtp, len, is_gop_start)) {
        return NGX_ERROR; /* socket full: stop the replay burst */
    }

    return NGX_OK;
}


/* Retransmit one video packet to a session, resolved from the source GOP ring
 * (same-worker, lock-free) or the shm retransmit ring (cross-worker, per-ring
 * lock). Dedup is applied in ngx_rtc_session_retransmit_send. */
static int32_t
ngx_rtc_stream_retransmit(ngx_rtc_session_t *sess, uint16_t seq)
{
    ngx_rtc_core_conf_t *ccf;
    u_char               scratch[NGX_RTC_RING_RTP_MAX];
    uint16_t             out_len;

    if (0 != ngx_rtc_source_gop_ready(sess->source)) {
        return ngx_rtc_session_retransmit(sess, seq);
    }

    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
    if (NULL == ccf || NULL == ccf->sh) {
        return NGX_RTC_ERR_PARSE;
    }

    if (ngx_rtc_shm_retransmit_get(ccf->sh, (u_char *) sess->source->name,
            ngx_strlen(sess->source->name), seq,
            scratch, sizeof(scratch), &out_len) != NGX_OK) {
        return NGX_RTC_ERR_PARSE;
    }

    return ngx_rtc_session_retransmit_send(sess, seq, scratch, out_len);
}


/* Replay the latest GOP to a session from the shared cache: the source GOP ring
 * same-worker, the shm retransmit ring cross-worker. */
static void
ngx_rtc_stream_replay_gop(ngx_rtc_session_t *sess)
{
    ngx_rtc_core_conf_t *ccf;

    if (0 != ngx_rtc_source_gop_ready(sess->source)) {
        ngx_rtc_rtp_ring_replay(&sess->source->gop, sess);
        return;
    }

    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
    if (NULL != ccf && NULL != ccf->sh) {
        (void) ngx_rtc_shm_retransmit_replay_gop(ccf->sh,
                (u_char *) sess->source->name,
                ngx_strlen(sess->source->name),
                ngx_rtc_stream_shm_gop_send, sess);
    }
}


static void
ngx_rtc_stream_reap_timer(ngx_event_t *ev)
{
    ngx_rtc_session_t   *sess;
    ngx_rtc_session_t   *next;
    ngx_rtc_core_conf_t *ccf;
    ngx_msec_t           now;
    ngx_msec_t           timeout;

    now = ngx_current_msec;
    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);

    /* Reap half-open shm skeletons and empty non-publishing sources. */
    if (NULL != ccf && NULL != ccf->sh) {
        ngx_rtc_shm_expire(ccf->sh, 0);
    }

    for (sess = ngx_rtc_session_first(); NULL != sess; sess = next) {
        /* Capture the successor before close() unlinks and frees the session. */
        next = ngx_rtc_session_next(sess);

        /* Admin kick: the control plane sets the skeleton's close_requested
         * flag; the owner worker turns it into an actual UDP close here. */
        if (NULL != ccf && NULL != ccf->sh
                && ngx_rtc_shm_session_is_close_requested(ccf->sh,
                        (u_char *) sess->ice_ufrag,
                        ngx_strlen(sess->ice_ufrag))) {
            ngx_log_error(NGX_LOG_INFO, ev->log, 0,
                          "ngx_rtc_stream: admin kick, closing session=%s",
                          sess->ice_ufrag);
            ngx_rtc_stream_session_close(sess, NGX_RTC_SESSION_EVT_CLOSE);
            continue;
        }

        timeout = ngx_rtc_session_fsm_is_ready(&sess->fsm)
                  ? ngx_rtc_stream_srv_conf->ready_timeout
                  : ngx_rtc_stream_srv_conf->handshake_timeout;

        if (now - sess->last_active >= timeout) {
            ngx_log_error(NGX_LOG_INFO, ev->log, 0,
                          "ngx_rtc_stream: idle timeout, closing session=%s",
                          sess->ice_ufrag);
            ngx_rtc_stream_session_close(sess, NGX_RTC_SESSION_EVT_TIMEOUT);
        }
    }

    ngx_add_timer(ev, ngx_rtc_stream_reap_interval());
}
