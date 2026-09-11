/*
 * test_audio.c - host tests for ngx_rtc_audio.c against the real FFmpeg + libopus.
 *
 * The unit is in every build of the module (config:10-16) but had never been in
 * a test build, so its codec setup (avcodec_open2 / swr_init / opus_encoder_create)
 * and teardown ran only in production.
 *
 * Scope, stated honestly: the round trip creates and destroys a real decoder,
 * resampler and encoder, which is where the ownership is (LeakSanitizer checks
 * every one of them is released). The transcode path is exercised only up to
 * its guards -- a genuine AAC frame would have to come from an encoder, and
 * feeding garbage gets a decoder error, which is itself a path worth having.
 */

#include "ngx_rtc_test.h"

#include <string.h>

#include "ngx_rtc_audio.h"

/* AAC-LC, 44100 Hz, stereo: 5-bit object type 2, 4-bit sample-rate index 4,
 * 4-bit channel config 2 -- the canonical 2-byte AudioSpecificConfig. */
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

NGX_RTC_TEST(audio_create_rejects_bad_config)
{
    NGX_RTC_TEST_ASSERT(NULL == ngx_rtc_audio_create(NULL, 2, 64000));
    NGX_RTC_TEST_ASSERT(NULL == ngx_rtc_audio_create(ASC_AAC_LC_44K_STEREO, 1, 64000));
    NGX_RTC_TEST_ASSERT(NULL == ngx_rtc_audio_create(ASC_AAC_LC_44K_STEREO, 2, 0));
    NGX_RTC_TEST_ASSERT(NULL == ngx_rtc_audio_create(ASC_AAC_LC_44K_STEREO, 2, -1));
}

NGX_RTC_TEST(audio_destroy_null_is_a_noop)
{
    ngx_rtc_audio_destroy(NULL);
}

/* Builds the real decoder + resampler + encoder and tears them down again.
 * Nothing here asserts behaviour beyond "it was created"; the value is that
 * LeakSanitizer sees whether every libav* and libopus object was released. */
NGX_RTC_TEST(audio_create_destroy_round_trip)
{
    ngx_rtc_audio_t *a;

    a = ngx_rtc_audio_create(ASC_AAC_LC_44K_STEREO, 2, 64000);
    NGX_RTC_TEST_ASSERT(NULL != a);

    ngx_rtc_audio_destroy(a);
}

NGX_RTC_TEST(audio_transcode_guards)
{
    ngx_rtc_audio_t *a;
    uint8_t          frame[64];

    (void)memset(frame, 0, sizeof(frame));

    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_audio_transcode(NULL, frame, sizeof(frame), count_emit, NULL), -1);

    a = ngx_rtc_audio_create(ASC_AAC_LC_44K_STEREO, 2, 64000);
    NGX_RTC_TEST_ASSERT(NULL != a);

    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_audio_transcode(a, NULL, sizeof(frame), count_emit, NULL), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_audio_transcode(a, frame, 0, count_emit, NULL), -1);

    /* NGX_RTC_AUDIO_AAC_MAX is 8192; one past it must be refused before the
     * length is used to touch anything. The buffer really is 64 bytes, so a
     * missing guard would be an out-of-bounds read here. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_audio_transcode(a, frame, 8193, count_emit, NULL), -1);

    ngx_rtc_audio_destroy(a);
}

/* A well-sized but meaningless frame: the decoder must reject it without
 * crashing or emitting a frame. */
NGX_RTC_TEST(audio_transcode_rejects_garbage_frame)
{
    ngx_rtc_audio_t *a;
    uint8_t          frame[512];
    int32_t          emitted;
    int32_t          rc;

    (void)memset(frame, 0xAB, sizeof(frame));

    a = ngx_rtc_audio_create(ASC_AAC_LC_44K_STEREO, 2, 64000);
    NGX_RTC_TEST_ASSERT(NULL != a);

    emitted = 0;
    rc = ngx_rtc_audio_transcode(a, frame, sizeof(frame), count_emit, &emitted);

    NGX_RTC_TEST_ASSERT(0 == rc || -1 == rc);
    NGX_RTC_TEST_ASSERT_I64_EQ(emitted, 0);   /* nothing decodable in there */

    ngx_rtc_audio_destroy(a);
}
