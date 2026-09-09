/*
 * ngx_rtc_audio.h - AAC -> Opus audio transcoder (pure C11).
 *
 * Translation of SRS 6.0 SrsAudioTranscoder:
 *   src/app/srs_app_rtc_codec.cpp  (initialize / transcode / decode_and_resample / encode)
 *
 * Pipeline: raw AAC frame (AACPacketType=1 payload, no ADTS header) is decoded
 * by the FFmpeg native AAC decoder with the AudioSpecificConfig as extradata.
 * The decoder emits planar float (FLTP); libswresample converts it to
 * interleaved S16 at 48kHz stereo (the WebRTC Opus profile), and native
 * libopus encodes it into Opus packets. One AAC frame may produce zero or
 * more Opus packets, delivered through an emit callback.
 *
 * No global state: the caller owns the handle and all input/output buffers.
 */

#ifndef NGX_RTC_AUDIO_H
#define NGX_RTC_AUDIO_H

#include <stdint.h>

typedef struct ngx_rtc_audio_s ngx_rtc_audio_t;

/* Opus output profile (matches the WebRTC "opus/48000/2" offer). */
#define NGX_RTC_AUDIO_OPUS_RATE       48000u
#define NGX_RTC_AUDIO_OPUS_CHANNELS   2u
#define NGX_RTC_AUDIO_OPUS_FRAME_SIZE 960u /* 20ms @ 48kHz, libopus default */

/* The interleaved-S16 FIFO is pre-sized once to hold a few Opus frames so the
 * hot transcode path never calls av_audio_fifo_realloc per AAC frame. The
 * decode loop drains the FIFO to below one frame before writing the next, so
 * 4 frames of headroom is a safe fixed bound. */
#define NGX_RTC_AUDIO_FIFO_MAX_SAMPLES (NGX_RTC_AUDIO_OPUS_FRAME_SIZE * 4u)

/* Upper bound of one raw AAC frame payload (FLV AACPacketType=1). */
#define NGX_RTC_AUDIO_AAC_MAX         8192u

/* Upper bound of one encoded Opus frame (well below RFC 6716 max size). */
#define NGX_RTC_AUDIO_OPUS_MAX_PACKET 4000u

/* Callback receives one encoded Opus frame (raw payload, no RTP header). */
typedef int32_t (*ngx_rtc_audio_frame_fn)(void *opaque, const uint8_t *opus,
                                          uint32_t len);

/*
 * Create the transcoder. asc/asc_len is the FLV AAC sequence header
 * (AudioSpecificConfig, at least 2 bytes), used as the AAC decoder extradata.
 * bitrate is the Opus target bitrate in bps (e.g. 64000). Returns NULL on
 * any allocation / codec initialization failure.
 */
ngx_rtc_audio_t *ngx_rtc_audio_create(const uint8_t *asc, uint32_t asc_len,
                                      int32_t bitrate);

/*
 * Transcode one raw AAC frame into zero or more Opus frames delivered through
 * emit. Returns 0 on success (including "no output yet"), negative on error.
 * The returned value of emit is propagated when it is negative.
 */
int32_t ngx_rtc_audio_transcode(ngx_rtc_audio_t *a, const uint8_t *aac,
                                uint32_t aac_len, ngx_rtc_audio_frame_fn emit,
                                void *opaque);

/* Release the transcoder and everything it owns. NULL is a no-op. */
void ngx_rtc_audio_destroy(ngx_rtc_audio_t *a);

#endif /* NGX_RTC_AUDIO_H */
