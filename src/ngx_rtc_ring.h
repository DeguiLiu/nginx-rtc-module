/**
 * @file    ngx_rtc_ring.h
 * @brief   Bounded single-consumer fixed-size ring buffer (pure C11).
 * @version 0.5.0
 * @license MIT, see LICENSE
 *
 * Stores capacity fixed-size elements and exposes FIFO push/pop with monotonic
 * head/tail counters. It is not internally synchronised: a caller that shares a
 * ring between threads must serialise access itself (the audio worker guards it
 * with a pthread mutex). The buffer is malloc'd by init and released by
 * destroy; it is independent of the nginx pool so a worker thread may use it.
 */

#ifndef NGX_RTC_RING_H
#define NGX_RTC_RING_H

#include <stdint.h>

typedef struct {
    uint8_t *buf;        /* capacity * elem_size bytes */
    uint32_t capacity;   /* power of two, number of slots */
    uint32_t elem_size;  /* bytes per slot */
    uint32_t head;       /* next write index (monotonic) */
    uint32_t tail;       /* next read index (monotonic) */
} ngx_rtc_ring_t;

/* Initialise a ring. capacity is rounded up to the next power of two.
 * Returns 0 on success, -1 on invalid arguments or allocation failure. */
int ngx_rtc_ring_init(ngx_rtc_ring_t *r, uint32_t capacity, uint32_t elem_size);

/* Release the backing buffer. Safe on a zeroed or already-destroyed ring. */
void ngx_rtc_ring_destroy(ngx_rtc_ring_t *r);

/* Push one element (copies elem_size bytes). 0 on success, -1 when full. */
int ngx_rtc_ring_push(ngx_rtc_ring_t *r, const void *elem);

/* Pop one element into elem (must hold elem_size bytes). 0 on success,
 * -1 when empty. */
int ngx_rtc_ring_pop(ngx_rtc_ring_t *r, void *elem);

int ngx_rtc_ring_full(const ngx_rtc_ring_t *r);
int ngx_rtc_ring_empty(const ngx_rtc_ring_t *r);
uint32_t ngx_rtc_ring_count(const ngx_rtc_ring_t *r);

/* Variable-length byte ring: each entry is [uint32_t len][len data bytes].
 * The arena holds only live bytes, so memory is proportional to actual payload
 * (no per-slot MAX padding). Not internally synchronised; the caller serialises
 * access (the audio worker guards it with a pthread mutex). */
typedef struct {
    uint8_t *buf;        /* capacity bytes */
    uint32_t capacity;   /* total bytes */
    uint32_t head;       /* next write byte offset */
    uint32_t tail;       /* next read byte offset */
    uint32_t bytes;      /* live bytes, including the 4-byte length prefixes */
} ngx_rtc_vring_t;

/* Initialise a byte arena of `capacity` bytes. Returns 0 / -1. */
int ngx_rtc_vring_init(ngx_rtc_vring_t *r, uint32_t capacity);

/* Release the backing buffer. Safe on a zeroed / already-destroyed ring. */
void ngx_rtc_vring_destroy(ngx_rtc_vring_t *r);

/* Append one variable-length entry. 0 on success, -1 when full or bad args. */
int ngx_rtc_vring_push(ngx_rtc_vring_t *r, const void *data, uint32_t len);

/* Remove the oldest entry into `out` (capacity `out_cap`); *out_len is set to
 * the entry length. 0 on success, -1 when empty or out_cap < len. */
int ngx_rtc_vring_pop(ngx_rtc_vring_t *r, void *out, uint32_t out_cap,
                      uint32_t *out_len);

int ngx_rtc_vring_empty(const ngx_rtc_vring_t *r);

#endif /* NGX_RTC_RING_H */
