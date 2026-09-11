/*
 * ngx_rtc_jitter.h - WHIP uplink reorder buffer (pure C11).
 *
 * The WHIP media path receives SRTP-decrypted RTP directly from the socket and
 * would otherwise broadcast packets in arrival order. On a lossy WAN this can
 * deliver H264 out of sequence and corrupt the decoder. This buffer caches
 * video RTP by sequence number and emits them in order; a gap older than
 * NGX_RTC_JITTER_TIMEOUT_MS is skipped (the missing packets are dropped) so a
 * single lost packet cannot stall the uplink forever.
 *
 * The slot array is fixed-size (no heap allocation), so it is safe to embed in
 * the per-process source struct and independent of the nginx pool.
 *
 * Audio deliberately bypasses this buffer (see ngx_rtc_stream_on_srtp): an Opus
 * packet is a whole frame, so losing one costs 20 ms that the decoder conceals,
 * while holding it back would add the same 20 ms of latency to every packet for
 * a reordering case that is rare on a 50 pps flow. The asymmetry is intentional,
 * not an oversight: it bounds the uplink A/V skew at NGX_RTC_JITTER_TIMEOUT_MS
 * (only while a gap is actually open, and the video-side drain is what closes
 * it), well inside the avsync component's tolerance.
 */

#ifndef NGX_RTC_JITTER_H
#define NGX_RTC_JITTER_H

#include <stdint.h>

#include "ngx_rtc_rtp.h"

/* Reorder window: power of two, indexed by seq & (capacity - 1). 128 packets
 * is several frames of 1080p H264 and far larger than typical WAN reordering. */
#define NGX_RTC_JITTER_CAP  128u

/* Gap timeout: how long to wait for a missing packet before skipping it. 50 ms
 * tolerates WAN reordering without adding perceptible uplink latency. */
#define NGX_RTC_JITTER_TIMEOUT_MS  50u

typedef struct {
    uint8_t  data[NGX_RTC_MAX_RTP_PKT];
    uint16_t len;
    uint8_t  is_gop_start;
    uint8_t  valid;      /* 1 = slot holds a packet awaiting its sequence turn */
} ngx_rtc_jitter_slot_t;

typedef struct {
    ngx_rtc_jitter_slot_t slots[NGX_RTC_JITTER_CAP];
    uint16_t next_seq;    /* next expected sequence (mod 2^16) */
    uint16_t pending;     /* cached packets still ahead of next_seq */
    uint8_t  initialized; /* 1 once next_seq is anchored */
    uint32_t gap_ts;      /* monotonic ms when the oldest open gap was first seen;
                           * held until the buffer drains, so an in-order fill of
                           * one gap cannot restart the timeout of another */
    uint32_t gap_timeout_ms; /* how long an open gap may hold the queue before it
                           * is skipped. 0 = use NGX_RTC_JITTER_TIMEOUT_MS; the
                           * nginx side sets it from rtc_jitter_timeout. Lives
                           * here rather than being read from the core config so
                           * this unit stays free of the nginx conf path (and of
                           * the crypto headers core.h pulls in). */
    /* Diagnostic counters (never reset except by a full reset). */
    uint32_t n_cached;    /* packets held for reordering */
    uint32_t n_skipped;   /* packets dropped by the gap timeout */
    uint32_t n_stale;     /* late duplicates dropped */
    uint32_t n_reanchor;  /* far-ahead re-anchors */
} ngx_rtc_jitter_t;

typedef int32_t (*ngx_rtc_jitter_emit_fn)(void *opaque, const uint8_t *rtp,
                                          uint32_t len, uint8_t is_gop_start);

/* Insert one video RTP packet and emit any now-in-order packets via cb.
 * `now` is a monotonic millisecond clock used only for gap timeout. Returns
 * NGX_RTC_OK, or a negative error on bad arguments. */
int32_t ngx_rtc_jitter_push(ngx_rtc_jitter_t *j, const uint8_t *rtp,
                            uint32_t len, uint16_t seq, uint8_t is_gop_start,
                            uint32_t now, ngx_rtc_jitter_emit_fn cb,
                            void *opaque);

/* Drop all cached packets and reset the sequence expectation. Call on WHIP
 * republish so stale packets from the previous generation cannot leak out.
 * gap_timeout_ms is a policy, not stream state, so it survives the reset. */
void ngx_rtc_jitter_reset(ngx_rtc_jitter_t *j);

#endif /* NGX_RTC_JITTER_H */
