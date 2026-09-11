/*
 * ngx_rtmp_rtc_bridge_module.c - RTMP -> RTC bridge.
 *
 * Registers as a raw RTMP audio/video handler (same pattern as
 * ngx_rtmp_gop_cache_module). For the publishing session it parses the FLV
 * video tag body (H264, AVCC length-prefixed NALUs), filters B frames, and
 * packetizes each NALU into RTP (single / STAP-A / FU-A), then broadcasts the
 * plaintext RTP to every subscribed player (SRTP-protected per session).
 * Audio (AAC) is transcoded to Opus (ngx_rtc_audio) and packetized as a single
 * RTP packet per Opus frame (RFC 7587).
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

#include "ngx_rtmp.h"
#include "ngx_rtmp_live_module.h"
#include "ngx_rtmp_cmd_module.h"

#include "ngx_rtc_rtp.h"
#include "ngx_rtc_srtp.h"
#include "ngx_rtc_core.h"
#include "ngx_rtc_shm.h"
#include "ngx_rtc_glue.h"
#include "ngx_rtc_audio.h"
#include "ngx_rtc_audio_worker.h"
#include "ngx_rtc_rtcp.h"

#define NGX_RTC_VIDEO_H264  7
#define NGX_RTC_AUDIO_AAC   10
#define NGX_RTC_MAX_NALUS   16
#define NGX_RTC_BRIDGE_MAX_SNAPSHOT 256u


static ngx_int_t  ngx_rtmp_rtc_av(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
                      ngx_chain_t *in);
static ngx_int_t  ngx_rtmp_rtc_disconnect(ngx_rtmp_session_t *s,
                      ngx_rtmp_header_t *h, ngx_chain_t *in);
static void       ngx_rtmp_rtc_release_publish(ngx_rtmp_session_t *s);
static ngx_int_t  ngx_rtmp_rtc_postconfiguration(ngx_conf_t *cf);
static ngx_int_t  ngx_rtmp_rtc_init_process(ngx_cycle_t *cycle);
static void       ngx_rtmp_rtc_rtcp_timer(ngx_event_t *ev);
static ngx_msec_t ngx_rtc_rtcp_sr_interval(void);
static ngx_int_t  ngx_rtmp_rtc_video(ngx_rtc_source_t *src, ngx_rtmp_header_t *h,
                      u_char *data, u_char *last);
static ngx_int_t  ngx_rtmp_rtc_audio(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
                      ngx_chain_t *in);
static void       ngx_rtmp_rtc_parse_avc_header(ngx_rtc_source_t *src,
                      u_char *data, u_char *last);
static int32_t    ngx_rtmp_rtc_emit(void *opaque, const uint8_t *rtp,
                      uint32_t len);
static int32_t    ngx_rtmp_rtc_audio_frame(void *opaque, const uint8_t *opus,
                      uint32_t len);
static void      *ngx_rtmp_rtc_create_main_conf(ngx_conf_t *cf);
static char      *ngx_rtmp_rtc_init_main_conf(ngx_conf_t *cf, void *conf);


typedef struct {
    ngx_rtc_source_t *src;
} ngx_rtmp_rtc_audio_ctx_t;


/*
 * Context threaded through the RTP packetizers into ngx_rtmp_rtc_emit.
 * is_gop_start is set for the STAP-A (SPS/PPS) that precedes an IDR so the GOP
 * ring can remember the start of the current keyframe access unit.
 */
typedef struct {
    ngx_rtc_source_t *src;
    uint8_t           is_gop_start;
    uint8_t           is_video;   /* 1 = H264 video, 0 = Opus audio */
} ngx_rtc_emit_ctx_t;

typedef struct {
    ngx_uint_t  audio_bitrate;      /* Opus target bitrate */
    ngx_msec_t  rtcp_sr_interval;   /* sender-report interval in ms */
} ngx_rtmp_rtc_main_conf_t;

/* Publisher teardown runs from the AMF_CMD event, not a close_stream hook: the
 * close_stream pointer is assigned outright by the cmd and live modules, which
 * postconfigure after this one, so a hook installed here never fires. */
static ngx_rtmp_rtc_main_conf_t *ngx_rtmp_rtc_main_conf;

/* One-shot per worker. A raw AAC frame arriving with no transcoder handle means
 * the AAC sequence header was never accepted (missing, over-long, or arriving
 * after this frame), and the stream then carries no audio until the next
 * republish. That failure was silent: the only symptoms were audio RTP packet
 * count 0 and askew=0 in the avsync line, with nothing in error.log -- see the
 * 2026-09-10 investigation in docs/. Warn once so the next occurrence names the
 * stream immediately instead of being reconstructed from counters. */
static ngx_uint_t ngx_rtmp_rtc_audio_noctx_warned;


static ngx_command_t ngx_rtmp_rtc_commands[] = {

    { ngx_string("rtc_audio_bitrate"),
      NGX_RTMP_MAIN_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_RTMP_MAIN_CONF_OFFSET,
      offsetof(ngx_rtmp_rtc_main_conf_t, audio_bitrate),
      NULL },

    { ngx_string("rtc_rtcp_sr_interval"),
      NGX_RTMP_MAIN_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_RTMP_MAIN_CONF_OFFSET,
      offsetof(ngx_rtmp_rtc_main_conf_t, rtcp_sr_interval),
      NULL },

      ngx_null_command
};


static ngx_rtmp_module_t ngx_rtmp_rtc_module_ctx = {
    NULL,                              /* preconfiguration */
    ngx_rtmp_rtc_postconfiguration,    /* postconfiguration */
    ngx_rtmp_rtc_create_main_conf,     /* create main configuration */
    ngx_rtmp_rtc_init_main_conf,       /* init main configuration */
    NULL,                              /* create server configuration */
    NULL,                              /* merge server configuration */
    NULL,                              /* create application configuration */
    NULL                               /* merge application configuration */
};


ngx_module_t ngx_rtmp_rtc_bridge_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_rtc_module_ctx,          /* module context */
    ngx_rtmp_rtc_commands,             /* module directives */
    NGX_RTMP_MODULE,                   /* module type */
    NULL,                              /* init master */
    NULL,                              /* init module */
    ngx_rtmp_rtc_init_process,         /* init process */
    NULL,                              /* init thread */
    NULL,                              /* exit thread */
    NULL,                              /* exit process */
    NULL,                              /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_rtmp_rtc_create_main_conf(ngx_conf_t *cf)
{
    ngx_rtmp_rtc_main_conf_t *mcf;

    mcf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_rtc_main_conf_t));
    if (NULL == mcf) {
        return NULL;
    }

    mcf->audio_bitrate = NGX_CONF_UNSET_UINT;
    mcf->rtcp_sr_interval = NGX_CONF_UNSET_MSEC;

    return mcf;
}


static char *
ngx_rtmp_rtc_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_rtmp_rtc_main_conf_t *mcf = conf;

    ngx_conf_init_uint_value(mcf->audio_bitrate, 64000);
    ngx_conf_init_msec_value(mcf->rtcp_sr_interval, 2000);

    ngx_rtmp_rtc_main_conf = mcf;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_rtmp_rtc_postconfiguration(ngx_conf_t *cf)
{
    ngx_rtmp_core_main_conf_t *cmcf;
    ngx_rtmp_handler_pt       *h;
    ngx_rtmp_rtc_main_conf_t  *mcf;

    cmcf = ngx_rtmp_conf_get_module_main_conf(cf, ngx_rtmp_core_module);
    mcf = ngx_rtmp_conf_get_module_main_conf(cf, ngx_rtmp_rtc_bridge_module);
    ngx_rtmp_rtc_main_conf = mcf;

    h = ngx_array_push(&cmcf->events[NGX_RTMP_MSG_VIDEO]);
    *h = ngx_rtmp_rtc_av;

    h = ngx_array_push(&cmcf->events[NGX_RTMP_MSG_AUDIO]);
    *h = ngx_rtmp_rtc_av;

    /*
     * Publisher teardown hangs off the disconnect event, not the
     * ngx_rtmp_close_stream pointer. That pointer is assigned outright with no
     * next-save by ngx_rtmp_cmd_module and ngx_rtmp_live_module, both of which
     * postconfigure after this module, so a hook installed here would be
     * overwritten and never fire; the events[] arrays are appended to, so a
     * handler registered here always runs.
     *
     * DISCONNECT rather than the AMF command handlers: ngx_rtmp_cmd_module
     * registers those in cmcf->amf (a separate table keyed by command name) and
     * routes a disconnect through deleteStream, so every publish teardown --
     * clean deleteStream, a client that just drops the socket -- reaches this
     * one handler.
     */
    h = ngx_array_push(&cmcf->events[NGX_RTMP_DISCONNECT]);
    *h = ngx_rtmp_rtc_disconnect;

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_rtc_disconnect(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
                        ngx_chain_t *in)
{
    ngx_rtmp_rtc_release_publish(s);

    return NGX_OK;
}


/*
 * Build this session's "app/stream" name for the process-local source
 * registry. NGX_ERROR means the name cannot be represented: either of the two
 * halves is empty, or "app/stream" does not fit cap. ngx_snprintf truncates
 * rather than failing, and a clipped name addresses a *different* source, so
 * the overflow has to be caught here -- the callers treat NGX_ERROR the same
 * as "no such source", which is exactly what a nameless stream is.
 * cap is normally NGX_RTC_SOURCE_NAME_MAX, the same limit ngx_rtc_source_get()
 * enforces, so anything this accepts is a name the registry can hold.
 */
static ngx_int_t
ngx_rtmp_rtc_source_name(ngx_rtmp_session_t *s, char *out, size_t cap)
{
    if (0 == s->app.len || 0 == s->stream.len
            || s->app.len + s->stream.len + 2 > cap) {
        return NGX_ERROR;
    }

    ngx_snprintf((u_char *) out, cap - 1, "%*s/%*s%Z",
                 s->app.len, s->app.data, s->stream.len, s->stream.data);

    return NGX_OK;
}


/*
 * Release the RTMP publish claim for the session's stream. Idempotent and a
 * no-op unless this worker actually owns the RTMP kind, so it is safe to call
 * from every deleteStream/closeStream and again on disconnect.
 */
static void
ngx_rtmp_rtc_release_publish(ngx_rtmp_session_t *s)
{
    ngx_rtc_source_t    *src;
    char                 name[NGX_RTC_SOURCE_NAME_MAX];

    if (0 != s->auto_pushed
            || NGX_OK != ngx_rtmp_rtc_source_name(s, name, sizeof(name))) {
        return;
    }

    src = ngx_rtc_source_find(name);
    if (NULL == src) {
        return;
    }

    /* The AAC->Opus transcode context is owned by this local source and must
     * not outlive the publisher, whatever the ownership tag says. Destroying
     * it only past the publisher_kind gate below left it behind on any
     * teardown that found the tag already cleared, stranding the transcoder
     * thread and its queues with no owner left to stop them. */
    if (NULL != src->audio_ctx) {
        ngx_rtc_audio_worker_destroy((ngx_rtc_audio_worker_t *) src->audio_ctx);
        src->audio_ctx = NULL;
    }

    if (NGX_RTC_PUBLISHER_RTMP != src->publisher_kind) {
        return;
    }

    /* Clears the local mirror, releases the shm claim, and hands the shm source
     * back if nothing else needs it. This must run before the local remove
     * below: that one frees the source once it is neither publishing nor
     * subscribed, so it cannot be the first step. */
    ngx_rtc_publish_release(src);

    ngx_rtc_source_remove(name);
}


/* Worker-wide timer that periodically sends an SR + SDES compound report to
 * every subscribed player so the client can measure RTT and keep stats. */
static ngx_event_t  ngx_rtmp_rtc_rtcp_timer_ev;


static void
ngx_rtmp_rtc_send_sr_sdes(ngx_rtc_session_t *sess, ngx_rtc_source_t *src)
{
    ngx_connection_t *c;
    ngx_rtc_rtcp_sr_t sr;
    uint8_t           compound[NGX_RTC_RTCP_MAX_PACKET];
    uint8_t           sub[NGX_RTC_RTCP_MAX_PACKET];
    uint32_t          out_len;
    uint32_t          sub_len;
    int               n;
    ssize_t           sent;

    out_len = 0;

    ngx_memzero(&sr, sizeof(sr));
    sr.ssrc = src->video_ssrc;
    sr.rtp_ts = src->video_ts;
    sr.packet_count = src->video_pkts;
    sr.octet_count = src->video_octets;
    sr.ntp = 0;   /* 0 = fill from wall clock */

    if (NGX_RTC_OK == ngx_rtc_rtcp_encode_sr(&sr, sub, sizeof(sub), &sub_len)) {
        if (NGX_RTC_OK != ngx_rtc_rtcp_compound_append(compound, sizeof(compound),
                                                &out_len, sub, sub_len)) {
            return;
        }
    }

    if (NGX_RTC_OK == ngx_rtc_rtcp_encode_sdes(sr.ssrc, "ngx-rtc", sub, sizeof(sub),
                                               &sub_len)) {
        if (NGX_RTC_OK != ngx_rtc_rtcp_compound_append(compound, sizeof(compound),
                                                &out_len, sub, sub_len)) {
            return;
        }
    }

    if (0 == out_len) {
        return;
    }

    n = (int)out_len;
    if (0 != ngx_rtc_srtp_protect_rtcp(&sess->srtp, compound, &n)) {
        return;
    }

    c = (ngx_connection_t *)sess->conn;
    if (NULL != c) {
        sent = c->send(c, compound, (size_t)n);
        if (sent == NGX_ERROR) {
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, ngx_socket_errno,
                       "ngx_rtc: RTCP sender report send failed");
        }
    }
}


static void
ngx_rtmp_rtc_rtcp_timer(ngx_event_t *ev)
{
    ngx_rtc_source_t     *src;
    ngx_rtc_source_t     *next_src;
    ngx_rtc_session_t    *sess;
    ngx_rtc_core_conf_t  *ccf;

    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);

    /* Capture the successor before the body, for the same reason as the inner
     * subscriber loop: send_sr_sdes can close a session, and the resulting
     * unsubscribe frees the source once it has no subscribers left. Reading
     * ngx_rtc_source_next(src) after the body would dereference the freed node
     * (observed as SIGSEGV in the backtrace handler from a reaper-driven close
     * that raced this timer). */
    for (src = ngx_rtc_source_first(); NULL != src; src = next_src) {
        next_src = ngx_rtc_source_next(src);

        if (0 == src->video_ssrc) {
            continue;
        }
        /* Capture the successor before the callback: send_sr_sdes may close
         * and free the session (send failure / fsm transition), which unlinks
         * it from the subscriber queue and would leave the loop's own
         * ngx_queue_next(&sess->sub_queue) walking a freed node. */
        for (sess = ngx_rtc_source_first_subscriber(src); NULL != sess;) {
            ngx_rtc_session_t *next = ngx_rtc_source_next_subscriber(src, sess);

            if (NULL != sess->conn
                    && ngx_rtc_session_fsm_is_ready(&sess->fsm)) {
                ngx_rtmp_rtc_send_sr_sdes(sess, src);

                /* Mirror the egress counters on the SR interval as well. The
                 * TWCC path only fires for a client that negotiated
                 * transport-cc, so a session without it would read as
                 * pacer_bps 0 in /rtc/v1/stats even though its pacer is armed
                 * and may be dropping. */
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
            sess = next;
        }

        /* A/V-skew observability (avsync Phase 1). Video ts is re-derived from
         * the RTMP clock each frame; audio ts is a free-running 48 kHz Opus
         * counter seeded once. All skews here are in wall-clock ms: vskew is
         * ~0 by construction (sanity baseline), a *constant* askew is expected,
         * and a *drifting* askew / av means one timeline is losing or gaining
         * samples (drops, resample, pause). */
        if (1 == src->publishing) {
            ngx_int_t  vms;
            ngx_int_t  ams;
            ngx_int_t  vskew;
            ngx_int_t  askew;
            ngx_int_t  av;
            uint32_t   idr;
            uint32_t   odr;

            vms = (ngx_int_t)(src->video_ts / 90);
            ams = (ngx_int_t)(src->audio_ts / 48);
            vskew = vms - (ngx_int_t) src->last_video_rtmp_ms;
            askew = (0 != src->audio_ts_valid)
                    ? ams - (ngx_int_t) src->last_audio_rtmp_ms : 0;
            av = (0 != src->audio_ts_valid) ? vms - ams : 0;

            idr = 0;
            odr = 0;
            if (NULL != src->audio_ctx) {
                ngx_rtc_audio_worker_drop_stats(
                        (ngx_rtc_audio_worker_t *) src->audio_ctx,
                        &idr, &odr);
            }

            ngx_log_error(NGX_LOG_INFO, ev->log, 0,
                          "ngx_rtmp_rtc: avsync src=\"%s\" vskew=%ims "
                          "askew=%ims av=%ims ring_drops=%ui adrop_in=%ui "
                          "adrop_out=%ui",
                          src->name, vskew, askew, av,
                          (ngx_uint_t) src->ring_drops, (ngx_uint_t) idr,
                          (ngx_uint_t) odr);
        }
    }

    ngx_add_timer(ev, ngx_rtc_rtcp_sr_interval());
}


/* Safe RTCP SR cadence. The rtmp{} directive parser may leave the addon main
 * conf unset (NGX_CONF_UNSET_MSEC ~ 49 days); clamp to 2 s so the sender-report
 * / A/V-skew timer is always live. */
static ngx_msec_t
ngx_rtc_rtcp_sr_interval(void)
{
    ngx_msec_t v;

    v = (NULL != ngx_rtmp_rtc_main_conf)
        ? ngx_rtmp_rtc_main_conf->rtcp_sr_interval : NGX_CONF_UNSET_MSEC;
    if (NGX_CONF_UNSET_MSEC == v || 0 == v || v > 60000) {
        v = 2000;
    }

    return v;
}


/* Arm the RTCP sender-report / observability timer for the calling worker.
 * Exported because two init_process handlers call it: nginx walks
 * cycle->modules[] with no module-type filter (ngx_process_cycle.c:966 for the
 * worker process, :297 for single-process mode), so an NGX_RTMP_MODULE's
 * init_process does run after all - this module's handler is merely ordered
 * first (ngx_modules.c emits ngx_rtmp_rtc_bridge_module before
 * ngx_rtc_http_module), and the http module's call then re-arms the same event.
 * ngx_add_timer on an already-armed event only resets the deadline, so the
 * duplicate is harmless. Worker-only. */
void
ngx_rtmp_rtc_rtcp_timer_start(ngx_cycle_t *cycle)
{
    if (ngx_process != NGX_PROCESS_WORKER) {
        return;
    }

    ngx_memzero(&ngx_rtmp_rtc_rtcp_timer_ev, sizeof(ngx_rtmp_rtc_rtcp_timer_ev));
    ngx_rtmp_rtc_rtcp_timer_ev.handler = ngx_rtmp_rtc_rtcp_timer;
    ngx_rtmp_rtc_rtcp_timer_ev.log = cycle->log;
    ngx_rtmp_rtc_rtcp_timer_ev.data = NULL;

    ngx_add_timer(&ngx_rtmp_rtc_rtcp_timer_ev, ngx_rtc_rtcp_sr_interval());
}


static ngx_int_t
ngx_rtmp_rtc_init_process(ngx_cycle_t *cycle)
{
    /* Runs in every worker ahead of the http module's handler; the second
     * ngx_add_timer is harmless (see ngx_rtmp_rtc_rtcp_timer_start above). */
    ngx_rtmp_rtc_rtcp_timer_start(cycle);

    return NGX_OK;
}


/*
 * Concatenate an ngx_chain_t body into dst (cap bytes). nginx-rtmp delivers a
 * message larger than the chunk size as a chain of chunk-sized buffers; the NALU
 * parser needs one contiguous view, so this walks the chain once. Returns the
 * total byte count, or -1 when the body does not fit in cap.
 */
static ssize_t
ngx_rtmp_rtc_chain_copy(ngx_chain_t *in, u_char *dst, size_t cap)
{
    u_char *p = dst;
    size_t  n;
    size_t  total = 0;

    for (; NULL != in; in = in->next) {
        if (NULL == in->buf) {
            continue;
        }
        n = (size_t)(in->buf->last - in->buf->pos);
        if (0 == n) {
            continue;
        }
        if (total + n > cap) {
            return -1;
        }
        ngx_memcpy(p, in->buf->pos, n);
        p += n;
        total += n;
    }

    return (ssize_t)total;
}


/*
 * Phase 1: sync SSRC/PT from the cross-worker shm registry (the authoritative
 * copy). The signaling worker allocates SSRC/PT on the first play and mirrors
 * them into shm; a publisher landing on a different worker must tag RTP with the
 * same values, otherwise the SDP answer and the media SSRC disagree. When the
 * publisher arrives first (no viewer yet) it allocates the SSRC/PT into shm so a
 * later play reuses them.
 */
/* Mirror the per-process counters into the claimed shm source. Lock-free by
 * design: the shm source stays alive while its publisher runs (publishing == 1
 * keeps both the reaper and remove away) and the counter words are naturally
 * aligned, so a cross-worker reader never tears one. Called per media packet,
 * so it must not take the pool mutex. */
static void
ngx_rtmp_rtc_shm_stats(ngx_rtc_source_t *src)
{
    ngx_rtc_shm_source_t *shm_src = (ngx_rtc_shm_source_t *) src->shm_src;

    if (NULL == shm_src) {
        return;
    }

    shm_src->video_pkts = src->video_pkts;
    shm_src->video_octets = src->video_octets;
    shm_src->audio_pkts = src->audio_pkts;
    shm_src->audio_octets = src->audio_octets;

    /* Heartbeat. This is the only shm write on the per-packet publish path, so
     * it carries the liveness signal: the reaper reclaims a publishing source
     * once this stops advancing, which is the only way to collect one whose
     * worker died without running ngx_rtc_publish_release(). */
    shm_src->publisher_seen_ms = (ngx_atomic_t) ngx_current_msec;
}


static ngx_int_t
ngx_rtmp_rtc_sync_shm(ngx_rtc_source_t *src)
{
    ngx_rtc_core_conf_t  *ccf;
    ngx_rtc_shm_source_t *shm_src;
    ngx_msec_t            now;

    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
    if (NULL == ccf || NULL == ccf->sh) {
        return NGX_OK; /* no cross-worker shm: single-worker mode */
    }

    /* Fast path: this process already claimed RTMP for this name and resolved
     * the shm source within the last interval, so the per-packet media path
     * skips the pool-mutex rbtree lookup, the ownership re-claim and the
     * SSRC/PT re-read (the SSRC/PT recorded at creation are reused). The
     * interval bounds how long a pointer that another worker's release made
     * reapable is still trusted.
     *
     * The kind check is load-bearing: src->shm_src can also be set by
     * attach_from_shm() for a WHIP-published name that this same worker happens
     * to serve. Without it, the cache would short-circuit the ownership
     * arbitration and let an RTMP ingest run alongside a live WHIP publisher.
     * Anything that is not already ours falls through to the claim below. */
    now = ngx_current_msec;
    if (NGX_RTC_PUBLISHER_RTMP == src->publisher_kind
            && NULL != src->shm_src
            && (ngx_msec_int_t) (now - src->shm_sync_ms)
                   < (ngx_msec_int_t) NGX_RTC_SHM_SYNC_MS) {
        ngx_rtmp_rtc_shm_stats(src);
        return NGX_OK;
    }

    /* Claim (or renew) publish ownership. NGX_BUSY means another protocol
     * already publishes this name: reject the ingest. On success src->shm_src
     * may still be NULL -- no rtc_zone, or the source vanished mid-claim --
     * which degrades to "no shm mirror" rather than rejecting. The claim also
     * clears any stale local tag, so a dead WHIP name never blocks the
     * takeover this call is trying to perform. */
    if (NGX_BUSY == ngx_rtc_publish_claim(src, NGX_RTC_PUBLISHER_RTMP)) {
        return NGX_BUSY;
    }

    shm_src = src->shm_src;
    if (NULL == shm_src) {
        return NGX_OK;
    }

    src->video_ssrc = shm_src->video_ssrc;
    src->audio_ssrc = shm_src->audio_ssrc;
    src->video_pt = shm_src->video_pt;
    src->audio_pt = shm_src->audio_pt;

    /* Stats authority lives in shm so /rtc/v1/stats on any worker sees the
     * whole registry. sync_shm only runs on the RTMP ingest worker, so this is
     * where the publisher's slot is recorded for the cross-worker check in
     * /rtc/v1/stats. */
    shm_src->publisher_slot = (ngx_int_t) ngx_worker;
    ngx_rtmp_rtc_shm_stats(src);

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_rtc_av(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h, ngx_chain_t *in)
{
    ngx_rtmp_live_ctx_t *ctx;
    ngx_rtc_source_t    *src;
    u_char              *pos;
    u_char              *last;
    char                 name[NGX_RTC_SOURCE_NAME_MAX];
    ssize_t              body_len;

    if (NULL == in || NULL == in->buf) {
        return NGX_OK;
    }

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);
    if (NULL == ctx || 0 == ctx->publishing) {
        return NGX_OK;
    }

    /*
     * Skip auto_push relay copies. With `rtmp_auto_push on`, the worker that
     * accepted the publish relays it over a unix socket to every sibling,
     * which re-runs this handler for the same app/stream. Those copies carry
     * no real publisher: they reconnect about every 100 ms while the peer's
     * source is gone, and each pass calls sync_shm, whose try_publish re-arms
     * publishing=1 in the shm source. The name then never becomes reapable, so
     * after the real publisher leaves no protocol can reclaim it. Only the
     * accepting worker (auto_pushed == 0) owns the publish right. */
    if (0 != s->auto_pushed) {
        return NGX_OK;
    }

    pos = in->buf->pos;
    last = in->buf->last;

    if (h->type == NGX_RTMP_MSG_AUDIO) {
        return ngx_rtmp_rtc_audio(s, h, in);
    }

    if (h->type != NGX_RTMP_MSG_VIDEO) {
        return NGX_OK;
    }

    if (last - pos < 5) {
        return NGX_OK;
    }

    if (NGX_RTC_VIDEO_H264 != (pos[0] & 0x0f)) {
        return NGX_OK;
    }

    if (NGX_OK != ngx_rtmp_rtc_source_name(s, name, sizeof(name))) {
        return NGX_OK;
    }

    src = ngx_rtc_source_get(name);
    if (NULL == src) {
        return NGX_OK;
    }
    /*
     * Ownership is arbitrated by the shm source, not by this process-local tag.
     * The tag is a per-worker cache: a WHIP publish is claimed by the HTTP
     * worker and mirrored by the media worker through attach_from_shm, so a
     * close on one worker can leave the other holding a stale WHIP tag after
     * the shm claim is already released. Trusting the tag alone made a name
     * unpublishable over RTMP until the whole process was restarted.
     *
     * sync_shm re-claims in shm and reports NGX_BUSY only while another
     * protocol really holds the name, so it is the deciding call. The claim
     * inside it also rewrites the local tag, so no pre-clearing is needed here.
     */
    if (NGX_OK != ngx_rtmp_rtc_sync_shm(src)) {
        /* Another protocol publishes this name (WHIP): reject the RTMP ingest. */
        return NGX_ERROR;
    }

    /* pos[0]=codec, pos[1]=AVCPacketType, pos[2..4]=CTS, then AVCC NALUs. */
    if (0 == pos[1]) {
        /* A new AVC sequence header means a (re)publish started. Reset the
         * video RTP state so seq/timestamp stay monotonic across restarts. */
        src->sps_len = 0;
        src->pps_len = 0;
        src->sps_profile_level_id_valid = 0;
        src->video_seq = 0;
        src->video_ts = 0;
        src->have_ts = 0;
        src->video_pkts = 0;
        src->video_octets = 0;

        /* Drop stale video caches: a restart resets the RTP sequence to 0, so
         * an old packet with a colliding seq would be served as a wrong
         * fast-start / NACK retransmit. */
        src->gop.count = 0;
        src->gop.gop_start = 0;
        {
            ngx_rtc_core_conf_t *ccf;

            ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
            if (NULL != ccf && NULL != ccf->sh) {
                ngx_rtc_shm_retransmit_reset(ccf->sh,
                        (u_char *) name, ngx_strlen(name));
            }
        }

        /* The AVC sequence header may also span chain buffers; concatenate it
         * before parsing SPS/PPS. */
        body_len = ngx_rtmp_rtc_chain_copy(in, src->video_body,
                                           sizeof(src->video_body));
        if (body_len < 5) {
            return NGX_OK;
        }
        ngx_rtmp_rtc_parse_avc_header(src, src->video_body + 5,
                                      src->video_body + body_len);
        return NGX_OK;
    }

    if (1 == pos[1]) {
        /* The tag body may span several chain buffers; concatenate it before
         * parsing the length-prefixed NALUs (otherwise large frames truncate). */
        body_len = ngx_rtmp_rtc_chain_copy(in, src->video_body,
                                           sizeof(src->video_body));
        if (body_len < 5) {
            return NGX_OK;
        }
        return ngx_rtmp_rtc_video(src, h, src->video_body + 5,
                                  src->video_body + body_len);
    }

    return NGX_OK;
}


/*
 * Parse the AVCDecoderConfigurationRecord and cache SPS/PPS. Layout:
 *   [0]=version [1]=profile [2]=compat [3]=level
 *   [4]=lengthSizeMinusOne(high 6 bits)
 *   [5]=numSPS(low 5 bits), then per SPS: 2-byte len + bytes
 *   then numPPS(1 byte), then per PPS: 2-byte len + bytes
 */
static void
ngx_rtmp_rtc_parse_avc_header(ngx_rtc_source_t *src, u_char *data, u_char *last)
{
    u_char   *p;
    ngx_uint_t i;
    ngx_uint_t n;
    ngx_uint_t len;

    if (last - data < 6) {
        return;
    }

    p = data + 6;

    n = data[5] & 0x1f;
    for (i = 0; i < n && p + 2 <= last; i++) {
        len = ((ngx_uint_t)p[0] << 8) | p[1];
        p += 2;
        if (p + len > last) {
            return;
        }
        if (0 == src->sps_len && len < sizeof(src->sps)) {
            src->sps_len = len;
            ngx_memcpy(src->sps, p, len);
            /* SPS layout: [0]=NAL header, [1]=profile_idc, [2]=constraint
             * flags, [3]=level_idc. Cache the 3 profile-level-id bytes so the
             * SDP answer can echo the real stream profile. */
            if (len >= 4u) {
                src->sps_profile_level_id[0] = p[1];
                src->sps_profile_level_id[1] = p[2];
                src->sps_profile_level_id[2] = p[3];
                src->sps_profile_level_id_valid = 1;
            }
        }
        p += len;
    }

    if (p + 1 > last) {
        return;
    }

    n = p[0];
    p += 1;

    for (i = 0; i < n && p + 2 <= last; i++) {
        len = ((ngx_uint_t)p[0] << 8) | p[1];
        p += 2;
        if (p + len > last) {
            return;
        }
        if (0 == src->pps_len && len < sizeof(src->pps)) {
            src->pps_len = len;
            ngx_memcpy(src->pps, p, len);
        }
        p += len;
    }
}


static ngx_int_t
ngx_rtmp_rtc_video(ngx_rtc_source_t *src, ngx_rtmp_header_t *h,
        u_char *data, u_char *last)
{
    const uint8_t *nalus[NGX_RTC_MAX_NALUS];
    uint32_t       sizes[NGX_RTC_MAX_NALUS];
    uint32_t       count;
    uint32_t       nalu_len;
    uint32_t       i;
    uint32_t       ts;
    uint8_t        scratch[1500];
    uint32_t       marker;
    int32_t        rc;
    ngx_rtc_emit_ctx_t ectx;

    /* Collect length-prefixed NALUs. */
    count = 0;
    while (data + 4 <= last && count < NGX_RTC_MAX_NALUS) {
        nalu_len = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
                 | ((uint32_t)data[2] << 8) | (uint32_t)data[3];
        data += 4;
        if (data + nalu_len > last) {
            break;
        }
        nalus[count] = data;
        sizes[count] = nalu_len;
        count++;
        data += nalu_len;
    }

    if (0 == count) {
        return NGX_OK;
    }

    ts = ngx_rtc_h264_timestamp_from_ms(h->timestamp);
    src->video_ts = ts;
    src->last_video_rtmp_ms = (uint32_t) h->timestamp;

    ectx.src = src;
    ectx.is_video = 1;
    ectx.is_gop_start = 0;

    for (i = 0; i < count; i++) {
        /* Drop B frames (WebRTC low-latency playout does not support them). */
        if (1 == ngx_rtc_h264_is_b_frame(nalus[i], sizes[i])) {
            continue;
        }

        marker = (i == count - 1) ? 1 : 0;

        /* Prepend SPS+PPS (STAP-A) before an IDR so the decoder can start. */
        if (NGX_RTC_NALU_IDR == ngx_rtc_h264_nalu_type(nalus[i], sizes[i])
                && src->sps_len > 0 && src->pps_len > 0) {
            const uint8_t *stap[2];
            uint32_t       stap_len[2];

            stap[0] = src->sps;
            stap[1] = src->pps;
            stap_len[0] = src->sps_len;
            stap_len[1] = src->pps_len;

            /* Mark the STAP-A that opens the GOP so fast-start replay can
             * begin at a decodable keyframe. */
            ectx.is_gop_start = 1;
            ngx_rtc_h264_packetize_stap_a(stap, stap_len, 2, ts,
                    &src->video_seq, src->video_ssrc, src->video_pt,
                    scratch, sizeof(scratch), ngx_rtmp_rtc_emit, &ectx);
            ectx.is_gop_start = 0;
        }

        rc = ngx_rtc_h264_packetize(nalus[i], sizes[i], ts,
                &src->video_seq, src->video_ssrc, src->video_pt, (int32_t)marker,
                scratch, sizeof(scratch), ngx_rtmp_rtc_emit, &ectx);
        if (rc != NGX_RTC_OK) {
            break;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_rtc_audio(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h, ngx_chain_t *in)
{
    ngx_rtc_source_t *src;
    char              name[NGX_RTC_SOURCE_NAME_MAX];
    uint8_t           sound_format;
    uint8_t           aac_packet_type;
    uint32_t          asc_len;
    ssize_t           body_len;
    u_char           *pos;

    if (NULL == in || NULL == in->buf) {
        return NGX_OK;
    }

    pos = in->buf->pos;
    if (in->buf->last - pos < 2) {
        return NGX_OK;
    }

    sound_format = (uint8_t)((pos[0] >> 4) & 0x0f);
    if (NGX_RTC_AUDIO_AAC != sound_format) {
        return NGX_OK;
    }

    if (NGX_OK != ngx_rtmp_rtc_source_name(s, name, sizeof(name))) {
        return NGX_OK;
    }

    src = ngx_rtc_source_get(name);
    if (NULL == src) {
        return NGX_OK;
    }
    /* See ngx_rtmp_rtc_av: the shm source arbitrates ownership, and the claim
     * inside sync_shm rewrites the local tag, so no pre-clearing is needed. */
    if (NGX_OK != ngx_rtmp_rtc_sync_shm(src)) {
        /* Another protocol publishes this name (WHIP): reject the RTMP ingest. */
        return NGX_ERROR;
    }

    /* The tag body may span several chain buffers; concatenate it before
     * reading the FLV audio header + raw AAC frame (otherwise large tags
     * truncate). */
    body_len = ngx_rtmp_rtc_chain_copy(in, src->audio_body,
                                       sizeof(src->audio_body));
    if (body_len < 2) {
        return NGX_OK;
    }

    pos = src->audio_body;
    aac_packet_type = pos[1];

    if (0 == aac_packet_type) {
        /* Sequence header: AudioSpecificConfig is the raw-AAC extradata. */
        asc_len = (uint32_t)(body_len - 2);
        if (asc_len < 2 || asc_len > sizeof(src->audio_asc)) {
            /* Silent before: this leaves audio_ctx NULL, so every later raw
             * frame is skipped and the stream has no audio at all. */
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "ngx_rtmp_rtc: AAC sequence header rejected, "
                          "stream=%s asc_len=%uD body_len=%uz asc_cap=%uz; "
                          "stream will have no audio", name, asc_len,
                          (size_t) body_len, sizeof(src->audio_asc));
            return NGX_OK;
        }

        ngx_memcpy(src->audio_asc, pos + 2, asc_len);
        src->audio_asc_len = asc_len;

        /* A new AAC sequence header means a (re)publish started: reset the
         * audio RTP state so the timestamp/seq stay monotonic across restarts. */
        if (NULL != src->audio_ctx) {
            ngx_rtc_audio_worker_destroy(
                    (ngx_rtc_audio_worker_t *)src->audio_ctx);
            src->audio_ctx = NULL;
        }
        src->audio_seq = 0;
        src->audio_ts = 0;
        src->audio_ts_valid = 0;
        src->audio_pkts = 0;
        src->audio_octets = 0;
        src->audio_ctx = ngx_rtc_audio_worker_create(
                src->audio_asc, src->audio_asc_len,
                ngx_rtmp_rtc_main_conf->audio_bitrate);
        if (NULL == src->audio_ctx) {
            /* Transcode offload failed (allocation / codec / thread); the
             * stream keeps video but has no audio until the next republish. */
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "ngx_rtmp_rtc: audio worker create failed, "
                          "stream=%s has no audio", name);
        }
        return NGX_OK;
    }

    if (1 != aac_packet_type) {
        return NGX_OK;
    }

    if (NULL == src->audio_ctx) {
        /* Every raw frame lands here for the rest of the publish, so warn
         * once per worker rather than per frame. */
        if (0 == ngx_rtmp_rtc_audio_noctx_warned) {
            ngx_rtmp_rtc_audio_noctx_warned = 1;
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "ngx_rtmp_rtc: raw AAC frame with no transcoder "
                          "handle, stream=%s; the AAC sequence header was "
                          "never accepted, so this stream has no audio until "
                          "the next republish", name);
        }
        return NGX_OK;
    }

    {
        ngx_rtmp_rtc_audio_ctx_t actx;
        uint32_t                 aac_len;

        /* Seed the source-level 48 kHz clock on the first raw AAC frame. The
         * transcoder thread emits raw Opus in FIFO order; ngx_rtmp_rtc_audio_frame
         * below advances the timestamp per frame, exactly as the old synchronous
         * path did. */
        src->last_audio_rtmp_ms = (uint32_t) h->timestamp;
        if (0 == src->audio_ts_valid) {
            src->audio_ts = ngx_rtc_opus_timestamp_from_ms(h->timestamp);
            src->audio_ts_valid = 1;
        }

        /* Deliver any Opus the thread finished since the last tag, then enqueue
         * this raw AAC frame for offloaded transcoding. */
        actx.src = src;
        (void)ngx_rtc_audio_worker_drain(
                (ngx_rtc_audio_worker_t *)src->audio_ctx,
                ngx_rtmp_rtc_audio_frame, &actx);

        aac_len = (uint32_t)(body_len - 2);
        (void)ngx_rtc_audio_worker_push(
                (ngx_rtc_audio_worker_t *)src->audio_ctx, pos + 2, aac_len);
    }

    return NGX_OK;
}


static int32_t
ngx_rtmp_rtc_audio_frame(void *opaque, const uint8_t *opus, uint32_t len)
{
    ngx_rtmp_rtc_audio_ctx_t *ctx = opaque;
    ngx_rtc_emit_ctx_t        ectx;
    uint8_t                   scratch[1500];
    uint32_t                  ts;

    /* Monotonic Opus RTP timestamp: one Opus frame == 960 samples @ 48 kHz. */
    ts = ctx->src->audio_ts;
    ctx->src->audio_ts += NGX_RTC_AUDIO_OPUS_FRAME_SIZE;

    ectx.src = ctx->src;
    ectx.is_gop_start = 0;
    ectx.is_video = 0;

    return ngx_rtc_opus_packetize(opus, len, ts,
            &ctx->src->audio_seq, ctx->src->audio_ssrc, ctx->src->audio_pt,
            1, scratch, sizeof(scratch), ngx_rtmp_rtc_emit, &ectx);
}


/* Phase 2 media-plane producer: snapshot the shm subscription table, group the
 * ready sessions by owner_slot, and enqueue one plaintext RTP entry per target
 * worker's ring. The owner worker later dequeues and applies per-session SRTP.
 */
void
ngx_rtc_broadcast_rtp(ngx_rtc_source_t *src, const uint8_t *rtp,
                      uint32_t len, uint8_t is_video, uint8_t is_gop_start)
{
    ngx_rtc_core_conf_t   *ccf;
    ngx_rtc_shm_ctx_t     *shm;
    ngx_rtc_shm_source_t  *shm_src;
    ngx_uint_t             n;
    ngx_uint_t             i;
    ngx_uint_t             w;
    ngx_uint_t             nsess;
    ngx_uint_t             sess_ids[NGX_RTC_RING_MAX_SESSIONS];
    ngx_rtc_session_t     *sess;

    if (len > NGX_RTC_RING_RTP_MAX) {
        return;
    }

    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
    if (NULL == ccf || NULL == ccf->sh) {
        return;
    }
    shm = ccf->sh;

    /* Refresh the cached ready-subscriber snapshot only when the shm source's
     * subscriber set changed; the steady-state hot path reads src->snap_* in
     * process memory and never touches the slab pool mutex. */
    shm_src = (ngx_rtc_shm_source_t *) src->shm_src;
    if (NULL == shm_src
            || (ngx_uint_t) shm_src->subscribers_version != src->snap_version) {
        src->snap_count = ngx_rtc_shm_source_snapshot(shm, (u_char *) src->name,
                ngx_strlen(src->name), src->snap_ids, src->snap_slots,
                NGX_RTC_SOURCE_MAX_SNAPSHOT);
        if (NULL != shm_src) {
            src->snap_version = (ngx_uint_t) shm_src->subscribers_version;
        }
    }
    n = src->snap_count;
    if (0 == n) {
        return;
    }

    /* Same-worker fast path: send directly instead of bouncing through the shm
     * ring + eventfd. Cross-worker targets still go through the ring below. */
    for (i = 0; i < n; i++) {
        if (src->snap_slots[i] != (ngx_int_t) ngx_worker) {
            continue;
        }
        sess = ngx_rtc_session_find_by_id(src->snap_ids[i]);
        if (NULL == sess) {
            continue;
        }
        /* No per-session copy: same-worker NACK/PLI reads src->gop directly. */
        (void) ngx_rtc_session_send_rtp(sess, rtp, len, is_gop_start);
    }

    /* One entry per target worker; carry its ready session ids. */
    for (w = 0; w < shm->nworkers; w++) {
        if (w == (ngx_uint_t) ngx_worker) {
            continue; /* already sent directly above */
        }
        nsess = 0;
        for (i = 0; i < n; i++) {
            if (src->snap_slots[i] == (ngx_int_t) w
                    && nsess < NGX_RTC_RING_MAX_SESSIONS) {
                sess_ids[nsess++] = src->snap_ids[i];
            }
        }

        if (0 == nsess || NULL == shm->rings[w]) {
            continue;
        }

        /* The plaintext RTP is written into the shm slot once by enqueue; no
         * stack staging entry is built here. */
        if (NGX_OK == ngx_rtc_shm_ring_enqueue(shm->rings[w],
                (uint8_t) (is_video ? 0 : 1),
                (uint8_t) (0 != is_gop_start ? 1 : 0),
                rtp, (uint16_t) len, sess_ids, nsess)) {
            uint64_t one;
            ssize_t  rc;
            one = 1;
            if (-1 != shm->notify_fd[w]) {
                rc = write(shm->notify_fd[w], &one, sizeof(one));
                (void) rc;
            }
        } else {
            src->ring_drops++;
        }
    }
}


static int32_t
ngx_rtmp_rtc_emit(void *opaque, const uint8_t *rtp, uint32_t len)
{
    ngx_rtc_emit_ctx_t *ctx = opaque;
    ngx_rtc_source_t   *src = ctx->src;

    /* Cache the plaintext once in the GOP ring; late subscribers replay it.
     * Only video is cached: audio seq and video seq advance independently, so
     * mixing both into one ring breaks the seq-indexed NACK lookup, and audio
     * needs no fast-start replay (it is a continuous live stream). */
    if (0 != ctx->is_video) {
        ngx_rtc_rtp_ring_push(&src->gop, rtp, len, ctx->is_gop_start);

        /* Mirror every video packet into the per-source shm retransmit ring so
         * a cross-worker subscriber fast-starts and answers NACK/PLI from the
         * same cache (no waiting for the next natural keyframe). */
        {
            ngx_rtc_core_conf_t *ccf;

            ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
            if (NULL != ccf && NULL != ccf->sh) {
                ngx_rtc_shm_retransmit_append(ccf->sh,
                        (ngx_rtc_shm_source_t *) src->shm_src,
                        rtp, len, ctx->is_gop_start);
            }
        }
    }

    /* Accumulate sender-report statistics (payload octets, excl. RTP header). */
    if (0 != ctx->is_video) {
        src->video_pkts++;
        src->video_octets += (len >= NGX_RTC_RTP_HEADER_SIZE)
                             ? (len - NGX_RTC_RTP_HEADER_SIZE) : len;
    } else {
        src->audio_pkts++;
        src->audio_octets += (len >= NGX_RTC_RTP_HEADER_SIZE)
                             ? (len - NGX_RTC_RTP_HEADER_SIZE) : len;
    }

    /* Phase 2: broadcast through the per-worker shm media rings. The owner
     * worker dequeues and applies per-session SRTP + send. */
    ngx_rtc_broadcast_rtp(src, rtp, len, ctx->is_video, ctx->is_gop_start);

    return NGX_RTC_OK;
}
