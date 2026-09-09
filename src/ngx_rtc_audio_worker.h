/*
 * ngx_rtc_audio_worker.h - thread-isolated AAC -> Opus transcoder.
 *
 * Wraps the pure ngx_rtc_audio_t transcoder with a dedicated pthread so the
 * FFmpeg decode / resample / Opus encode never blocks the nginx worker event
 * loop. The nginx worker pushes raw AAC frames and drains ready Opus frames;
 * all nginx-side work (packetization, SRTP, send) stays in the worker.
 */

#ifndef NGX_RTC_AUDIO_WORKER_H
#define NGX_RTC_AUDIO_WORKER_H

#include "ngx_rtc_audio.h"

typedef struct ngx_rtc_audio_worker_s ngx_rtc_audio_worker_t;

/* Create the worker and its transcoder thread. asc/asc_len is the AAC
 * AudioSpecificConfig; bitrate is the Opus target bitrate. Returns NULL on any
 * allocation / codec / thread failure. */
ngx_rtc_audio_worker_t *ngx_rtc_audio_worker_create(const uint8_t *asc,
                                                    uint32_t asc_len,
                                                    int32_t bitrate);

/* Stop and join the thread, then release the transcoder and queues. */
void ngx_rtc_audio_worker_destroy(ngx_rtc_audio_worker_t *w);

/* Enqueue one raw AAC frame (copy). 0 on success, -1 when the bounded queue is
 * full (frame dropped). */
int ngx_rtc_audio_worker_push(ngx_rtc_audio_worker_t *w, const uint8_t *aac,
                              uint32_t len);

/* Drain every ready Opus frame through emit(opaque, opus, len). Returns the
 * number of frames delivered. */
int ngx_rtc_audio_worker_drain(ngx_rtc_audio_worker_t *w,
                               ngx_rtc_audio_frame_fn emit, void *opaque);

/* Snapshot the cumulative in/out queue drop counters (either pointer may be
 * NULL). Used by the bridge observability log to expose AAC/Opus drops. */
void ngx_rtc_audio_worker_drop_stats(ngx_rtc_audio_worker_t *w,
                                     uint32_t *in_dropped,
                                     uint32_t *out_dropped);

#endif /* NGX_RTC_AUDIO_WORKER_H */
