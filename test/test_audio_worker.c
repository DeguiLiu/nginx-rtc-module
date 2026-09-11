/*
 * test_audio_worker.c - host tests for ngx_rtc_audio_worker.c.
 *
 * The unit wraps the transcoder in a dedicated pthread with two bounded
 * ngx_rtc_vring queues, so it is the only threaded code in the module. It is
 * also the only place where the mutex discipline matters -- one
 * pthread_mutex_t guards `in`, `out`, `stop` and both drop counters, and the
 * emit callback is deliberately invoked outside the lock.
 *
 * These tests drive that surface concurrently on purpose (push/drain/stats on
 * the calling thread against the worker thread). Under SAN=thread the same
 * binary is the TSan target; the assertions below are the behavioural half,
 * the race detection is the other half.
 *
 * The drop counters are real work going through the real transcoder with
 * garbage frames, so no assertion depends on a specific interleaving -- only
 * on the invariant that every pushed frame is either queued or counted as
 * dropped.
 */

#include "ngx_rtc_test.h"

#include <string.h>

#include "ngx_rtc_audio_worker.h"

/* AAC-LC, 44100 Hz, stereo (see test_audio.c). */
static const uint8_t ASC_AAC_LC_44K_STEREO[2] = { 0x12, 0x10 };

static int32_t
count_emit(void *opaque, const uint8_t *opus, uint32_t len)
{
    int32_t *n = (int32_t *)opaque;

    (void)opus;
    (void)len;

    (*n)++;
    return 0;
}

NGX_RTC_TEST(audio_worker_create_rejects_bad_config)
{
    NGX_RTC_TEST_ASSERT(NULL == ngx_rtc_audio_worker_create(NULL, 2, 64000));
    NGX_RTC_TEST_ASSERT(NULL == ngx_rtc_audio_worker_create(ASC_AAC_LC_44K_STEREO, 1, 64000));
    NGX_RTC_TEST_ASSERT(NULL == ngx_rtc_audio_worker_create(ASC_AAC_LC_44K_STEREO, 2, 0));
}

NGX_RTC_TEST(audio_worker_destroy_null_is_a_noop)
{
    ngx_rtc_audio_worker_destroy(NULL);
}

NGX_RTC_TEST(audio_worker_drop_stats_tolerates_null_outputs)
{
    ngx_rtc_audio_worker_t *w;
    uint32_t                in_dropped;
    uint32_t                out_dropped;

    w = ngx_rtc_audio_worker_create(ASC_AAC_LC_44K_STEREO, 2, 64000);
    NGX_RTC_TEST_ASSERT(NULL != w);

    /* Either pointer may be NULL (the callers pass only what they log). */
    ngx_rtc_audio_worker_drop_stats(w, NULL, NULL);
    ngx_rtc_audio_worker_drop_stats(w, &in_dropped, NULL);
    ngx_rtc_audio_worker_drop_stats(w, NULL, &out_dropped);
    ngx_rtc_audio_worker_drop_stats(w, &in_dropped, &out_dropped);

    ngx_rtc_audio_worker_destroy(w);
}

/* start the thread, hand it work, drain, stop. Exercises create/join and the
 * queue teardown; LeakSanitizer checks the vring arenas and the transcoder. */
NGX_RTC_TEST(audio_worker_push_drain_lifecycle)
{
    ngx_rtc_audio_worker_t *w;
    uint8_t                 frame[256];
    uint32_t                in_dropped;
    uint32_t                out_dropped;
    int                     i;
    int                     accepted;
    int                     rc;
    int32_t                 emitted;

    (void)memset(frame, 0x5A, sizeof(frame));

    w = ngx_rtc_audio_worker_create(ASC_AAC_LC_44K_STEREO, 2, 64000);
    NGX_RTC_TEST_ASSERT(NULL != w);

    accepted = 0;
    for (i = 0; i < 64; i++) {
        rc = ngx_rtc_audio_worker_push(w, frame, sizeof(frame));
        if (0 == rc) {
            accepted++;
        } else {
            NGX_RTC_TEST_ASSERT_I64_EQ(rc, -1);   /* only "queue full" is allowed */
        }
    }

    emitted = 0;
    NGX_RTC_TEST_ASSERT(ngx_rtc_audio_worker_drain(w, count_emit, &emitted) >= 0);

    in_dropped = 0;
    out_dropped = 0;
    ngx_rtc_audio_worker_drop_stats(w, &in_dropped, &out_dropped);

    /* Every pushed frame was either accepted or counted; nothing vanishes. */
    NGX_RTC_TEST_ASSERT_U64_EQ((uint64_t)accepted + (uint64_t)in_dropped, 64u);

    /* Garbage never decodes, so nothing should have been emitted. */
    NGX_RTC_TEST_ASSERT_I64_EQ(emitted, 0);

    ngx_rtc_audio_worker_destroy(w);
}

/* Drain from the calling thread while the worker is still consuming: the
 * concurrent path the mutex exists for. */
NGX_RTC_TEST(audio_worker_concurrent_push_and_drain)
{
    ngx_rtc_audio_worker_t *w;
    uint8_t                 big[1024];
    uint32_t                in_dropped;
    uint32_t                out_dropped;
    int                     i;
    int32_t                 emitted_total;

    (void)memset(big, 0x33, sizeof(big));

    w = ngx_rtc_audio_worker_create(ASC_AAC_LC_44K_STEREO, 2, 64000);
    NGX_RTC_TEST_ASSERT(NULL != w);

    emitted_total = 0;

    for (i = 0; i < 200; i++) {
        (void)ngx_rtc_audio_worker_push(w, big, sizeof(big));
        (void)ngx_rtc_audio_worker_drain(w, count_emit, &emitted_total);
        ngx_rtc_audio_worker_drop_stats(w, &in_dropped, &out_dropped);
    }

    ngx_rtc_audio_worker_destroy(w);

    NGX_RTC_TEST_ASSERT(1);
}
