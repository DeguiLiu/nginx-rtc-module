/**
 * @file    ngx_rtc_sdp.c
 * @brief   Minimal WebRTC SDP offer parser and answer generator.
 * @version 0.5.0
 * @license MIT, see LICENSE
 *
 * Pure C11 translation of SRS 6.0:
 *   src/app/srs_app_rtc_sdp.cpp  (SrsSessionInfo::parse_attribute/encode,
 *                                 SrsSdp::parse, SrsSdp::encode,
 *                                 SrsMediaDesc::parse_attr_rtpmap,
 *                                 SrsMediaDesc::parse_attr_ssrc,
 *                                 srs_parse_h264_fmtp)
 *   src/app/srs_app_rtc_conn.cpp (generate_publish_local_sdp_for_audio/video)
 */

#include "ngx_rtc_sdp.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Number of payload types per media description. */
#define NGX_RTC_SDP_MAX_PT    32u

/* Bounded writer for the generated answer. */
typedef struct
{
    char    *data;
    uint32_t cap;
    uint32_t len;
} ngx_rtc_sdp_writer_t;

typedef struct
{
    uint8_t  pt;
    char     encoding[NGX_RTC_SDP_STR_LEN];
    uint32_t clock_rate;
    uint32_t channels;
    char     fmtp[NGX_RTC_SDP_FMTP_LEN];
} ngx_rtc_sdp_pt_t;

typedef struct
{
    char     type[NGX_RTC_SDP_STR_LEN];
    char     mid[NGX_RTC_SDP_MID_LEN];
    uint32_t ssrc;
    uint32_t n_pt;
    uint8_t  twcc_ext;   /* transport-wide-cc ext id, 0 = not negotiated */
    char     twcc_uri[NGX_RTC_SDP_TWCC_URI_LEN]; /* full ext URI to echo back */
    ngx_rtc_sdp_pt_t pts[NGX_RTC_SDP_MAX_PT];
} ngx_rtc_sdp_media_t;

typedef struct
{
    ngx_rtc_sdp_session_t session;
    ngx_rtc_sdp_media_t media[NGX_RTC_SDP_MAX_MEDIA];
    uint32_t n_media;
    int32_t  in_media;
} ngx_rtc_sdp_parser_t;

/* Append a formatted string to the writer; returns NGX_RTC_ERR_TOO_SMALL on overflow. */
static int32_t ngx_rtc_sdp_writer_append(ngx_rtc_sdp_writer_t *w, const char *fmt, ...)
{
    va_list ap;
    int32_t n;

    if (w->len >= w->cap)
    {
        return NGX_RTC_ERR_TOO_SMALL;
    }

    va_start(ap, fmt);
    n = vsnprintf(w->data + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);

    if (n < 0)
    {
        return NGX_RTC_ERR_PARSE;
    }
    if ((uint32_t)n >= (w->cap - w->len))
    {
        w->len = w->cap;
        return NGX_RTC_ERR_TOO_SMALL;
    }

    w->len += (uint32_t)n;
    return NGX_RTC_OK;
}

/* Copy src[0..src_len) into a fixed buffer with a trailing NUL. */
static int32_t ngx_rtc_sdp_copy_token(char *dst, uint32_t dst_cap, const char *src, uint32_t src_len)
{
    if (0 == dst_cap)
    {
        return NGX_RTC_ERR_TOO_SMALL;
    }
    if (src_len >= dst_cap)
    {
        return NGX_RTC_ERR_TOO_SMALL;
    }
    if (0 != src_len)
    {
        memcpy(dst, src, src_len);
    }
    dst[src_len] = '\0';
    return NGX_RTC_OK;
}

/* Parse an unsigned decimal number; stops at the first non-digit. */
static int32_t ngx_rtc_sdp_parse_u32(const char *s, uint32_t len, uint32_t *out)
{
    uint32_t v = 0;
    uint32_t i = 0;
    int32_t seen = 0;

    while (i < len)
    {
        char c = s[i];
        uint32_t d;

        if ((c < '0') || (c > '9'))
        {
            break;
        }
        seen = 1;
        d = (uint32_t)(c - '0');
        if (v > ((0xffffffffu - d) / 10u))
        {
            return NGX_RTC_ERR_PARSE;
        }
        v = v * 10u + d;
        i++;
    }

    if (0 == seen)
    {
        return NGX_RTC_ERR_PARSE;
    }
    *out = v;
    return NGX_RTC_OK;
}

/* Skip spaces and tabs. */
static void ngx_rtc_sdp_skip_ws(const char *s, uint32_t len, uint32_t *pos)
{
    while ((*pos < len) && ((' ' == s[*pos]) || ('\t' == s[*pos])))
    {
        (*pos)++;
    }
}

/* Extract the next whitespace-delimited token starting at *pos. */
static int32_t ngx_rtc_sdp_next_token(const char *s, uint32_t len, uint32_t *pos,
                                      const char **tok, uint32_t *tok_len)
{
    uint32_t start;

    ngx_rtc_sdp_skip_ws(s, len, pos);
    if (*pos >= len)
    {
        return NGX_RTC_ERR_PARSE;
    }

    start = *pos;
    while ((*pos < len) && (' ' != s[*pos]) && ('\t' != s[*pos]))
    {
        (*pos)++;
    }

    *tok = s + start;
    *tok_len = *pos - start;
    return NGX_RTC_OK;
}

/* Compare a length-bounded string with a NUL-terminated literal. */
static int32_t ngx_rtc_sdp_name_eq(const char *a, uint32_t a_len, const char *b)
{
    uint32_t b_len = 0;
    uint32_t i;

    while ('\0' != b[b_len])
    {
        b_len++;
    }
    if (a_len != b_len)
    {
        return 0;
    }
    for (i = 0; i < a_len; i++)
    {
        if (a[i] != b[i])
        {
            return 0;
        }
    }
    return 1;
}

/* Compare two NUL-terminated strings (exact). */
static int32_t ngx_rtc_sdp_streq(const char *a, const char *b)
{
    while (('\0' != *a) && ('\0' != *b))
    {
        if (*a != *b)
        {
            return 0;
        }
        a++;
        b++;
    }
    return (('\0' == *a) && ('\0' == *b));
}

/* Compare two NUL-terminated strings, ASCII case-insensitive. */
static int32_t ngx_rtc_sdp_streq_ci(const char *a, const char *b)
{
    while (('\0' != *a) && ('\0' != *b))
    {
        char ca = *a;
        char cb = *b;

        if ((ca >= 'a') && (ca <= 'z'))
        {
            ca = (char)(ca - 'a' + 'A');
        }
        if ((cb >= 'a') && (cb <= 'z'))
        {
            cb = (char)(cb - 'a' + 'A');
        }
        if (ca != cb)
        {
            return 0;
        }
        a++;
        b++;
    }
    return (('\0' == *a) && ('\0' == *b));
}

/* Look up a payload type entry in a media description. */
static ngx_rtc_sdp_pt_t *ngx_rtc_sdp_find_pt(ngx_rtc_sdp_media_t *m, uint8_t pt)
{
    uint32_t i;

    for (i = 0; i < m->n_pt; i++)
    {
        if (pt == m->pts[i].pt)
        {
            return &m->pts[i];
        }
    }
    return NULL;
}

/* Fill a session string field only when it is still empty (session level wins). */
static int32_t ngx_rtc_sdp_set_if_empty(char *dst, uint32_t dst_cap,
                                        const char *value, uint32_t value_len)
{
    if ('\0' != dst[0])
    {
        return NGX_RTC_OK;
    }
    return ngx_rtc_sdp_copy_token(dst, dst_cap, value, value_len);
}

/* Parse "a=fingerprint:<algo> <hash>". */
static int32_t ngx_rtc_sdp_parse_fingerprint(ngx_rtc_sdp_session_t *s,
                                             const char *value, uint32_t value_len)
{
    uint32_t pos = 0;
    const char *tok;
    uint32_t tok_len;
    int32_t r;

    r = ngx_rtc_sdp_next_token(value, value_len, &pos, &tok, &tok_len);
    if (0 != r)
    {
        return r;
    }

    if ('\0' != s->fingerprint_algo[0])
    {
        return NGX_RTC_OK;
    }
    r = ngx_rtc_sdp_copy_token(s->fingerprint_algo, sizeof(s->fingerprint_algo), tok, tok_len);
    if (0 != r)
    {
        return r;
    }

    ngx_rtc_sdp_skip_ws(value, value_len, &pos);

    /* The remainder is the fingerprint hash itself. */
    return ngx_rtc_sdp_copy_token(s->fingerprint, sizeof(s->fingerprint),
                                  value + pos, value_len - pos);
}

/* Parse "a=rtpmap:<pt> <encoding>/<clock>[/<channels>]" (RFC 4566). */
static int32_t ngx_rtc_sdp_parse_rtpmap(ngx_rtc_sdp_media_t *m,
                                        const char *value, uint32_t value_len)
{
    uint32_t pos = 0;
    const char *tok;
    uint32_t tok_len;
    uint32_t pt;
    uint32_t clock;
    uint32_t enc_len;
    const char *enc_start;
    const char *clock_start;
    uint32_t clock_len;
    ngx_rtc_sdp_pt_t *p;
    int32_t r;

    r = ngx_rtc_sdp_next_token(value, value_len, &pos, &tok, &tok_len);
    if (0 != r)
    {
        return r;
    }
    r = ngx_rtc_sdp_parse_u32(tok, tok_len, &pt);
    if (0 != r)
    {
        return r;
    }
    if (pt > 127u)
    {
        return NGX_RTC_ERR_PARSE;
    }

    p = ngx_rtc_sdp_find_pt(m, (uint8_t)pt);
    if (NULL == p)
    {
        return NGX_RTC_ERR_PARSE;
    }

    ngx_rtc_sdp_skip_ws(value, value_len, &pos);

    /* encoding name up to '/'. */
    enc_start = value + pos;
    enc_len = 0;
    while ((pos < value_len) && ('/' != value[pos]))
    {
        pos++;
        enc_len++;
    }
    r = ngx_rtc_sdp_copy_token(p->encoding, sizeof(p->encoding), enc_start, enc_len);
    if (0 != r)
    {
        return r;
    }

    /* clock rate up to '/' or end. */
    if (pos < value_len)
    {
        pos++;
    }
    clock_start = value + pos;
    clock_len = 0;
    while ((pos < value_len) && ('/' != value[pos]))
    {
        pos++;
        clock_len++;
    }
    r = ngx_rtc_sdp_parse_u32(clock_start, clock_len, &clock);
    if (0 != r)
    {
        return r;
    }
    p->clock_rate = clock;

    /* optional /channels. */
    if (pos < value_len)
    {
        pos++;
    }
    if (pos < value_len)
    {
        uint32_t channels;

        r = ngx_rtc_sdp_parse_u32(value + pos, value_len - pos, &channels);
        if (0 != r)
        {
            return r;
        }
        p->channels = channels;
    }

    return NGX_RTC_OK;
}

/* Parse "a=ssrc:<ssrc> <attr>:<value>"; only the SSRC number is kept. */
static int32_t ngx_rtc_sdp_parse_ssrc(ngx_rtc_sdp_media_t *m,
                                      const char *value, uint32_t value_len)
{
    uint32_t pos = 0;
    const char *tok;
    uint32_t tok_len;
    uint32_t ssrc;
    int32_t r;

    r = ngx_rtc_sdp_next_token(value, value_len, &pos, &tok, &tok_len);
    if (0 != r)
    {
        return r;
    }
    r = ngx_rtc_sdp_parse_u32(tok, tok_len, &ssrc);
    if (0 != r)
    {
        return r;
    }
    m->ssrc = ssrc;
    return NGX_RTC_OK;
}

/* Parse "a=fmtp:<pt> <format specific parameters>". */
static int32_t ngx_rtc_sdp_parse_fmtp(ngx_rtc_sdp_media_t *m,
                                      const char *value, uint32_t value_len)
{
    uint32_t pos = 0;
    const char *tok;
    uint32_t tok_len;
    uint32_t pt;
    ngx_rtc_sdp_pt_t *p;
    int32_t r;

    r = ngx_rtc_sdp_next_token(value, value_len, &pos, &tok, &tok_len);
    if (0 != r)
    {
        return r;
    }
    r = ngx_rtc_sdp_parse_u32(tok, tok_len, &pt);
    if (0 != r)
    {
        return r;
    }
    if (pt > 127u)
    {
        return NGX_RTC_ERR_PARSE;
    }

    p = ngx_rtc_sdp_find_pt(m, (uint8_t)pt);
    if (NULL == p)
    {
        return NGX_RTC_ERR_PARSE;
    }
    ngx_rtc_sdp_skip_ws(value, value_len, &pos);
    return ngx_rtc_sdp_copy_token(p->fmtp, sizeof(p->fmtp), value + pos, value_len - pos);
}

/* Parse "extmap:<id>[:dir] <uri>" and record the id + full URI when the
 * extension is transport-wide-cc (both the draft and the final urn URIs carry
 * "transport-wide-cc"). One-byte headers (RFC 8285) need id <= 14. The full URI
 * is kept so the answer echoes exactly what the client offered. */
static int32_t ngx_rtc_sdp_parse_extmap(ngx_rtc_sdp_media_t *m,
                                        const char *value, uint32_t value_len)
{
    static const char twcc[] = "transport-wide-cc";
    uint32_t pos = 0;
    uint32_t uri_start;
    uint32_t uri_len;
    uint32_t id = 0;
    uint32_t i;
    uint8_t  matched = 0;

    if ((NULL == m) || (NULL == value))
    {
        return NGX_RTC_ERR_INVALID;
    }

    ngx_rtc_sdp_skip_ws(value, value_len, &pos);
    while ((pos < value_len) && ('0' <= value[pos]) && ('9' >= value[pos]))
    {
        id = (id * 10u) + (uint32_t)(value[pos] - '0');
        pos++;
    }
    if (0 == id)
    {
        return NGX_RTC_OK;
    }

    /* The URI is the next whitespace-delimited token (no direction in the
     * offers we see, but tolerate a leading "dir " form by scanning on). */
    ngx_rtc_sdp_skip_ws(value, value_len, &pos);
    uri_start = pos;
    while ((pos < value_len) && (' ' != value[pos]) && ('\t' != value[pos])
            && ('\r' != value[pos]) && ('\n' != value[pos]))
    {
        pos++;
    }
    uri_len = pos - uri_start;

    for (i = uri_start; (i + sizeof(twcc) - 1u) <= (uri_start + uri_len); i++)
    {
        if (0 == memcmp(value + i, twcc, sizeof(twcc) - 1u))
        {
            matched = 1;
            break;
        }
    }

    if ((0 != matched) && (id <= 14u) && (uri_len > 0)
            && (uri_len < NGX_RTC_SDP_TWCC_URI_LEN))
    {
        memcpy(m->twcc_uri, value + uri_start, uri_len);
        m->twcc_uri[uri_len] = '\0';
        m->twcc_ext = (uint8_t)id;
    }

    return NGX_RTC_OK;
}

/* Dispatch one attribute line; media is NULL at session level. */
static int32_t ngx_rtc_sdp_apply_attr(ngx_rtc_sdp_parser_t *st,
                                      const char *name, uint32_t name_len,
                                      const char *value, uint32_t value_len,
                                      ngx_rtc_sdp_media_t *media)
{
    if (ngx_rtc_sdp_name_eq(name, name_len, "ice-ufrag"))
    {
        return ngx_rtc_sdp_set_if_empty(st->session.ice_ufrag, sizeof(st->session.ice_ufrag),
                                        value, value_len);
    }
    if (ngx_rtc_sdp_name_eq(name, name_len, "ice-pwd"))
    {
        return ngx_rtc_sdp_set_if_empty(st->session.ice_pwd, sizeof(st->session.ice_pwd),
                                        value, value_len);
    }
    if (ngx_rtc_sdp_name_eq(name, name_len, "setup"))
    {
        return ngx_rtc_sdp_set_if_empty(st->session.setup, sizeof(st->session.setup),
                                        value, value_len);
    }
    if (ngx_rtc_sdp_name_eq(name, name_len, "fingerprint"))
    {
        return ngx_rtc_sdp_parse_fingerprint(&st->session, value, value_len);
    }

    if (NULL == media)
    {
        return NGX_RTC_OK;
    }
    if (ngx_rtc_sdp_name_eq(name, name_len, "mid"))
    {
        return ngx_rtc_sdp_copy_token(media->mid, sizeof(media->mid), value, value_len);
    }
    if (ngx_rtc_sdp_name_eq(name, name_len, "rtpmap"))
    {
        return ngx_rtc_sdp_parse_rtpmap(media, value, value_len);
    }
    if (ngx_rtc_sdp_name_eq(name, name_len, "ssrc"))
    {
        return ngx_rtc_sdp_parse_ssrc(media, value, value_len);
    }
    if (ngx_rtc_sdp_name_eq(name, name_len, "fmtp"))
    {
        return ngx_rtc_sdp_parse_fmtp(media, value, value_len);
    }
    if (ngx_rtc_sdp_name_eq(name, name_len, "extmap"))
    {
        return ngx_rtc_sdp_parse_extmap(media, value, value_len);
    }

    /* msid, rtcp-fb, rtcp-mux, direction flags: not needed here. */
    return NGX_RTC_OK;
}

/* Split "name:value" at the first ':' and dispatch. */
static int32_t ngx_rtc_sdp_parse_attr(ngx_rtc_sdp_parser_t *st,
                                      const char *content, uint32_t content_len,
                                      ngx_rtc_sdp_media_t *media)
{
    uint32_t colon = 0;
    const char *name;
    uint32_t name_len;
    const char *value;
    uint32_t value_len;

    while ((colon < content_len) && (':' != content[colon]))
    {
        colon++;
    }

    name = content;
    name_len = colon;

    if (colon < content_len)
    {
        value = content + colon + 1u;
        value_len = content_len - colon - 1u;
    }
    else
    {
        value = content + content_len;
        value_len = 0;
    }

    return ngx_rtc_sdp_apply_attr(st, name, name_len, value, value_len, media);
}

/* Parse "m=<media> <port> <proto> <fmt>..." (RFC 4566). */
static int32_t ngx_rtc_sdp_parse_media(ngx_rtc_sdp_parser_t *st,
                                       const char *content, uint32_t content_len)
{
    ngx_rtc_sdp_media_t *m;
    uint32_t pos = 0;
    const char *tok;
    uint32_t tok_len;
    int32_t r;

    if (st->n_media >= NGX_RTC_SDP_MAX_MEDIA)
    {
        return NGX_RTC_ERR_TOO_LARGE;
    }

    m = &st->media[st->n_media];
    memset(m, 0, sizeof(*m));

    /* media type, e.g. "audio" / "video". */
    r = ngx_rtc_sdp_next_token(content, content_len, &pos, &tok, &tok_len);
    if (0 != r)
    {
        return r;
    }
    r = ngx_rtc_sdp_copy_token(m->type, sizeof(m->type), tok, tok_len);
    if (0 != r)
    {
        return r;
    }

    /* port (ignored for the minimal parser). */
    r = ngx_rtc_sdp_next_token(content, content_len, &pos, &tok, &tok_len);
    if (0 != r)
    {
        return r;
    }
    (void)tok;

    /* proto (ignored for the minimal parser). */
    r = ngx_rtc_sdp_next_token(content, content_len, &pos, &tok, &tok_len);
    if (0 != r)
    {
        return r;
    }
    (void)tok;

    /* remaining fmt tokens are payload type numbers. */
    while (pos < content_len)
    {
        uint32_t pt;

        r = ngx_rtc_sdp_next_token(content, content_len, &pos, &tok, &tok_len);
        if (0 != r)
        {
            break;
        }
        r = ngx_rtc_sdp_parse_u32(tok, tok_len, &pt);
        if (0 != r)
        {
            break;
        }
        if (pt > 127u)
        {
            return NGX_RTC_ERR_PARSE;
        }
        if (m->n_pt >= NGX_RTC_SDP_MAX_PT)
        {
            return NGX_RTC_ERR_TOO_LARGE;
        }
        m->pts[m->n_pt].pt = (uint8_t)pt;
        m->n_pt++;
    }

    st->n_media++;
    st->in_media = 1;
    return NGX_RTC_OK;
}

/* Parse one SDP line. */
static int32_t ngx_rtc_sdp_parse_line(ngx_rtc_sdp_parser_t *st,
                                      const char *line, uint32_t line_len)
{
    const char *content = line + 2;
    uint32_t content_len = line_len - 2u;

    switch (line[0])
    {
        case 'm':
            return ngx_rtc_sdp_parse_media(st, content, content_len);
        case 'a':
            if (0 != st->in_media)
            {
                return ngx_rtc_sdp_parse_attr(st, content, content_len,
                                              &st->media[st->n_media - 1u]);
            }
            return ngx_rtc_sdp_parse_attr(st, content, content_len, NULL);
        case 'v':
        case 'o':
        case 's':
        case 't':
        case 'c':
        default:
            break;
    }
    return NGX_RTC_OK;
}

/* Pick the H264 / opus payload type and SSRC out of the parsed media list. */
static int32_t ngx_rtc_sdp_build_offer(const ngx_rtc_sdp_parser_t *st, ngx_rtc_sdp_offer_t *out)
{
    uint32_t i;

    out->session = st->session;

    for (i = 0; i < st->n_media; i++)
    {
        const ngx_rtc_sdp_media_t *m = &st->media[i];
        uint32_t k;

        if (ngx_rtc_sdp_streq(m->type, "video"))
        {
            out->video_twcc_ext = m->twcc_ext;
            memcpy(out->video_twcc_uri, m->twcc_uri, sizeof(out->video_twcc_uri));
            for (k = 0; k < m->n_pt; k++)
            {
                if (ngx_rtc_sdp_streq_ci(m->pts[k].encoding, "H264"))
                {
                    out->video_pt = m->pts[k].pt;
                    out->video_ssrc = m->ssrc;
                    out->video_clock_rate = m->pts[k].clock_rate;
                    memcpy(out->video_encoding, m->pts[k].encoding, sizeof(out->video_encoding));
                    memcpy(out->video_fmtp, m->pts[k].fmtp, sizeof(out->video_fmtp));
                    break;
                }
            }
        }
        else if (ngx_rtc_sdp_streq(m->type, "audio"))
        {
            out->audio_twcc_ext = m->twcc_ext;
            memcpy(out->audio_twcc_uri, m->twcc_uri, sizeof(out->audio_twcc_uri));
            for (k = 0; k < m->n_pt; k++)
            {
                if (ngx_rtc_sdp_streq_ci(m->pts[k].encoding, "opus"))
                {
                    out->audio_pt = m->pts[k].pt;
                    out->audio_ssrc = m->ssrc;
                    out->audio_clock_rate = m->pts[k].clock_rate;
                    out->audio_channels = m->pts[k].channels;
                    memcpy(out->audio_encoding, m->pts[k].encoding, sizeof(out->audio_encoding));
                    memcpy(out->audio_fmtp, m->pts[k].fmtp, sizeof(out->audio_fmtp));
                    break;
                }
            }
        }
    }

    /* Record the offer's m-line order and mids so the answer can mirror them
     * (RFC 3264 requires the answer m-line order to match the offer). */
    out->n_media = st->n_media;
    for (i = 0; i < st->n_media; i++)
    {
        memcpy(out->media_type[i], st->media[i].type, sizeof(out->media_type[i]));
        memcpy(out->media_mid[i], st->media[i].mid, sizeof(out->media_mid[i]));
    }

    return NGX_RTC_OK;
}

int32_t ngx_rtc_sdp_parse_offer(const char *sdp, uint32_t len, ngx_rtc_sdp_offer_t *out)
{
    ngx_rtc_sdp_parser_t st;
    uint32_t pos = 0;

    if ((NULL == sdp) || (NULL == out))
    {
        return NGX_RTC_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    memset(&st, 0, sizeof(st));

    while (pos < len)
    {
        uint32_t line_start = pos;
        uint32_t line_len;
        int32_t r;

        while ((pos < len) && ('\r' != sdp[pos]) && ('\n' != sdp[pos]))
        {
            pos++;
        }
        line_len = pos - line_start;

        if (pos < len)
        {
            if ('\r' == sdp[pos])
            {
                pos++;
            }
            if ((pos < len) && ('\n' == sdp[pos]))
            {
                pos++;
            }
        }

        if ((line_len >= 2u) && ('=' == sdp[line_start + 1u]))
        {
            r = ngx_rtc_sdp_parse_line(&st, sdp + line_start, line_len);
            if (0 != r)
            {
                return r;
            }
        }
    }

    return ngx_rtc_sdp_build_offer(&st, out);
}

void ngx_rtc_sdp_answer_init(ngx_rtc_sdp_answer_t *cfg)
{
    if (NULL == cfg)
    {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->proto = "UDP/TLS/RTP/SAVPF";
    cfg->video_pt = 102u;
    cfg->audio_pt = 111u;
    cfg->video_clock_rate = 90000u;
    cfg->audio_clock_rate = 48000u;
    cfg->audio_channels = 2u;
    cfg->video_fmtp = "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f";
    cfg->audio_fmtp = "minptime=10;useinbandfec=1";
    cfg->sendonly = 1;
    cfg->rtcp_mux = 1;
    cfg->rtcp_rsize = 1;

    /* Default media order: video (mid 0) then audio (mid 1). The caller
     * overrides these from the parsed offer before generating the answer. */
    cfg->n_media = 2u;
    ngx_rtc_sdp_copy_token(cfg->media_type[0], NGX_RTC_SDP_STR_LEN, "video", 5);
    ngx_rtc_sdp_copy_token(cfg->media_mid[0], NGX_RTC_SDP_MID_LEN, "0", 1);
    ngx_rtc_sdp_copy_token(cfg->media_type[1], NGX_RTC_SDP_STR_LEN, "audio", 5);
    ngx_rtc_sdp_copy_token(cfg->media_mid[1], NGX_RTC_SDP_MID_LEN, "1", 1);
}

/* Emit "a=<name>:<value>" when value is non-empty. */
static int32_t ngx_rtc_sdp_write_attr(ngx_rtc_sdp_writer_t *w, const char *name, const char *value)
{
    if ((NULL == value) || ('\0' == value[0]))
    {
        return NGX_RTC_OK;
    }
    return ngx_rtc_sdp_writer_append(w, "a=%s:%s\r\n", name, value);
}

/* Emit one m= section. */
static int32_t ngx_rtc_sdp_write_media(ngx_rtc_sdp_writer_t *w,
                                       const char *type, const char *mid,
                                       uint8_t pt, uint32_t ssrc, uint32_t clock_rate,
                                       uint32_t channels, const char *encoding,
                                       const char *fmtp, const ngx_rtc_sdp_answer_t *cfg,
                                       uint8_t twcc_ext, const char *twcc_uri)
{
    int32_t r;

    r = ngx_rtc_sdp_writer_append(w, "m=%s 9 %s %u\r\n", type, cfg->proto, (unsigned)pt);
    if (0 != r)
    {
        return r;
    }
    r = ngx_rtc_sdp_writer_append(w, "c=IN IP4 0.0.0.0\r\n");
    if (0 != r)
    {
        return r;
    }
    r = ngx_rtc_sdp_writer_append(w, "a=mid:%s\r\n", mid);
    if (0 != r)
    {
        return r;
    }
    /* Echo the negotiated transport-wide-cc header extension (RFC 3550 +
     * RFC 8285 one-byte header): the peer only runs transport-cc when both the
     * offer and the answer carry the extmap for the medium, and it must carry
     * the exact URI the peer offered (draft and final URIs both appear). */
    if ((0 != twcc_ext) && (NULL != twcc_uri) && ('\0' != twcc_uri[0]))
    {
        r = ngx_rtc_sdp_writer_append(w, "a=extmap:%u %s\r\n",
                                      (unsigned)twcc_ext, twcc_uri);
        if (0 != r)
        {
            return r;
        }
    }
    if ((NULL != cfg->candidate_ip) && ('\0' != cfg->candidate_ip[0]))
    {
        r = ngx_rtc_sdp_writer_append(w,
                "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                cfg->candidate_ip, (unsigned)cfg->candidate_port);
        if (0 != r)
        {
            return r;
        }
    }
    if (0 != cfg->sendonly)
    {
        r = ngx_rtc_sdp_writer_append(w,
                (0 < cfg->sendonly) ? "a=sendonly\r\n" : "a=recvonly\r\n");
        if (0 != r)
        {
            return r;
        }
    }
    if (0 != cfg->rtcp_mux)
    {
        r = ngx_rtc_sdp_writer_append(w, "a=rtcp-mux\r\n");
        if (0 != r)
        {
            return r;
        }
    }
    if (0 != cfg->rtcp_rsize)
    {
        r = ngx_rtc_sdp_writer_append(w, "a=rtcp-rsize\r\n");
        if (0 != r)
        {
            return r;
        }
    }

    if (0 != channels)
    {
        r = ngx_rtc_sdp_writer_append(w, "a=rtpmap:%u %s/%u/%u\r\n",
                                      (unsigned)pt, encoding, (unsigned)clock_rate, (unsigned)channels);
    }
    else
    {
        r = ngx_rtc_sdp_writer_append(w, "a=rtpmap:%u %s/%u\r\n",
                                      (unsigned)pt, encoding, (unsigned)clock_rate);
    }
    if (0 != r)
    {
        return r;
    }

    if ((NULL != fmtp) && ('\0' != fmtp[0]))
    {
        r = ngx_rtc_sdp_writer_append(w, "a=fmtp:%u %s\r\n", (unsigned)pt, fmtp);
        if (0 != r)
        {
            return r;
        }
    }

    /* Declare RTCP feedback (RFC 4585) for the payload type used in this
     * answer. a=rtcp-fb states what the SDP author can RECEIVE, and RFC 3264
     * negotiation means the peer only acts on it when both offer and answer
     * carry it -- so this list has to name exactly what the path behind it
     * generates, no more. Only the video track appears: the server retransmits
     * video only, so advertising anything on audio would invite requests we
     * cannot serve. */
    if (ngx_rtc_sdp_streq(type, "video"))
    {
        /* PLI holds in both directions: the server reads viewer PLI off the
         * play path (replay from cache) and sends its own PLI down the WHIP
         * path when a viewer cannot be served (ngx_rtc_stream_request_keyframe). */
        r = ngx_rtc_sdp_writer_append(w, "a=rtcp-fb:%u nack pli\r\n",
                                      (unsigned)pt);
        if (0 != r)
        {
            return r;
        }

        /* NACK belongs to the sendonly answer only. The play path answers
         * viewer NACK from the GOP ring, so declaring it is a promise the
         * server keeps. The recvonly (WHIP) answer has no receive-side
         * retransmission request generator at all: declaring it there makes
         * the publisher enable RTX and wait for requests that never come, and
         * is exactly what made "we advertise feedback we do not send" true. */
        if (cfg->sendonly > 0)
        {
            r = ngx_rtc_sdp_writer_append(w, "a=rtcp-fb:%u nack\r\n",
                                          (unsigned)pt);
            if (0 != r)
            {
                return r;
            }
        }
    }

    return ngx_rtc_sdp_writer_append(w, "a=ssrc:%u cname:%u\r\n", (unsigned)ssrc, (unsigned)ssrc);
}

int32_t ngx_rtc_sdp_generate_answer(const ngx_rtc_sdp_answer_t *cfg,
                                    char *buf, uint32_t cap, uint32_t *out_len)
{
    ngx_rtc_sdp_writer_t w;
    int32_t r;
    uint32_t mi;

    if ((NULL == cfg) || (NULL == buf) || (NULL == out_len))
    {
        return NGX_RTC_ERR_INVALID;
    }
    if (0 == cap)
    {
        return NGX_RTC_ERR_TOO_SMALL;
    }

    w.data = buf;
    w.cap = cap;
    w.len = 0;

    r = ngx_rtc_sdp_writer_append(&w, "v=0\r\n");
    if (0 != r)
    {
        return r;
    }
    r = ngx_rtc_sdp_writer_append(&w, "o=ngx-rtc 0 1 IN IP4 0.0.0.0\r\n");
    if (0 != r)
    {
        return r;
    }
    r = ngx_rtc_sdp_writer_append(&w, "s=-\r\n");
    if (0 != r)
    {
        return r;
    }
    r = ngx_rtc_sdp_writer_append(&w, "t=0 0\r\n");
    if (0 != r)
    {
        return r;
    }
    /* BUNDLE lists the answered mids in offer order. */
    r = ngx_rtc_sdp_writer_append(&w, "a=group:BUNDLE");
    if (0 != r)
    {
        return r;
    }
    for (mi = 0; mi < cfg->n_media; mi++)
    {
        if (ngx_rtc_sdp_streq(cfg->media_type[mi], "video")
                || ngx_rtc_sdp_streq(cfg->media_type[mi], "audio"))
        {
            r = ngx_rtc_sdp_writer_append(&w, " %s", cfg->media_mid[mi]);
            if (0 != r)
            {
                return r;
            }
        }
    }
    r = ngx_rtc_sdp_writer_append(&w, "\r\n");
    if (0 != r)
    {
        return r;
    }
    r = ngx_rtc_sdp_writer_append(&w, "a=msid-semantic: WMS\r\n");
    if (0 != r)
    {
        return r;
    }
    r = ngx_rtc_sdp_writer_append(&w, "a=ice-lite\r\n");
    if (0 != r)
    {
        return r;
    }

    r = ngx_rtc_sdp_write_attr(&w, "ice-ufrag", cfg->ice_ufrag);
    if (0 != r)
    {
        return r;
    }
    r = ngx_rtc_sdp_write_attr(&w, "ice-pwd", cfg->ice_pwd);
    if (0 != r)
    {
        return r;
    }

    if ((NULL != cfg->fingerprint_algo) && ('\0' != cfg->fingerprint_algo[0]) &&
        (NULL != cfg->fingerprint) && ('\0' != cfg->fingerprint[0]))
    {
        r = ngx_rtc_sdp_writer_append(&w, "a=fingerprint:%s %s\r\n",
                                      cfg->fingerprint_algo, cfg->fingerprint);
        if (0 != r)
        {
            return r;
        }
    }

    r = ngx_rtc_sdp_write_attr(&w, "setup", cfg->setup);
    if (0 != r)
    {
        return r;
    }

    /* Emit one m= section per offer media, in offer order (RFC 3264). */
    for (mi = 0; mi < cfg->n_media; mi++)
    {
        if (ngx_rtc_sdp_streq(cfg->media_type[mi], "video"))
        {
            r = ngx_rtc_sdp_write_media(&w, "video", cfg->media_mid[mi],
                                        cfg->video_pt, cfg->video_ssrc,
                                        cfg->video_clock_rate, 0u, "H264",
                                        cfg->video_fmtp, cfg,
                                        cfg->video_twcc_ext,
                                        cfg->video_twcc_uri);
        }
        else if (ngx_rtc_sdp_streq(cfg->media_type[mi], "audio"))
        {
            r = ngx_rtc_sdp_write_media(&w, "audio", cfg->media_mid[mi],
                                        cfg->audio_pt, cfg->audio_ssrc,
                                        cfg->audio_clock_rate, cfg->audio_channels,
                                        "opus", cfg->audio_fmtp, cfg,
                                        cfg->audio_twcc_ext,
                                        cfg->audio_twcc_uri);
        }
        else
        {
            /* Reject media we do not carry (e.g. application) with port 0. */
            r = ngx_rtc_sdp_writer_append(&w, "m=%s 0 %s\r\n",
                                          cfg->media_type[mi], cfg->proto);
        }
        if (0 != r)
        {
            return r;
        }
    }

    *out_len = w.len;
    return NGX_RTC_OK;
}
