/**
 * @file    ngx_rtc_avsync.h
 * @brief   Map a media RTP timestamp onto the NTP wall clock via the RTCP
 *          Sender Report anchor.
 * @version 0.5.0
 * @license MIT, see LICENSE
 *
 * Pure C11 translation of SRS 6.0 SrsRtcRecvTrack:
 *   update_send_report_time (SrsRtcPublishStream::on_rtcp_sr) -> ngx_rtc_avsync_on_sr
 *   cal_avsync_time (SrsRtcVideoRecvTrack::on_rtp)              -> ngx_rtc_avsync_map_rtp_to_wall
 *
 * Two SRs (ntp, rtp_ts) anchor a line ntp = ntp0 + (rtp_ts - rtp_ts0) * slope,
 * where slope = dntp/drtp is kept as an exact rational so the 64x64 multiply
 * cannot overflow (GCC/Clang __uint128_t in the .c file). The newest accepted
 * SR is the mapping reference (ntp_cur/rtp_cur); a late out-of-order SR may
 * complete the slope but never moves that reference backward.
 *
 * No global state; the caller owns ngx_rtc_avsync_t and calls on_sr whenever an
 * SR is decoded and map_rtp_to_wall whenever an RTP packet timestamp must be
 * placed on the wall clock. Depends only on <stdint.h> and the shared return
 * codes also declared by ngx_rtc_rtcp.h / ngx_rtc_core.h.
 */

#ifndef NGX_RTC_AVSYNC_H
#define NGX_RTC_AVSYNC_H

#include <stdint.h>

#ifndef NGX_RTC_OK
#define NGX_RTC_OK 0
#endif
#ifndef NGX_RTC_AGAIN
#define NGX_RTC_AGAIN (-6)
#endif
#ifndef NGX_RTC_ERR_INVALID
#define NGX_RTC_ERR_INVALID (-1)
#endif

/*
 * Per-track SR anchor state. prev/cur are the two calibration anchors, cur is
 * the mapping reference. slope_num/slope_den is the exact rational slope in
 * NTP ticks per RTP tick; den is the wrap-aware RTP distance between anchors.
 */
typedef struct
{
    uint8_t  has_anchor;  /* at least one SR accepted */
    uint8_t  has_slope;   /* two anchors accepted, mapping is possible */
    uint64_t ntp_cur;     /* newest accepted SR NTP timestamp */
    uint32_t rtp_cur;     /* newest accepted SR RTP timestamp */
    uint64_t ntp_prev;    /* previous accepted SR NTP timestamp */
    uint32_t rtp_prev;    /* previous accepted SR RTP timestamp */
    uint64_t slope_num;   /* NTP ticks between the two anchors */
    uint64_t slope_den;   /* RTP ticks between the two anchors */
} ngx_rtc_avsync_t;

/* Reset all state; safe to call on a zeroed struct. */
void ngx_rtc_avsync_init(ngx_rtc_avsync_t *s);

/*
 * Feed one decoded SR anchor (RFC 3550 sender info). Returns:
 *   NGX_RTC_AGAIN        - first anchor recorded, not yet mappable
 *   NGX_RTC_OK           - anchor accepted (slope refreshed when possible)
 *   NGX_RTC_ERR_INVALID  - s is NULL
 *
 * A forward SR (newer NTP) shifts prev/cur and recomputes the slope. A
 * duplicate SR (same NTP) is idempotent. A late SR (older NTP) may complete
 * the slope against cur but never regresses the cur anchor. The 32-bit rtp_ts
 * is compared with wrap-aware modular arithmetic.
 */
int32_t ngx_rtc_avsync_on_sr(ngx_rtc_avsync_t *s, uint64_t ntp, uint32_t rtp_ts);

/*
 * Map an RTP timestamp onto the NTP wall clock:
 *   ntp = ntp_cur + (rtp_ts - rtp_cur) * slope_num / slope_den
 * Returns NGX_RTC_OK with *ntp_out filled, NGX_RTC_AGAIN when fewer than two
 * anchors are recorded, or NGX_RTC_ERR_INVALID for a NULL argument.
 */
int32_t ngx_rtc_avsync_map_rtp_to_wall(const ngx_rtc_avsync_t *s,
                                       uint32_t rtp_ts, uint64_t *ntp_out);

#endif /* NGX_RTC_AVSYNC_H */
