/*
 * ngx_rtc_sdp.h - Minimal WebRTC SDP offer parser and answer generator
 *
 * Pure C11 translation of SRS 6.0:
 *   src/app/srs_app_rtc_sdp.cpp  (SrsSessionInfo::parse_attribute/encode,
 *                                 SrsSdp::parse, SrsSdp::encode,
 *                                 SrsMediaDesc::parse_attr_rtpmap,
 *                                 SrsMediaDesc::parse_attr_ssrc)
 *   src/app/srs_app_rtc_conn.cpp (generate_publish_local_sdp_for_audio/video)
 *
 * Supports H264 + Opus only. No global state and no dynamic allocation: parsed
 * values live in fixed-size structs and the answer is written into a
 * caller-provided buffer.
 */

#ifndef NGX_RTC_SDP_H
#define NGX_RTC_SDP_H

#include <stdint.h>

/* Return codes (also declared in ngx_rtc_rtp.h; guarded so both headers can be included). */
#ifndef NGX_RTC_OK
#define NGX_RTC_OK 0
#endif
#ifndef NGX_RTC_ERR_INVALID
#define NGX_RTC_ERR_INVALID (-1)
#endif
#ifndef NGX_RTC_ERR_TOO_SMALL
#define NGX_RTC_ERR_TOO_SMALL (-2)
#endif
#ifndef NGX_RTC_ERR_TOO_LARGE
#define NGX_RTC_ERR_TOO_LARGE (-3)
#endif
#ifndef NGX_RTC_ERR_PARSE
#define NGX_RTC_ERR_PARSE (-4)
#endif

/* Fixed buffer sizes for parsed values. */
#define NGX_RTC_SDP_ICE_UFRAG_LEN 64u
#define NGX_RTC_SDP_ICE_PWD_LEN   256u
#define NGX_RTC_SDP_FP_ALGO_LEN   32u
#define NGX_RTC_SDP_FP_LEN        160u
#define NGX_RTC_SDP_SETUP_LEN     32u
#define NGX_RTC_SDP_STR_LEN       64u
#define NGX_RTC_SDP_FMTP_LEN      256u
#define NGX_RTC_SDP_MAX_MEDIA     4u
#define NGX_RTC_SDP_MID_LEN       32u
#define NGX_RTC_SDP_TWCC_URI_LEN  96u   /* full transport-wide-cc extension URI */

/* Session-level ICE/DTLS parameters (RFC 5245 / RFC 4572). */
typedef struct
{
    char ice_ufrag[NGX_RTC_SDP_ICE_UFRAG_LEN];
    char ice_pwd[NGX_RTC_SDP_ICE_PWD_LEN];
    char fingerprint_algo[NGX_RTC_SDP_FP_ALGO_LEN];
    char fingerprint[NGX_RTC_SDP_FP_LEN];
    char setup[NGX_RTC_SDP_SETUP_LEN];
} ngx_rtc_sdp_session_t;

/* Parsed remote SDP offer. */
typedef struct
{
    ngx_rtc_sdp_session_t session;

    uint8_t  video_pt;
    uint32_t video_ssrc;
    char     video_encoding[NGX_RTC_SDP_STR_LEN];
    uint32_t video_clock_rate;
    char     video_fmtp[NGX_RTC_SDP_FMTP_LEN];

    uint8_t  audio_pt;
    uint32_t audio_ssrc;
    char     audio_encoding[NGX_RTC_SDP_STR_LEN];
    uint32_t audio_clock_rate;
    uint32_t audio_channels;
    char     audio_fmtp[NGX_RTC_SDP_FMTP_LEN];

    /* transport-wide-cc RTP header-extension id per medium as offered by the
     * client (0 = the client did not negotiate it for that medium). The answer
     * echoes the same id + the exact URI the client used (the draft and final
     * transport-wide-cc URIs both appear in the wild), so the peer enables
     * transport-cc feedback. */
    uint8_t  video_twcc_ext;
    uint8_t  audio_twcc_ext;
    char     video_twcc_uri[NGX_RTC_SDP_TWCC_URI_LEN];
    char     audio_twcc_uri[NGX_RTC_SDP_TWCC_URI_LEN];

    /* Offer m-line order + mid, so the answer can mirror the offer's media
     * order (RFC 3264 rejects an answer whose m-line order differs). */
    uint32_t n_media;
    char     media_type[NGX_RTC_SDP_MAX_MEDIA][NGX_RTC_SDP_STR_LEN];
    char     media_mid[NGX_RTC_SDP_MAX_MEDIA][NGX_RTC_SDP_MID_LEN];
} ngx_rtc_sdp_offer_t;

/* Server-side answer configuration. Pointers may be NULL to skip an attribute. */
typedef struct
{
    const char *ice_ufrag;
    const char *ice_pwd;
    const char *fingerprint_algo;
    const char *fingerprint;
    const char *setup;   /* DTLS role, e.g. "passive" (RFC 4145) */
    const char *proto;   /* e.g. "UDP/TLS/RTP/SAVPF" or "RTP/AVP" */

    const char *candidate_ip;    /* server host candidate IP, or NULL to skip */
    uint32_t    candidate_port;  /* server host candidate UDP port */

    /* Answer m-line order + mid, mirrored from the offer (defaults in
     * ngx_rtc_sdp_answer_init; the caller overrides from the parsed offer). */
    uint32_t n_media;
    char     media_type[NGX_RTC_SDP_MAX_MEDIA][NGX_RTC_SDP_STR_LEN];
    char     media_mid[NGX_RTC_SDP_MAX_MEDIA][NGX_RTC_SDP_MID_LEN];

    uint32_t video_ssrc;
    uint32_t audio_ssrc;
    uint8_t  video_pt;
    uint8_t  audio_pt;
    uint32_t video_clock_rate;
    uint32_t audio_clock_rate;
    uint32_t audio_channels;

    /* transport-wide-cc ext id echoed into the matching answer m= section
     * (0 = omit). Mirrored from the offer by the caller. uri must stay valid
     * through ngx_rtc_sdp_generate_answer (usually points into the offer). */
    uint8_t  video_twcc_ext;
    uint8_t  audio_twcc_ext;
    const char *video_twcc_uri;
    const char *audio_twcc_uri;

    const char *video_fmtp;
    const char *audio_fmtp;

    int32_t sendonly;   /* 1 = sendonly, -1 = recvonly, 0 = sendrecv */
    int32_t rtcp_mux;
    int32_t rtcp_rsize;
} ngx_rtc_sdp_answer_t;

/*
 * Parse a remote SDP offer. sdp is a byte buffer of len bytes (may contain \r\n
 * or \n line endings). On success fills out and returns NGX_RTC_OK. video_pt /
 * audio_pt remain 0 when no matching H264 / opus payload was found.
 */
int32_t ngx_rtc_sdp_parse_offer(const char *sdp, uint32_t len, ngx_rtc_sdp_offer_t *out);

/*
 * Initialize an answer config with SRS-compatible defaults: proto
 * "UDP/TLS/RTP/SAVPF", H264 PT 102 @90000 with packetization-mode=1, Opus
 * PT 111 @48000/2, sendonly/rtcp-mux/rtcp-rsize enabled. The caller still has to
 * fill ice_ufrag, ice_pwd, fingerprint* and the SSRCs.
 */
void ngx_rtc_sdp_answer_init(ngx_rtc_sdp_answer_t *cfg);

/*
 * Generate an SDP answer into a caller-provided buffer. *out_len receives the
 * text length (without the terminating NUL). Returns NGX_RTC_OK or
 * NGX_RTC_ERR_TOO_SMALL when the buffer is not large enough.
 */
int32_t ngx_rtc_sdp_generate_answer(const ngx_rtc_sdp_answer_t *cfg,
                                    char *buf, uint32_t cap, uint32_t *out_len);

#endif /* NGX_RTC_SDP_H */
