/*
 * ngx_rtc_http_module.c - HTTP signaling: /rtc/v1/play/ offer/answer.
 *
 * Parses the client's SDP offer, creates (or reuses) the RTC source and a
 * player session, then returns an SDP answer with the server ICE/DTLS/SSRC
 * parameters. Matches the SRS /rtc/v1/play/ JSON contract so the existing
 * werift client keeps working.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <fcntl.h>
#include <unistd.h>

#include "ngx_rtc_rtp.h"
#include "ngx_rtc_sdp.h"
#include "ngx_rtc_dtls.h"
#include "ngx_rtc_core.h"
#include "ngx_rtc_shm.h"
#include "ngx_rtc_glue.h"

/* ngx_lua FFI shdict store, used to mirror stats into `lua_shared_dict
 * rtc_stats`. op=0 is a plain set; value_type=4 is SHDICT_TSTRING. */
extern int ngx_http_lua_ffi_shdict_store(ngx_shm_zone_t *zone, int op,
    u_char *key, size_t key_len, int value_type, u_char *str_value_buf,
    size_t str_value_len, double num_value, long exptime, int user_flags,
    char **errmsg, int *forcible);

#define NGX_RTC_SHDICT_TSTRING  4
#define NGX_RTC_STATS_MIRROR_MS 1000

/* ngx_lua wraps each lua_shared_dict: the zone registered in
 * cycle->shared_memory is only a wrapper whose ->data points to a
 * ngx_http_lua_shm_zone_ctx_t { log, lmcf, cycle, zone }.  The real shdict
 * zone (->data = the shared slab ctx, ->shm.addr = the mmap'd arena) is that
 * trailing `zone` member.  Offset: three leading pointers on LP64. */
#define NGX_RTC_LUA_SHM_CTX_ZONE_OFF 24


typedef struct
{
    ngx_str_t  candidate_ip;    /* server host candidate IP */
    ngx_int_t  candidate_port;  /* server host candidate UDP port */
} ngx_rtc_http_loc_conf_t;


/* Cursor over an ngx_chain_t body: reads one byte at a time across buf
 * boundaries so the 8KB stack copy of the request body is unnecessary. */
typedef struct
{
    ngx_chain_t *cl;
    u_char      *p;    /* current read position inside cl->buf */
} ngx_rtc_chain_reader_t;


static ngx_int_t  ngx_rtc_http_play_handler(ngx_http_request_t *r);
static void       ngx_rtc_http_body_handler(ngx_http_request_t *r);
static ngx_int_t  ngx_rtc_http_kick_handler(ngx_http_request_t *r);
static ngx_int_t  ngx_rtc_http_disconnect_handler(ngx_http_request_t *r);
static ngx_int_t  ngx_rtc_http_whip_handler(ngx_http_request_t *r);
static void       ngx_rtc_http_whip_body_handler(ngx_http_request_t *r);
static char      *ngx_rtc_http_play(ngx_conf_t *cf, ngx_command_t *cmd,
                     void *conf);
static char      *ngx_rtc_http_kick(ngx_conf_t *cf, ngx_command_t *cmd,
                     void *conf);
static char      *ngx_rtc_http_disconnect(ngx_conf_t *cf, ngx_command_t *cmd,
                     void *conf);
static char      *ngx_rtc_http_whip(ngx_conf_t *cf, ngx_command_t *cmd,
                     void *conf);
static void      *ngx_rtc_http_create_loc_conf(ngx_conf_t *cf);
static ngx_int_t  ngx_rtc_http_init_process(ngx_cycle_t *cycle);

static void       ngx_rtc_chain_reader_init(ngx_rtc_chain_reader_t *rd,
                     ngx_chain_t *cl);
static ngx_int_t  ngx_rtc_chain_reader_get(ngx_rtc_chain_reader_t *rd,
                     u_char *out);
static ngx_int_t  ngx_rtc_http_json_string(ngx_rtc_chain_reader_t *rd,
                     const char *key, char *out, size_t out_cap, size_t *out_len);
static u_char    *ngx_rtc_http_render_stats(u_char *p, u_char *end);
static void       ngx_rtc_stats_flush_send_failures(void);
static void       ngx_rtc_stats_mirror_timer(ngx_event_t *ev);
static ngx_shm_zone_t *ngx_rtc_http_find_shm_zone(ngx_cycle_t *cycle,
                     ngx_str_t *name);

/* Periodic stats -> lua_shared_dict mirror. */
static ngx_event_t      ngx_rtc_stats_mirror_timer_ev;
static ngx_shm_zone_t  *ngx_rtc_stats_shdict_zone;


static ngx_command_t ngx_rtc_http_commands[] = {

    { ngx_string("rtc_play"),
      NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
      ngx_rtc_http_play,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("rtc_candidate_ip"),
      NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_rtc_http_loc_conf_t, candidate_ip),
      NULL },

    { ngx_string("rtc_candidate_port"),
      NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_rtc_http_loc_conf_t, candidate_port),
      NULL },

    { ngx_string("rtc_kick"),
      NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
      ngx_rtc_http_kick,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("rtc_disconnect"),
      NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
      ngx_rtc_http_disconnect,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("rtc_whip"),
      NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
      ngx_rtc_http_whip,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t ngx_rtc_http_module_ctx = {
    NULL,                               /* preconfiguration */
    NULL,                               /* postconfiguration */
    NULL,                               /* create main configuration */
    NULL,                               /* init main configuration */
    NULL,                               /* create server configuration */
    NULL,                               /* merge server configuration */
    ngx_rtc_http_create_loc_conf,       /* create location configuration */
    NULL                                /* merge location configuration */
};


ngx_module_t ngx_rtc_http_module = {
    NGX_MODULE_V1,
    &ngx_rtc_http_module_ctx,           /* module context */
    ngx_rtc_http_commands,              /* module directives */
    NGX_HTTP_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    ngx_rtc_http_init_process,          /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_rtc_http_create_loc_conf(ngx_conf_t *cf)
{
    ngx_rtc_http_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_rtc_http_loc_conf_t));
    if (NULL == conf) {
        return NULL;
    }

    conf->candidate_ip.len = 0;
    conf->candidate_ip.data = NULL;
    conf->candidate_port = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_rtc_http_play(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_rtc_http_play_handler;

    return NGX_CONF_OK;
}


/*
 * GET /rtc/v1/stats -> live stream status (SRS /api/v1/streams equivalent).
 * Reads the C source registry directly (the data source Lua cannot reach) and
 * renders a compact JSON document. Best-effort: the response buffer is fixed,
 * so a very large registry is truncated rather than failed.
 */
static u_char *
ngx_rtc_http_render_stats(u_char *p, u_char *end)
{
    ngx_rtc_core_conf_t    *ccf;
    ngx_rtc_shm_ctx_t      *sh;
    ngx_rtc_shm_source_t   *shm_src;
    ngx_rtc_shm_session_t  *shm_sess;
    ngx_queue_t            *q;
    ngx_queue_t            *sq;
    ngx_uint_t              nsub;
    ngx_uint_t              cross;
    ngx_uint_t              first;
    ngx_uint_t              first_sess;
    ngx_uint_t              total_streams;
    ngx_uint_t              total_clients;
    ngx_uint_t              total_video_octets;
    ngx_uint_t              total_audio_octets;
    ngx_uint_t              total_send_failed;
    ngx_uint_t              total_send_eagain;

    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
    if (NULL == ccf || NULL == ccf->sh) {
        return ngx_slprintf(p, end, "{\"code\":0,\"streams\":[],"
                            "\"total_streams\":0,\"total_clients\":0}");
    }
    sh = ccf->sh;

    p = ngx_slprintf(p, end, "{\"code\":0,\"streams\":[");
    first = 1;
    total_streams = 0;
    total_clients = 0;
    total_video_octets = 0;
    total_audio_octets = 0;
    total_send_failed = 0;
    total_send_eagain = 0;

    ngx_shmtx_lock(&sh->pool->mutex);

    for (q = ngx_queue_head(&sh->source_list);
         q != ngx_queue_sentinel(&sh->source_list);
         q = ngx_queue_next(q)) {
        shm_src = ngx_queue_data(q, ngx_rtc_shm_source_t, queue);

        nsub = 0;
        cross = 0;
        for (sq = ngx_queue_head(&shm_src->subscribers);
             sq != ngx_queue_sentinel(&shm_src->subscribers);
             sq = ngx_queue_next(sq)) {
            shm_sess = ngx_queue_data(sq, ngx_rtc_shm_session_t, sub_queue);
            nsub++;
            if (shm_sess->owner_slot >= 0
                    && shm_sess->owner_slot != shm_src->publisher_slot) {
                cross = 1;
            }
        }

        p = ngx_slprintf(p, end,
                "%s{\"name\":\"%s\",\"publishing\":%ui,\"pub_worker\":%d,"
                "\"cross_worker\":%ui,\"clients\":%ui,"
                "\"send_failed\":%ui,\"send_eagain\":%ui,\"sessions\":[",
                first ? "" : ",",
                shm_src->name, (ngx_uint_t)shm_src->publishing,
                (int) shm_src->publisher_slot, cross, nsub,
                (ngx_uint_t)shm_src->send_failed,
                (ngx_uint_t)shm_src->send_eagain);

        first_sess = 1;
        for (sq = ngx_queue_head(&shm_src->subscribers);
             sq != ngx_queue_sentinel(&shm_src->subscribers);
             sq = ngx_queue_next(sq)) {
            shm_sess = ngx_queue_data(sq, ngx_rtc_shm_session_t, sub_queue);
            p = ngx_slprintf(p, end,
                    "%s{\"id\":%ui,\"ufrag\":\"%s\",\"twcc_lost\":%ui,"
                    "\"twcc_received\":%ui}",
                    first_sess ? "" : ",", (ngx_uint_t) shm_sess->id,
                    shm_sess->ice_ufrag, (ngx_uint_t) shm_sess->twcc_lost,
                    (ngx_uint_t) shm_sess->twcc_received);
            first_sess = 0;
        }

        p = ngx_slprintf(p, end,
                "],\"video\":{\"ssrc\":%ui,\"pt\":%ui,\"packets\":%ui,\"octets\":%ui},"
                "\"audio\":{\"ssrc\":%ui,\"pt\":%ui,\"packets\":%ui,\"octets\":%ui}}",
                (ngx_uint_t)shm_src->video_ssrc, (ngx_uint_t)shm_src->video_pt,
                (ngx_uint_t)shm_src->video_pkts, (ngx_uint_t)shm_src->video_octets,
                (ngx_uint_t)shm_src->audio_ssrc, (ngx_uint_t)shm_src->audio_pt,
                (ngx_uint_t)shm_src->audio_pkts, (ngx_uint_t)shm_src->audio_octets);
        first = 0;

        total_streams++;
        total_clients += nsub;
        total_video_octets += shm_src->video_octets;
        total_audio_octets += shm_src->audio_octets;
        total_send_failed += shm_src->send_failed;
        total_send_eagain += shm_src->send_eagain;

        if (p >= end - 64) {
            break;
        }
    }

    ngx_shmtx_unlock(&sh->pool->mutex);

    return ngx_slprintf(p, end, "],\"total_streams\":%ui,\"total_clients\":%ui,"
                        "\"total_video_octets\":%ui,\"total_audio_octets\":%ui,"
                        "\"total_send_failed\":%ui,\"total_send_eagain\":%ui}",
                        total_streams, total_clients,
                        total_video_octets, total_audio_octets,
                        total_send_failed, total_send_eagain);
}


/*
 * Move this process's accumulated send-drop counters onto the matching shm
 * source (owned counters are only touched by the session's own worker, so a
 * per-worker flush never double-counts). Runs right before each mirror tick so
 * the rendered snapshot and the lua_shared_dict stay within one second.
 */
static void
ngx_rtc_stats_flush_send_failures(void)
{
    ngx_rtc_core_conf_t *ccf;
    ngx_rtc_session_t   *sess;

    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
    if (NULL == ccf || NULL == ccf->sh) {
        return;
    }

    for (sess = ngx_rtc_session_first(); NULL != sess;
         sess = ngx_rtc_session_next(sess)) {
        if (0 == sess->send_failed && 0 == sess->send_eagain) {
            continue;
        }
        if (NULL != sess->source) {
            ngx_rtc_shm_source_add_send_stats(ccf->sh,
                (u_char *) sess->source->name,
                ngx_strlen(sess->source->name),
                sess->send_failed, sess->send_eagain);
        }
        sess->send_failed = 0;
        sess->send_eagain = 0;
    }
}


static ngx_shm_zone_t *
ngx_rtc_http_find_shm_zone(ngx_cycle_t *cycle, ngx_str_t *name)
{
    ngx_uint_t       i;
    ngx_shm_zone_t  *shm_zone;
    ngx_list_part_t *part;

    part = &cycle->shared_memory.part;
    shm_zone = part->elts;

    for (i = 0; /* void */ ; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            shm_zone = part->elts;
            i = 0;
        }

        if (name->len == shm_zone[i].shm.name.len
                && 0 == ngx_strncmp(name->data, shm_zone[i].shm.name.data,
                                    name->len)) {
            return &shm_zone[i];
        }
    }

    return NULL;
}


static void
ngx_rtc_stats_mirror_timer(ngx_event_t *ev)
{
    u_char   buf[16384];
    u_char  *p;
    u_char  *end;
    size_t   len;
    char    *errmsg;
    int      forcible;
    ngx_str_t key = ngx_string("stats");

    end = buf + sizeof(buf);
    ngx_rtc_stats_flush_send_failures();
    p = ngx_rtc_http_render_stats(buf, end);
    len = (size_t)(p - buf);

    if (NULL != ngx_rtc_stats_shdict_zone) {
        if (ngx_http_lua_ffi_shdict_store(ngx_rtc_stats_shdict_zone, 0,
                key.data, key.len, NGX_RTC_SHDICT_TSTRING, buf, len,
                0, 0, 0, &errmsg, &forcible) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                          "ngx_rtc: shdict store failed: %s",
                          (NULL != errmsg) ? errmsg : "unknown");
        }
    }

    ngx_add_timer(ev, NGX_RTC_STATS_MIRROR_MS);
}


static ngx_int_t
ngx_rtc_http_init_process(ngx_cycle_t *cycle)
{
    ngx_str_t  name = ngx_string("rtc_stats");

    if (ngx_process != NGX_PROCESS_WORKER) {
        return NGX_OK;
    }

    /* Idempotent; the stream module may already have done this. */
    if (ngx_rtc_dtls_global_init() != 0) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "ngx_rtc_http: DTLS init failed");
        return NGX_ERROR;
    }

    /* Mirror the shm stats into lua_shared_dict rtc_stats periodically.
     * The zone registered in cycle->shared_memory for a lua_shared_dict is
     * only a wrapper whose ->data is a ngx_http_lua_shm_zone_ctx_t; hand
     * ngx_lua's real shdict zone to ffi_shdict_store instead (it reads
     * zone->data as the shared slab ctx and would crash on the wrapper). */
    ngx_rtc_stats_shdict_zone = ngx_rtc_http_find_shm_zone(cycle, &name);
    if (NULL != ngx_rtc_stats_shdict_zone) {
        ngx_shm_zone_t *real = (ngx_shm_zone_t *)
            ((u_char *) ngx_rtc_stats_shdict_zone->data
             + NGX_RTC_LUA_SHM_CTX_ZONE_OFF);
        if (real->shm.addr != ngx_rtc_stats_shdict_zone->shm.addr) {
            /* unwrap offset drifted from the running ngx_lua layout: disable
             * the mirror rather than risk a crash in the timer handler. */
            ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                          "ngx_rtc_http: rtc_stats shdict unwrap mismatch "
                          "(real %p shell %p); stats mirror disabled",
                          real->shm.addr, ngx_rtc_stats_shdict_zone->shm.addr);
            ngx_rtc_stats_shdict_zone = NULL;
        } else {
            ngx_rtc_stats_shdict_zone = real;
        }
    }
    if (NULL != ngx_rtc_stats_shdict_zone) {
        ngx_memzero(&ngx_rtc_stats_mirror_timer_ev,
                    sizeof(ngx_rtc_stats_mirror_timer_ev));
        ngx_rtc_stats_mirror_timer_ev.handler = ngx_rtc_stats_mirror_timer;
        ngx_rtc_stats_mirror_timer_ev.log = cycle->log;
        ngx_rtc_stats_mirror_timer_ev.data = NULL;
        ngx_add_timer(&ngx_rtc_stats_mirror_timer_ev, NGX_RTC_STATS_MIRROR_MS);
    }

    /* The RTCP sender-report / A/V-skew timer lives in the rtmp bridge module,
     * but nginx never runs an NGX_RTMP_MODULE's init_process - so start it
     * here, from an init_process that nginx does run. */
    ngx_rtmp_rtc_rtcp_timer_start(cycle);

    return NGX_OK;
}


static ngx_int_t
ngx_rtc_http_play_handler(ngx_http_request_t *r)
{
    ngx_int_t rc;

    if (r->method != NGX_HTTP_POST) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_read_client_request_body(r, ngx_rtc_http_body_handler);
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "rtc_play: read_client_body rc=%d", (int)rc);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    return NGX_DONE;
}


static void
ngx_rtc_http_body_handler(ngx_http_request_t *r)
{
    ngx_rtc_chain_reader_t rd;
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "rtc_play: body_handler called, request_body=%p bufs=%p",
                  r->request_body,
                  (r->request_body ? r->request_body->bufs : NULL));
    char                sdp[8192];
    size_t              sdp_len;
    char                streamurl[256];
    size_t              su_len;
    char               *app;
    char               *stream;
    char                name[160];
    ngx_rtc_sdp_offer_t offer;
    ngx_rtc_sdp_answer_t cfg;
    ngx_rtc_source_t   *src;
    ngx_rtc_session_t  *sess;
    char                ans_buf[4096];
    uint32_t            ans_len;
    ngx_str_t           resp;
    u_char              resp_buf[8192];
    size_t              resp_len;
    ngx_int_t           i;
    ngx_rtc_http_loc_conf_t *rcf;
    char                candidate_ip[NGX_RTC_SDP_STR_LEN];
    char                video_fmtp_buf[128];

    if (NULL == r->request_body || NULL == r->request_body->bufs) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "rtc_play: empty request body");
        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return;
    }

    /* Parse straight from the request-body chain: no 8KB stack copy. The
     * reader walks across buf boundaries, so an SDP offer split between two
     * bufs is still extracted whole. Re-init per field because JSON field
     * order is not guaranteed. */
    sdp_len = 0;
    ngx_rtc_chain_reader_init(&rd, r->request_body->bufs);
    if (ngx_rtc_http_json_string(&rd, "sdp",
            sdp, sizeof(sdp), &sdp_len) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "rtc_play: sdp field not found in body");
        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return;
    }

    su_len = 0;
    ngx_rtc_chain_reader_init(&rd, r->request_body->bufs);
    if (ngx_rtc_http_json_string(&rd, "streamurl",
            streamurl, sizeof(streamurl), &su_len) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "rtc_play: streamurl field not found in body");
        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return;
    }

    /* streamurl: webrtc://host/app/stream -> "app/stream". */
    app = strstr(streamurl, "://");
    if (NULL == app) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "rtc_play: streamurl has no :// -> '%s'", streamurl);
        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return;
    }
    app += 3;
    app = strchr(app, '/');
    if (NULL == app) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "rtc_play: streamurl has no app sep -> '%s'", streamurl);
        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return;
    }
    app += 1;
    stream = strchr(app, '/');
    if (NULL == stream) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "rtc_play: streamurl has no stream sep -> '%s'", streamurl);
        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return;
    }
    *stream = '\0';
    stream += 1;

    snprintf(name, sizeof(name), "%s/%s", app, stream);

    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                  "rtc_play: streamurl='%s' name='%s'", streamurl, name);

    /* Parse the client offer. */
    if (ngx_rtc_sdp_parse_offer(sdp, sdp_len, &offer) != NGX_RTC_OK) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "rtc_play: sdp parse failed, sdp_len=%uz", sdp_len);
        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return;
    }

    src = ngx_rtc_source_get(name);
    if (NULL == src) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    /* Phase 1: SSRC/PT are authoritative in shm, shared with the publisher
     * worker. Read them from shm first; allocate on first use so a publisher
     * that arrived before any viewer and a later play agree on the same SSRC.
     * PT always follows this offer. */
    {
        ngx_rtc_core_conf_t  *ccf;
        ngx_rtc_shm_source_t *shm_src;

        ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
        if (NULL != ccf && NULL != ccf->sh) {
            shm_src = ngx_rtc_shm_source_get(ccf->sh, (u_char *) name,
                                             strlen(name));
            if (NULL != shm_src) {
                src->video_ssrc = shm_src->video_ssrc;
                src->audio_ssrc = shm_src->audio_ssrc;
                src->video_pt = (0 != offer.video_pt) ? offer.video_pt
                                                      : shm_src->video_pt;
                src->audio_pt = (0 != offer.audio_pt) ? offer.audio_pt
                                                      : shm_src->audio_pt;
                ngx_rtc_shm_source_set_pt(ccf->sh, (u_char *) name,
                                          strlen(name), offer.video_pt,
                                          offer.audio_pt);
            }
        }
    }

    /* Session outlives the HTTP request (it is bound to the UDP DTLS/SRTP
     * connection), so allocate from process memory, not the request pool. */
    sess = ngx_alloc(sizeof(ngx_rtc_session_t), r->connection->log);
    if (NULL == sess) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }
    ngx_memzero(sess, sizeof(*sess));

    /* Self-link the nginx queue nodes so ngx_queue_remove() is a safe no-op
     * when a session that never subscribed (or was never added) is closed. */
    ngx_queue_init(&sess->queue);
    ngx_queue_init(&sess->sub_queue);

    /* Initialize the session lifecycle state machine (NEW). */
    ngx_rtc_session_fsm_init(&sess->fsm, sess->fsm_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, sess);
    sess->last_active = ngx_current_msec;

    sess->source = src;

    /* Per-session payload types taken from this session's own offer (RFC 3264:
     * the answer PT must be a subset of the offer). Fall back to the source PT
     * when the codec is absent from the offer. The plaintext stream is sent
     * with these PTs (rewritten in ngx_rtc_session_send_rtp), so viewers with
     * heterogeneous PT assignments all negotiate successfully. */
    sess->video_pt = (0 != offer.video_pt) ? offer.video_pt : src->video_pt;
    sess->audio_pt = (0 != offer.audio_pt) ? offer.audio_pt : src->audio_pt;
    sess->twcc_video_ext = offer.video_twcc_ext; /* 0 = not offered */
    sess->twcc_audio_ext = offer.audio_twcc_ext;

    snprintf(sess->ice_ufrag, sizeof(sess->ice_ufrag), "%08xu",
            (unsigned)ngx_random());
    snprintf(sess->ice_pwd, sizeof(sess->ice_pwd), "%08x%08x%08x",
            (unsigned)ngx_random(), (unsigned)ngx_random(),
            (unsigned)ngx_random());
    ngx_rtc_session_add(sess);

    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                  "ngx_rtc_http: new session stream=%s ufrag=%s",
                  name, sess->ice_ufrag);

    /* Phase 1: add the session skeleton to the cross-worker shm registry so a
     * UDP worker on a different process can find it by ICE ufrag. */
    {
        ngx_rtc_core_conf_t   *ccf;
        ngx_rtc_shm_session_t *shm_sess;

        ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
        if (NULL != ccf && NULL != ccf->sh) {
            shm_sess = ngx_rtc_shm_session_add(ccf->sh,
                    (u_char *)sess->ice_ufrag, strlen(sess->ice_ufrag),
                    (u_char *)sess->ice_pwd, strlen(sess->ice_pwd),
                    (u_char *)name, strlen(name),
                    sess->video_pt, sess->audio_pt,
                    sess->twcc_video_ext, sess->twcc_audio_ext,
                    sess->publishing);
            if (NULL == shm_sess) {
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                              "ngx_rtc_http: shm session add failed "
                              "ufrag=%s", sess->ice_ufrag);
            } else {
                sess->id = shm_sess->id;
            }
        }
    }

    /* Build the SDP answer. */
    ngx_rtc_sdp_answer_init(&cfg);
    cfg.ice_ufrag = sess->ice_ufrag;
    cfg.ice_pwd = sess->ice_pwd;
    cfg.fingerprint_algo = "sha-256";
    cfg.fingerprint = ngx_rtc_dtls_fingerprint();
    cfg.setup = "passive";

    /* Candidate comes from the location config; copy into a NUL-terminated
     * buffer because the SDP writer prints it with %s. */
    rcf = ngx_http_get_module_loc_conf(r, ngx_rtc_http_module);
    if ((0 == rcf->candidate_ip.len) || (NULL == rcf->candidate_ip.data)
            || (rcf->candidate_ip.len >= sizeof(candidate_ip))) {
        cfg.candidate_ip = "127.0.0.1";
    } else {
        ngx_memcpy(candidate_ip, rcf->candidate_ip.data, rcf->candidate_ip.len);
        candidate_ip[rcf->candidate_ip.len] = '\0';
        cfg.candidate_ip = candidate_ip;
    }
    cfg.candidate_port = (NGX_CONF_UNSET == rcf->candidate_port)
                         ? 8000u : (uint32_t)rcf->candidate_port;
    cfg.video_ssrc = src->video_ssrc;
    cfg.audio_ssrc = src->audio_ssrc;
    cfg.video_pt = sess->video_pt;
    cfg.audio_pt = sess->audio_pt;
    cfg.video_twcc_ext = sess->twcc_video_ext;
    cfg.audio_twcc_ext = sess->twcc_audio_ext;
    cfg.video_twcc_uri = offer.video_twcc_uri; /* echo the exact URI offered */
    cfg.audio_twcc_uri = offer.audio_twcc_uri;

    /* Mirror the offer's m-line order and mids into the answer (RFC 3264
     * rejects an answer whose m-line order differs from the offer). */
    cfg.n_media = offer.n_media;
    for (i = 0; i < (ngx_int_t)offer.n_media; i++)
    {
        ngx_memcpy(cfg.media_type[i], offer.media_type[i], sizeof(cfg.media_type[i]));
        ngx_memcpy(cfg.media_mid[i], offer.media_mid[i], sizeof(cfg.media_mid[i]));
    }

    /* Echo the real stream profile-level-id from the SPS instead of the
     * hardcoded default: a mismatch makes Chrome's hardware decoder render
     * garbage (green/blocky image). ngx_snprintf does not zero-pad %x, so the
     * 6 hex chars are written by hand. */
    if (src->sps_profile_level_id_valid)
    {
        static const char hex_digits[] = "0123456789abcdef";
        u_char  *dst = (u_char *)video_fmtp_buf;
        const char prefix[] =
            "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=";
        ngx_uint_t k;

        ngx_memcpy(dst, prefix, sizeof(prefix) - 1);
        dst += sizeof(prefix) - 1;
        for (k = 0; k < 3; k++)
        {
            dst[k * 2]     = (u_char)hex_digits[src->sps_profile_level_id[k] >> 4];
            dst[k * 2 + 1] = (u_char)hex_digits[src->sps_profile_level_id[k] & 0x0f];
        }
        dst[6] = '\0';
        cfg.video_fmtp = video_fmtp_buf;
    }

    if (ngx_rtc_sdp_generate_answer(&cfg, ans_buf, sizeof(ans_buf), &ans_len)
            != NGX_RTC_OK) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    /* {"code":0,"sdp":"<escaped sdp>"} */
    resp_len = 0;
    resp_buf[resp_len++] = '{';
    memcpy(resp_buf + resp_len, "\"code\":0,\"sdp\":\"", 16);
    resp_len += 16;
    for (i = 0; i < (ngx_int_t)ans_len; i++) {
        if (resp_len + 2 >= sizeof(resp_buf)) {
            break;
        }
        if ('\n' == ans_buf[i]) {
            resp_buf[resp_len++] = '\\';
            resp_buf[resp_len++] = 'n';
        } else if ('\r' == ans_buf[i]) {
            resp_buf[resp_len++] = '\\';
            resp_buf[resp_len++] = 'r';
        } else if ('"' == ans_buf[i]) {
            resp_buf[resp_len++] = '\\';
            resp_buf[resp_len++] = '"';
        } else {
            resp_buf[resp_len++] = (u_char)ans_buf[i];
        }
    }
    memcpy(resp_buf + resp_len, "\"}", 2);
    resp_len += 2;

    resp.data = resp_buf;
    resp.len = resp_len;

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_type.len = sizeof("application/json") - 1;
    r->headers_out.content_type.data = (u_char *)"application/json";
    r->headers_out.content_length_n = (off_t)resp.len;

    ngx_http_send_header(r);

    {
        ngx_buf_t  *outb;
        ngx_chain_t out;

        outb = ngx_create_temp_buf(r->pool, resp.len);
        if (NULL == outb) {
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
            return;
        }
        outb->last = ngx_cpymem(outb->last, resp.data, resp.len);
        outb->last_buf = 1;

        out.buf = outb;
        out.next = NULL;
        ngx_http_output_filter(r, &out);
    }

    ngx_http_finalize_request(r, NGX_OK);
}


static void
ngx_rtc_chain_reader_init(ngx_rtc_chain_reader_t *rd, ngx_chain_t *cl)
{
    rd->cl = cl;
    rd->p = NULL;
    if (NULL != cl && NULL != cl->buf) {
        rd->p = cl->buf->pos;
    }
}


/* Read one byte across the chain; NGX_ERROR at chain end. */
static ngx_int_t
ngx_rtc_chain_reader_get(ngx_rtc_chain_reader_t *rd, u_char *out)
{
    for (;;) {
        if (NULL == rd->cl || NULL == rd->cl->buf) {
            return NGX_ERROR;
        }
        if (NULL == rd->p) {
            rd->p = rd->cl->buf->pos;
        }
        if (rd->p < rd->cl->buf->last) {
            *out = *rd->p;
            rd->p++;
            return NGX_OK;
        }
        rd->cl = rd->cl->next;
        rd->p = (NULL != rd->cl && NULL != rd->cl->buf) ? rd->cl->buf->pos
                                                        : NULL;
    }
}


/*
 * Extract a JSON string field ("key":"value") into out. Unescapes \" \\ \n \r
 * \t. Returns NGX_OK / NGX_ERROR. out is NUL-terminated; *out_len excludes the
 * NUL. The reader is advanced past the matched value; a failed key match only
 * consumes the opening quote so overlapping matches are not skipped.
 */
static ngx_int_t
ngx_rtc_http_json_string(ngx_rtc_chain_reader_t *rd, const char *key,
        char *out, size_t out_cap, size_t *out_len)
{
    size_t                 key_len;
    size_t                 k;
    size_t                 olen;
    ngx_rtc_chain_reader_t mark;
    u_char                 ch;

    key_len = strlen(key);

    for (;;) {
        /* Skip to the next quote that may open the field. */
        for (;;) {
            if (ngx_rtc_chain_reader_get(rd, &ch) != NGX_OK) {
                return NGX_ERROR;
            }
            if ('"' == ch) {
                break;
            }
        }

        /* Snapshot just past the quote; restore it on a failed match so a
         * later overlapping match is never skipped. */
        mark = *rd;

        for (k = 0; k < key_len; k++) {
            if (ngx_rtc_chain_reader_get(rd, &ch) != NGX_OK
                    || ch != (u_char)key[k]) {
                *rd = mark;
                break;
            }
        }
        if (k != key_len) {
            continue;
        }

        if (ngx_rtc_chain_reader_get(rd, &ch) != NGX_OK || '"' != ch) {
            *rd = mark;
            continue;
        }

        /* Field name matched; the next non-space byte must be ':'. */
        for (;;) {
            if (ngx_rtc_chain_reader_get(rd, &ch) != NGX_OK) {
                return NGX_ERROR;
            }
            if (' ' != ch && '\t' != ch) {
                break;
            }
        }
        if (':' != ch) {
            return NGX_ERROR;
        }

        for (;;) {
            if (ngx_rtc_chain_reader_get(rd, &ch) != NGX_OK) {
                return NGX_ERROR;
            }
            if (' ' != ch && '\t' != ch) {
                break;
            }
        }
        if ('"' != ch) {
            return NGX_ERROR;
        }

        /* Read and unescape the value string. */
        olen = 0;
        for (;;) {
            if (ngx_rtc_chain_reader_get(rd, &ch) != NGX_OK || '"' == ch) {
                break;
            }
            if (olen + 1 >= out_cap) {
                break;
            }
            if ('\\' == ch) {
                if (ngx_rtc_chain_reader_get(rd, &ch) != NGX_OK) {
                    out[olen++] = '\\';
                    break;
                }
                if ('n' == ch) {
                    out[olen++] = '\n';
                } else if ('r' == ch) {
                    out[olen++] = '\r';
                } else if ('t' == ch) {
                    out[olen++] = '\t';
                } else {
                    out[olen++] = (char)ch;
                }
            } else {
                out[olen++] = (char)ch;
            }
        }
        out[olen] = '\0';
        *out_len = olen;
        return NGX_OK;
    }
}


static char *
ngx_rtc_http_kick(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_rtc_http_kick_handler;

    return NGX_CONF_OK;
}


/*
 * Internal kick endpoint invoked by the Lua admin control plane via
 * ngx.location.capture. It flips the skeleton's close_requested flag and the
 * owning worker reaps it on the next timer tick. Only the id argument is used.
 */
static ngx_int_t
ngx_rtc_http_kick_handler(ngx_http_request_t *r)
{
    ngx_rtc_core_conf_t *ccf;
    ngx_str_t            val;
    ngx_int_t            id;

    if (r->method != NGX_HTTP_POST) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    if (ngx_http_arg(r, (u_char *) "id", sizeof("id") - 1, &val) != NGX_OK) {
        return NGX_HTTP_BAD_REQUEST;
    }

    id = ngx_atoi(val.data, val.len);
    if (NGX_ERROR == id || id <= 0) {
        return NGX_HTTP_BAD_REQUEST;
    }

    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
    if (NULL == ccf || NULL == ccf->sh) {
        return NGX_HTTP_SERVICE_UNAVAILABLE;
    }

    if (ngx_rtc_shm_session_request_close(ccf->sh, (ngx_uint_t) id)
            != NGX_OK) {
        return NGX_HTTP_NOT_FOUND;
    }

    return NGX_OK;
}


static char *
ngx_rtc_http_disconnect(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_rtc_http_disconnect_handler;

    return NGX_CONF_OK;
}


/*
 * Internal disconnect endpoint: marks every viewer of one source for close by
 * its "app/stream" name. The owner workers reap them on the next timer tick.
 */
static ngx_int_t
ngx_rtc_http_disconnect_handler(ngx_http_request_t *r)
{
    ngx_rtc_core_conf_t *ccf;
    ngx_str_t            val;

    if (r->method != NGX_HTTP_POST) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    if (ngx_http_arg(r, (u_char *) "name", sizeof("name") - 1, &val)
            != NGX_OK) {
        return NGX_HTTP_BAD_REQUEST;
    }

    if (0 == val.len) {
        return NGX_HTTP_BAD_REQUEST;
    }

    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
    if (NULL == ccf || NULL == ccf->sh) {
        return NGX_HTTP_SERVICE_UNAVAILABLE;
    }

    if (ngx_rtc_shm_source_request_close(ccf->sh, val.data, val.len)
            != NGX_OK) {
        return NGX_HTTP_NOT_FOUND;
    }

    return NGX_OK;
}


static char *
ngx_rtc_http_whip(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_rtc_http_whip_handler;

    return NGX_CONF_OK;
}


/*
 * RFC 8224 WHIP endpoint: POST /whip/endpoint?app=live&stream=livestream with
 * the body being a plain SDP offer. Creates (or reuses) the source, allocates a
 * publisher session and answers with a recvonly SDP. Media reception is wired
 * by the stream module once the session reaches SRTP_READY.
 */
static ngx_int_t
ngx_rtc_http_whip_handler(ngx_http_request_t *r)
{
    ngx_int_t rc;

    if (r->method != NGX_HTTP_POST) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_read_client_request_body(r, ngx_rtc_http_whip_body_handler);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    return NGX_DONE;
}


static void
ngx_rtc_http_whip_body_handler(ngx_http_request_t *r)
{
    ngx_rtc_core_conf_t  *ccf;
    ngx_rtc_shm_source_t *shm_src;
    ngx_rtc_sdp_offer_t   offer;
    ngx_rtc_sdp_answer_t  cfg;
    ngx_rtc_source_t     *src;
    ngx_rtc_session_t    *sess;
    ngx_rtc_http_loc_conf_t *rcf;
    ngx_str_t             app;
    ngx_str_t             stream;
    u_char               *p;
    size_t                sdp_len;
    size_t                off;
    ngx_chain_t          *cl;
    char                  name[NGX_RTC_SOURCE_NAME_MAX];
    char                  ans_buf[4096];
    uint32_t              ans_len;
    char                  candidate_ip[NGX_RTC_SDP_STR_LEN];

    if (NULL == r->request_body || NULL == r->request_body->bufs) {
        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return;
    }

    if (ngx_http_arg(r, (u_char *) "app", sizeof("app") - 1, &app) != NGX_OK
            || ngx_http_arg(r, (u_char *) "stream",
                            sizeof("stream") - 1, &stream) != NGX_OK
            || 0 == app.len || 0 == stream.len
            || app.len + stream.len + 1 >= sizeof(name)) {
        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return;
    }

    /* Copy the raw SDP offer out of the request-body chain (it may span
     * several chunk buffers). */
    sdp_len = 0;
    for (cl = r->request_body->bufs; NULL != cl; cl = cl->next) {
        if (NULL != cl->buf) {
            sdp_len += (size_t)(cl->buf->last - cl->buf->pos);
        }
    }

    p = ngx_pnalloc(r->pool, sdp_len + 1);
    if (NULL == p) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }
    off = 0;
    for (cl = r->request_body->bufs; NULL != cl; cl = cl->next) {
        if (NULL != cl->buf) {
            ngx_memcpy(p + off, cl->buf->pos, cl->buf->last - cl->buf->pos);
            off += (size_t)(cl->buf->last - cl->buf->pos);
        }
    }
    p[sdp_len] = '\0';

    ngx_memcpy(name, app.data, app.len);
    name[app.len] = '/';
    ngx_memcpy(name + app.len + 1, stream.data, stream.len);
    name[app.len + 1 + stream.len] = '\0';

    if (ngx_rtc_sdp_parse_offer((char *) p, (uint32_t) sdp_len, &offer)
            != NGX_RTC_OK) {
        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return;
    }

    src = ngx_rtc_source_get(name);
    if (NULL == src) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }
    src->publishing = 1;

    /* SSRC/PT authority lives in shm, shared with any playback worker. */
    ccf = ngx_rtc_core_get_conf((ngx_cycle_t *) ngx_cycle);
    if (NULL != ccf && NULL != ccf->sh) {
        shm_src = ngx_rtc_shm_source_get(ccf->sh, (u_char *) name,
                                         strlen(name));
        if (NULL != shm_src) {
            ngx_rtc_shm_source_set_publishing(ccf->sh, (u_char *) name,
                                              strlen(name), 1);
            src->video_ssrc = shm_src->video_ssrc;
            src->audio_ssrc = shm_src->audio_ssrc;
            src->video_pt = (0 != offer.video_pt) ? offer.video_pt
                                                  : shm_src->video_pt;
            src->audio_pt = (0 != offer.audio_pt) ? offer.audio_pt
                                                  : shm_src->audio_pt;
            ngx_rtc_shm_source_set_pt(ccf->sh, (u_char *) name, strlen(name),
                                      offer.video_pt, offer.audio_pt);
        }
    }

    sess = ngx_alloc(sizeof(ngx_rtc_session_t), r->connection->log);
    if (NULL == sess) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }
    ngx_memzero(sess, sizeof(*sess));
    ngx_queue_init(&sess->queue);
    ngx_queue_init(&sess->sub_queue);
    ngx_rtc_session_fsm_init(&sess->fsm, sess->fsm_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, sess);
    sess->last_active = ngx_current_msec;
    sess->source = src;
    sess->publishing = 1; /* WHIP: this session sends media, does not receive */
    sess->video_pt = (0 != offer.video_pt) ? offer.video_pt : src->video_pt;
    sess->audio_pt = (0 != offer.audio_pt) ? offer.audio_pt : src->audio_pt;
    sess->twcc_video_ext = offer.video_twcc_ext;
    sess->twcc_audio_ext = offer.audio_twcc_ext;
    snprintf(sess->ice_ufrag, sizeof(sess->ice_ufrag), "%08xu",
             (unsigned) ngx_random());
    snprintf(sess->ice_pwd, sizeof(sess->ice_pwd), "%08x%08x%08x",
             (unsigned) ngx_random(), (unsigned) ngx_random(),
             (unsigned) ngx_random());
    ngx_rtc_session_add(sess);

    if (NULL != ccf && NULL != ccf->sh) {
        shm_src = ngx_rtc_shm_source_get(ccf->sh, (u_char *) name,
                                         strlen(name));
        if (NULL != shm_src) {
            ngx_rtc_shm_session_t *shm_sess;

            shm_sess = ngx_rtc_shm_session_add(ccf->sh,
                    (u_char *) sess->ice_ufrag, strlen(sess->ice_ufrag),
                    (u_char *) sess->ice_pwd, strlen(sess->ice_pwd),
                    (u_char *) name, strlen(name),
                    sess->video_pt, sess->audio_pt,
                    sess->twcc_video_ext, sess->twcc_audio_ext,
                    sess->publishing);
            if (NULL != shm_sess) {
                sess->id = shm_sess->id;
            }
        }
    }

    ngx_rtc_sdp_answer_init(&cfg);
    cfg.ice_ufrag = sess->ice_ufrag;
    cfg.ice_pwd = sess->ice_pwd;
    cfg.fingerprint_algo = "sha-256";
    cfg.fingerprint = ngx_rtc_dtls_fingerprint();
    cfg.setup = "passive";
    cfg.sendonly = -1; /* WHIP: server receives the publisher's media */

    rcf = ngx_http_get_module_loc_conf(r, ngx_rtc_http_module);
    if (0 == rcf->candidate_ip.len || NULL == rcf->candidate_ip.data
            || rcf->candidate_ip.len >= sizeof(candidate_ip)) {
        cfg.candidate_ip = "127.0.0.1";
    } else {
        ngx_memcpy(candidate_ip, rcf->candidate_ip.data, rcf->candidate_ip.len);
        candidate_ip[rcf->candidate_ip.len] = '\0';
        cfg.candidate_ip = candidate_ip;
    }
    cfg.candidate_port = (NGX_CONF_UNSET == rcf->candidate_port)
                         ? 8000u : (uint32_t) rcf->candidate_port;
    cfg.video_ssrc = src->video_ssrc;
    cfg.audio_ssrc = src->audio_ssrc;
    cfg.video_pt = sess->video_pt;
    cfg.audio_pt = sess->audio_pt;
    cfg.n_media = offer.n_media;
    ngx_memcpy(cfg.media_type, offer.media_type, sizeof(cfg.media_type));
    ngx_memcpy(cfg.media_mid, offer.media_mid, sizeof(cfg.media_mid));
    cfg.video_twcc_ext = offer.video_twcc_ext;
    cfg.audio_twcc_ext = offer.audio_twcc_ext;
    cfg.video_twcc_uri = offer.video_twcc_uri;
    cfg.audio_twcc_uri = offer.audio_twcc_uri;

    if (ngx_rtc_sdp_generate_answer(&cfg, ans_buf, sizeof(ans_buf), &ans_len)
            != NGX_RTC_OK) {
        ngx_rtc_session_remove(sess);
        ngx_free(sess);
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                  "ngx_rtc_http: whip publish stream=%s ufrag=%s",
                  name, sess->ice_ufrag);

    r->headers_out.status = NGX_HTTP_CREATED;
    r->headers_out.content_type.len = sizeof("application/sdp") - 1;
    r->headers_out.content_type.data = (u_char *) "application/sdp";
    r->headers_out.content_length_n = (off_t) ans_len;

    ngx_http_send_header(r);

    {
        ngx_buf_t  *outb;
        ngx_chain_t out;

        outb = ngx_create_temp_buf(r->pool, ans_len);
        if (NULL == outb) {
            ngx_rtc_session_remove(sess);
            ngx_free(sess);
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
            return;
        }
        outb->last = ngx_cpymem(outb->last, ans_buf, ans_len);
        outb->last_buf = 1;
        out.buf = outb;
        out.next = NULL;
        ngx_http_output_filter(r, &out);
    }

    ngx_http_finalize_request(r, NGX_OK);
}
