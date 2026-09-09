/*
 * ngx_rtc_audio_worker.c - thread-isolated AAC -> Opus transcoder.
 *
 * One worker thread owns a stateful ngx_rtc_audio_t and consumes raw AAC frames
 * from an input ring, emitting Opus frames into an output ring. The rings are
 * plain ngx_rtc_ring_t buffers serialised by a single pthread mutex; the wakeup
 * condition variable signals "input available or stop requested". This file
 * uses only C11/POSIX primitives (no nginx API) so the thread never enters
 * nginx data structures.
 */

#include "ngx_rtc_audio_worker.h"

#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "ngx_rtc_ring.h"

/* Bounded queue depth. 16 AAC frames ~= 340 ms at 48 kHz / 1024 samples, far
 * more than enough to absorb transcode jitter without unbounded latency. */
#define NGX_RTC_AUDIO_WORKER_RING_CAP 16u

typedef struct {
    uint32_t len;
    uint8_t  data[NGX_RTC_AUDIO_AAC_MAX];
} ngx_rtc_audio_in_t;

typedef struct {
    uint32_t len;
    uint8_t  data[NGX_RTC_AUDIO_OPUS_MAX_PACKET];
} ngx_rtc_audio_out_t;

struct ngx_rtc_audio_worker_s {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             stop;          /* guarded by lock */

    ngx_rtc_audio_t *a;           /* transcoder, owned by the thread */
    ngx_rtc_ring_t   in;          /* raw AAC frames, worker -> thread */
    ngx_rtc_ring_t   out;         /* Opus frames, thread -> worker */

    uint32_t         in_dropped;   /* guarded by lock */
    uint32_t         out_dropped;  /* guarded by lock */
};

static void    *ngx_rtc_audio_worker_main(void *arg);
static int32_t  ngx_rtc_audio_worker_emit(void *opaque, const uint8_t *opus,
                                          uint32_t len);

ngx_rtc_audio_worker_t *
ngx_rtc_audio_worker_create(const uint8_t *asc, uint32_t asc_len,
                            int32_t bitrate)
{
    ngx_rtc_audio_worker_t *w;
    int                     mutex_ok = 0;
    int                     cond_ok = 0;

    w = calloc(1, sizeof(*w));
    if (NULL == w) {
        return NULL;
    }

    if (pthread_mutex_init(&w->lock, NULL) != 0) {
        goto fail;
    }
    mutex_ok = 1;

    if (pthread_cond_init(&w->cond, NULL) != 0) {
        goto fail;
    }
    cond_ok = 1;

    w->a = ngx_rtc_audio_create(asc, asc_len, bitrate);
    if (NULL == w->a) {
        goto fail;
    }

    if (ngx_rtc_ring_init(&w->in, NGX_RTC_AUDIO_WORKER_RING_CAP,
                          sizeof(ngx_rtc_audio_in_t)) != 0) {
        goto fail;
    }

    if (ngx_rtc_ring_init(&w->out, NGX_RTC_AUDIO_WORKER_RING_CAP,
                          sizeof(ngx_rtc_audio_out_t)) != 0) {
        goto fail;
    }

    if (pthread_create(&w->thread, NULL, ngx_rtc_audio_worker_main, w) != 0) {
        goto fail;
    }

    return w;

fail:
    if (NULL != w->a) {
        ngx_rtc_audio_destroy(w->a);
    }
    if (NULL != w->out.buf) {
        ngx_rtc_ring_destroy(&w->out);
    }
    if (NULL != w->in.buf) {
        ngx_rtc_ring_destroy(&w->in);
    }
    if (cond_ok) {
        pthread_cond_destroy(&w->cond);
    }
    if (mutex_ok) {
        pthread_mutex_destroy(&w->lock);
    }
    free(w);
    return NULL;
}

void
ngx_rtc_audio_worker_destroy(ngx_rtc_audio_worker_t *w)
{
    if (NULL == w) {
        return;
    }

    pthread_mutex_lock(&w->lock);
    w->stop = 1;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);

    pthread_join(w->thread, NULL);

    ngx_rtc_audio_destroy(w->a);
    ngx_rtc_ring_destroy(&w->in);
    ngx_rtc_ring_destroy(&w->out);
    pthread_cond_destroy(&w->cond);
    pthread_mutex_destroy(&w->lock);

    free(w);
}

int
ngx_rtc_audio_worker_push(ngx_rtc_audio_worker_t *w, const uint8_t *aac,
                          uint32_t len)
{
    ngx_rtc_audio_in_t in;
    int                rc;

    if (NULL == w || NULL == aac || 0 == len
            || len > NGX_RTC_AUDIO_AAC_MAX) {
        return -1;
    }

    in.len = len;
    memcpy(in.data, aac, len);

    pthread_mutex_lock(&w->lock);
    rc = ngx_rtc_ring_push(&w->in, &in);
    if (0 == rc) {
        pthread_cond_signal(&w->cond);
    } else {
        w->in_dropped++;
    }
    pthread_mutex_unlock(&w->lock);

    return rc;
}

int
ngx_rtc_audio_worker_drain(ngx_rtc_audio_worker_t *w,
                           ngx_rtc_audio_frame_fn emit, void *opaque)
{
    ngx_rtc_audio_out_t out;
    int                 drained;

    if (NULL == w || NULL == emit) {
        return 0;
    }

    drained = 0;
    for (;;) {
        pthread_mutex_lock(&w->lock);
        {
            int ok = ngx_rtc_ring_pop(&w->out, &out);
            if (ok != 0) {
                pthread_mutex_unlock(&w->lock);
                break;
            }
        }
        pthread_mutex_unlock(&w->lock);

        (void)emit(opaque, out.data, out.len);
        drained++;
    }

    return drained;
}


void
ngx_rtc_audio_worker_drop_stats(ngx_rtc_audio_worker_t *w,
                                uint32_t *in_dropped, uint32_t *out_dropped)
{
    if (NULL == w) {
        return;
    }

    pthread_mutex_lock(&w->lock);
    if (NULL != in_dropped) {
        *in_dropped = w->in_dropped;
    }
    if (NULL != out_dropped) {
        *out_dropped = w->out_dropped;
    }
    pthread_mutex_unlock(&w->lock);
}

static void *
ngx_rtc_audio_worker_main(void *arg)
{
    ngx_rtc_audio_worker_t *w = arg;
    ngx_rtc_audio_in_t      in;

    for (;;) {
        pthread_mutex_lock(&w->lock);
        while (!w->stop && ngx_rtc_ring_empty(&w->in)) {
            pthread_cond_wait(&w->cond, &w->lock);
        }

        if (w->stop) {
            pthread_mutex_unlock(&w->lock);
            break;
        }

        (void)ngx_rtc_ring_pop(&w->in, &in);
        pthread_mutex_unlock(&w->lock);

        (void)ngx_rtc_audio_transcode(w->a, in.data, in.len,
                                      ngx_rtc_audio_worker_emit, w);
    }

    return NULL;
}

static int32_t
ngx_rtc_audio_worker_emit(void *opaque, const uint8_t *opus, uint32_t len)
{
    ngx_rtc_audio_worker_t *w = opaque;
    ngx_rtc_audio_out_t     out;

    if (NULL == w || NULL == opus || 0 == len
            || len > NGX_RTC_AUDIO_OPUS_MAX_PACKET) {
        return -1;
    }

    out.len = len;
    memcpy(out.data, opus, len);

    pthread_mutex_lock(&w->lock);
    if (ngx_rtc_ring_push(&w->out, &out) != 0) {
        w->out_dropped++;
    }
    pthread_mutex_unlock(&w->lock);

    return 0;
}
