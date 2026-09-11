/**
 * @file    ngx_rtc_avsync.c
 * @brief   SR-anchor RTP <-> NTP wall-clock mapping (see ngx_rtc_avsync.h).
 * @version 0.5.0
 * @license MIT, see LICENSE
 *
 * Holds the two Sender Reports that anchor the mapping and the exact rational
 * slope dntp/drtp between them. The 64x64-bit multiply that applies the slope
 * is done in __uint128_t so it cannot overflow, and all RTP arithmetic is
 * wrap-aware 32-bit distance.
 */

#include "ngx_rtc_avsync.h"

#include <stddef.h>

/* Wrap-aware signed 32-bit RTP distance; correct while |delta| < 2^31. */
static int32_t
ngx_rtc_avsync_rtp_delta(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

/* Record the two calibration endpoints and the exact rational slope. The
 * caller guarantees ntp_prev < ntp_cur and drtp > 0. */
static void
ngx_rtc_avsync_calibrate(ngx_rtc_avsync_t *s,
                         uint64_t ntp_prev, uint32_t rtp_prev,
                         uint64_t ntp_cur, uint32_t rtp_cur, uint32_t drtp)
{
    s->ntp_prev = ntp_prev;
    s->rtp_prev = rtp_prev;
    s->ntp_cur = ntp_cur;
    s->rtp_cur = rtp_cur;
    s->slope_num = ntp_cur - ntp_prev;
    s->slope_den = (uint64_t)drtp;
    s->has_slope = 1;
}

void
ngx_rtc_avsync_init(ngx_rtc_avsync_t *s)
{
    if (NULL == s)
    {
        return;
    }

    s->has_anchor = 0;
    s->has_slope = 0;
    s->ntp_cur = 0;
    s->rtp_cur = 0;
    s->ntp_prev = 0;
    s->rtp_prev = 0;
    s->slope_num = 0;
    s->slope_den = 0;
}

int32_t
ngx_rtc_avsync_on_sr(ngx_rtc_avsync_t *s, uint64_t ntp, uint32_t rtp_ts)
{
    int32_t drtp;

    if (NULL == s)
    {
        return NGX_RTC_ERR_INVALID;
    }

    if (0 == s->has_anchor)
    {
        s->ntp_cur = ntp;
        s->rtp_cur = rtp_ts;
        s->has_anchor = 1;
        return NGX_RTC_AGAIN;
    }

    if (ntp == s->ntp_cur)
    {
        /* Duplicate (or same-NTP) SR: idempotent, leave the anchor alone. */
        return NGX_RTC_OK;
    }

    if (ntp < s->ntp_cur)
    {
        /* Late/out-of-order SR: use it as the earlier calibration endpoint but
         * do not regress the mapping reference (ntp_cur/rtp_cur). */
        drtp = ngx_rtc_avsync_rtp_delta(s->rtp_cur, rtp_ts);
        if (drtp <= 0)
        {
            /* rtp_ts did not move backwards consistently: ignore the report. */
            return NGX_RTC_OK;
        }

        ngx_rtc_avsync_calibrate(s, ntp, rtp_ts, s->ntp_cur, s->rtp_cur,
                                 (uint32_t)drtp);
        return NGX_RTC_OK;
    }

    /* Forward SR: shift the anchor pair and recompute the slope. */
    drtp = ngx_rtc_avsync_rtp_delta(rtp_ts, s->rtp_cur);
    if (drtp <= 0)
    {
        /* rtp_ts stalled or regressed: keep the current calibration. */
        return NGX_RTC_OK;
    }

    ngx_rtc_avsync_calibrate(s, s->ntp_cur, s->rtp_cur, ntp, rtp_ts,
                             (uint32_t)drtp);
    return NGX_RTC_OK;
}

int32_t
ngx_rtc_avsync_map_rtp_to_wall(const ngx_rtc_avsync_t *s,
                               uint32_t rtp_ts, uint64_t *ntp_out)
{
    int64_t delta;
    uint64_t adelta;
    uint64_t ntp_delta;

    if ((NULL == s) || (NULL == ntp_out))
    {
        return NGX_RTC_ERR_INVALID;
    }

    if (0 == s->has_slope)
    {
        return NGX_RTC_AGAIN;
    }

    delta = (int64_t)ngx_rtc_avsync_rtp_delta(rtp_ts, s->rtp_cur);

    if (delta >= 0)
    {
        adelta = (uint64_t)delta;
        ntp_delta = (uint64_t)(((__uint128_t)adelta * s->slope_num) / s->slope_den);
        *ntp_out = s->ntp_cur + ntp_delta;
    }
    else
    {
        adelta = (uint64_t)(-delta);
        ntp_delta = (uint64_t)(((__uint128_t)adelta * s->slope_num) / s->slope_den);
        *ntp_out = s->ntp_cur - ntp_delta;
    }

    return NGX_RTC_OK;
}
