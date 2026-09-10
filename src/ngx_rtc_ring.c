/*
 * ngx_rtc_ring.c - bounded fixed-size ring buffer (see ngx_rtc_ring.h).
 */

#include "ngx_rtc_ring.h"

#include <stdlib.h>
#include <string.h>

static uint32_t
ngx_rtc_ring_next_pow2(uint32_t v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

int
ngx_rtc_ring_init(ngx_rtc_ring_t *r, uint32_t capacity, uint32_t elem_size)
{
    if (NULL == r || 0 == capacity || 0 == elem_size) {
        return -1;
    }

    capacity = ngx_rtc_ring_next_pow2(capacity);

    r->buf = calloc(capacity, elem_size);
    if (NULL == r->buf) {
        return -1;
    }

    r->capacity = capacity;
    r->elem_size = elem_size;
    r->head = 0;
    r->tail = 0;

    return 0;
}

void
ngx_rtc_ring_destroy(ngx_rtc_ring_t *r)
{
    if (NULL == r) {
        return;
    }

    free(r->buf);
    r->buf = NULL;
    r->capacity = 0;
    r->elem_size = 0;
    r->head = 0;
    r->tail = 0;
}

int
ngx_rtc_ring_full(const ngx_rtc_ring_t *r)
{
    if (NULL == r || NULL == r->buf) {
        return 1;
    }

    return (r->head - r->tail) >= r->capacity;
}

int
ngx_rtc_ring_empty(const ngx_rtc_ring_t *r)
{
    if (NULL == r || NULL == r->buf) {
        return 1;
    }

    return r->head == r->tail;
}

uint32_t
ngx_rtc_ring_count(const ngx_rtc_ring_t *r)
{
    if (NULL == r || NULL == r->buf) {
        return 0;
    }

    return r->head - r->tail;
}

int
ngx_rtc_ring_push(ngx_rtc_ring_t *r, const void *elem)
{
    uint32_t idx;

    if (NULL == r || NULL == r->buf || NULL == elem || ngx_rtc_ring_full(r)) {
        return -1;
    }

    idx = (r->head & (r->capacity - 1u)) * r->elem_size;
    memcpy(r->buf + idx, elem, r->elem_size);
    r->head++;

    return 0;
}

int
ngx_rtc_ring_pop(ngx_rtc_ring_t *r, void *elem)
{
    uint32_t idx;

    if (NULL == r || NULL == r->buf || NULL == elem || ngx_rtc_ring_empty(r)) {
        return -1;
    }

    idx = (r->tail & (r->capacity - 1u)) * r->elem_size;
    memcpy(elem, r->buf + idx, r->elem_size);
    r->tail++;

    return 0;
}

/* ---- variable-length byte ring ---- */

static void
ngx_rtc_vring_write_at(uint8_t *buf, uint32_t capacity, uint32_t off,
                       const void *src, uint32_t n)
{
    uint32_t first;

    first = capacity - off;
    if (first > n) {
        first = n;
    }
    memcpy(buf + off, src, first);
    if (first < n) {
        memcpy(buf, (const uint8_t *) src + first, n - first);
    }
}

static void
ngx_rtc_vring_read_at(const uint8_t *buf, uint32_t capacity, uint32_t off,
                      void *dst, uint32_t n)
{
    uint32_t first;

    first = capacity - off;
    if (first > n) {
        first = n;
    }
    memcpy(dst, buf + off, first);
    if (first < n) {
        memcpy((uint8_t *) dst + first, buf, n - first);
    }
}

int
ngx_rtc_vring_init(ngx_rtc_vring_t *r, uint32_t capacity)
{
    if (NULL == r || 0 == capacity) {
        return -1;
    }

    r->buf = calloc(capacity, 1u);
    if (NULL == r->buf) {
        return -1;
    }

    r->capacity = capacity;
    r->head = 0;
    r->tail = 0;
    r->bytes = 0;

    return 0;
}

void
ngx_rtc_vring_destroy(ngx_rtc_vring_t *r)
{
    if (NULL == r) {
        return;
    }

    free(r->buf);
    r->buf = NULL;
    r->capacity = 0;
    r->head = 0;
    r->tail = 0;
    r->bytes = 0;
}

int
ngx_rtc_vring_empty(const ngx_rtc_vring_t *r)
{
    if (NULL == r || NULL == r->buf) {
        return 1;
    }

    return r->bytes == 0;
}

int
ngx_rtc_vring_push(ngx_rtc_vring_t *r, const void *data, uint32_t len)
{
    uint32_t total;

    if (NULL == r || NULL == r->buf || NULL == data || 0 == len) {
        return -1;
    }

    total = 4u + len;
    if (r->bytes + total > r->capacity) {
        return -1; /* full */
    }

    ngx_rtc_vring_write_at(r->buf, r->capacity, r->head, &len, 4u);
    ngx_rtc_vring_write_at(r->buf, r->capacity,
                           (r->head + 4u) % r->capacity, data, len);

    r->head = (r->head + total) % r->capacity;
    r->bytes += total;

    return 0;
}

int
ngx_rtc_vring_pop(ngx_rtc_vring_t *r, void *out, uint32_t out_cap,
                  uint32_t *out_len)
{
    uint32_t len;
    uint32_t total;

    if (NULL == r || NULL == r->buf || NULL == out || NULL == out_len
            || 0 == r->bytes) {
        return -1;
    }

    ngx_rtc_vring_read_at(r->buf, r->capacity, r->tail, &len, 4u);
    if (len > out_cap) {
        return -1; /* caller buffer too small; entry stays */
    }

    ngx_rtc_vring_read_at(r->buf, r->capacity,
                          (r->tail + 4u) % r->capacity, out, len);
    *out_len = len;

    total = 4u + len;
    r->tail = (r->tail + total) % r->capacity;
    r->bytes -= total;

    return 0;
}
