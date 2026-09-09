/*
 * ngx_rtc_ring.h - bounded single-consumer fixed-size ring buffer (pure C11).
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

#endif /* NGX_RTC_RING_H */
