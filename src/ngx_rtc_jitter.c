/*
 * ngx_rtc_jitter.c - WHIP uplink reorder buffer (pure C11).
 *
 * See ngx_rtc_jitter.h. The buffer is intentionally tiny and allocation-free:
 * a fixed slot array indexed by seq & (capacity - 1) with a 16-bit modular
 * comparison, mirroring the RTP sequence semantics used by the GOP retransmit
 * ring. A gap older than NGX_RTC_JITTER_TIMEOUT_MS is skipped rather than
 * generating an upstream NACK (the module has no RTCP NACK generator and the
 * publisher may not implement RTX).
 */

#include "ngx_rtc_jitter.h"

#include <string.h>

/* 16-bit modular sequence comparison: positive when a is "after" b. */
static int32_t
ngx_rtc_jitter_seq_gt(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) > 0;
}

/* Emit the run of consecutive cached packets starting at next_seq. */
static void
ngx_rtc_jitter_drain(ngx_rtc_jitter_t *j, ngx_rtc_jitter_emit_fn cb,
                     void *opaque)
{
    ngx_rtc_jitter_slot_t *slot;

    for (;;) {
        slot = &j->slots[j->next_seq & (NGX_RTC_JITTER_CAP - 1u)];
        if (0 == slot->valid) {
            break;
        }
        (void) cb(opaque, slot->data, slot->len, slot->is_gop_start);
        slot->valid = 0;
        if (j->pending > 0) {
            j->pending--;
        }
        j->next_seq++;
    }

    /* Contiguous again: the next reorder starts a fresh timeout window. Keep
     * gap_ts while anything is still queued, otherwise a gap filled in order
     * would restart the deadline of the packets waiting behind it. */
    if (0 == j->pending) {
        j->gap_ts = 0;
    }
}

/* Skip the missing run up to the first cached packet, then drain from there. */
static void
ngx_rtc_jitter_skip_gap(ngx_rtc_jitter_t *j, ngx_rtc_jitter_emit_fn cb,
                        void *opaque)
{
    while (0 == j->slots[j->next_seq & (NGX_RTC_JITTER_CAP - 1u)].valid) {
        j->next_seq++;
        j->n_skipped++;
    }
    ngx_rtc_jitter_drain(j, cb, opaque);
}

int32_t
ngx_rtc_jitter_push(ngx_rtc_jitter_t *j, const uint8_t *rtp, uint32_t len,
                    uint16_t seq, uint8_t is_gop_start, uint32_t now,
                    ngx_rtc_jitter_emit_fn cb, void *opaque)
{
    ngx_rtc_jitter_slot_t *slot;
    uint32_t               timeout;

    if (NULL == j || NULL == rtp || NULL == cb || 0 == len
            || len > NGX_RTC_MAX_RTP_PKT) {
        return NGX_RTC_ERR_INVALID;
    }

    timeout = (0 != j->gap_timeout_ms) ? j->gap_timeout_ms
                                       : NGX_RTC_JITTER_TIMEOUT_MS;

    if (0 == j->initialized) {
        j->next_seq = seq;
        j->initialized = 1;
    }

    /* In-order: emit immediately, then unlock any consecutive cached run. */
    if (seq == j->next_seq) {
        (void) cb(opaque, rtp, len, is_gop_start);
        j->next_seq++;
        ngx_rtc_jitter_drain(j, cb, opaque);

        /* Packets are still queued behind an unresolved gap: enforce the
         * deadline on every arrival, in-order ones included, so a gap that is
         * never filled cannot hold the queue past its timeout. */
        if (j->pending > 0 && now - j->gap_ts >= timeout) {
            ngx_rtc_jitter_skip_gap(j, cb, opaque);
        }
        return NGX_RTC_OK;
    }

    /* Ahead of the expected sequence (reordering or a fresh gap): cache it. */
    if (ngx_rtc_jitter_seq_gt(seq, j->next_seq)) {
        /* A packet CAP or more ahead would alias an un-drained slot (an extreme
         * ahead-burst or a corrupted seq). The gap is unrecoverable: drop all
         * buffered state and re-anchor at this packet so late arrivals of the
         * previous generation are treated as stale instead of overwriting slots. */
        if ((uint16_t)(seq - j->next_seq) >= NGX_RTC_JITTER_CAP) {
            ngx_rtc_jitter_reset(j);
            j->n_reanchor++;
            j->next_seq = seq;
            j->initialized = 1;
            (void) cb(opaque, rtp, len, is_gop_start);
            j->next_seq++;
            return NGX_RTC_OK;
        }

        slot = &j->slots[seq & (NGX_RTC_JITTER_CAP - 1u)];
        if (0 == slot->valid) {
            j->pending++;
        }
        memcpy(slot->data, rtp, len);
        slot->len = (uint16_t) len;
        slot->is_gop_start = (uint8_t) (0 != is_gop_start ? 1 : 0);
        slot->valid = 1;
        j->n_cached++;

        if (0 == j->gap_ts) {
            j->gap_ts = now;
        }
        if (now - j->gap_ts >= timeout) {
            ngx_rtc_jitter_skip_gap(j, cb, opaque);
        }
        return NGX_RTC_OK;
    }

    /* Behind the expected sequence: a stale duplicate, drop it. */
    j->n_stale++;
    return NGX_RTC_OK;
}

void
ngx_rtc_jitter_reset(ngx_rtc_jitter_t *j)
{
    uint32_t timeout;

    if (NULL != j) {
        /* The gap timeout is configured policy, not stream state: a reset
         * starts a new sequence space, not a new policy. */
        timeout = j->gap_timeout_ms;
        memset(j, 0, sizeof(*j));
        j->gap_timeout_ms = timeout;
    }
}
