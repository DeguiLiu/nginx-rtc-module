/*
 * ngx_rtc_audio.c - AAC -> Opus audio transcoder (pure C11).
 *
 * See ngx_rtc_audio.h for the pipeline. The FFmpeg side follows SRS 6.0
 * SrsAudioTranscoder (init_dec / init_enc / init_swr / init_fifo /
 * decode_and_resample / encode); the Opus encoder is native libopus, so the
 * resampler output format is fixed to interleaved S16.
 */

#include "ngx_rtc_audio.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <opus/opus.h>

struct ngx_rtc_audio_s {
    AVCodecContext *dec;
    AVFrame        *dec_frame;
    AVPacket       *dec_pkt;

    OpusEncoder    *enc;

    SwrContext     *swr;
    uint8_t        *swr_data[NGX_RTC_AUDIO_OPUS_CHANNELS];
    AVAudioFifo    *fifo;

    uint8_t         swr_ready;
    uint8_t        *pcm_buf;   /* interleaved S16 input of one Opus frame */
    uint8_t         opus_buf[NGX_RTC_AUDIO_OPUS_MAX_PACKET];
    uint8_t         aac_buf[NGX_RTC_AUDIO_AAC_MAX + AV_INPUT_BUFFER_PADDING_SIZE];
};

static int32_t ngx_rtc_audio_init_swr(ngx_rtc_audio_t *a);
static int32_t ngx_rtc_audio_fifo_write(ngx_rtc_audio_t *a, int32_t nb_samples);
static int32_t ngx_rtc_audio_pump(ngx_rtc_audio_t *a);
static int32_t ngx_rtc_audio_encode(ngx_rtc_audio_t *a,
                                    ngx_rtc_audio_frame_fn emit, void *opaque);

ngx_rtc_audio_t *
ngx_rtc_audio_create(const uint8_t *asc, uint32_t asc_len, int32_t bitrate)
{
    ngx_rtc_audio_t *a;
    const AVCodec   *codec;
    int              err;

    if ((NULL == asc) || (asc_len < 2u) || (bitrate <= 0)) {
        return NULL;
    }

    a = calloc(1, sizeof(*a));
    if (NULL == a) {
        return NULL;
    }

    /* AAC decoder: AudioSpecificConfig is the raw-AAC extradata. */
    codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (NULL == codec) {
        goto fail;
    }
    a->dec = avcodec_alloc_context3(codec);
    if (NULL == a->dec) {
        goto fail;
    }

    a->dec->extradata = av_mallocz((size_t)asc_len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (NULL == a->dec->extradata) {
        goto fail;
    }
    memcpy(a->dec->extradata, asc, asc_len);
    a->dec->extradata_size = (int)asc_len;

    if (avcodec_open2(a->dec, codec, NULL) < 0) {
        goto fail;
    }
#if LIBAVCODEC_VERSION_MAJOR < 59
    if (0 == a->dec->channel_layout) {
        a->dec->channel_layout = (uint64_t)av_get_default_channel_layout(a->dec->channels);
    }
#endif

    a->dec_frame = av_frame_alloc();
    a->dec_pkt = av_packet_alloc();
    if ((NULL == a->dec_frame) || (NULL == a->dec_pkt)) {
        goto fail;
    }

    a->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_S16,
                                  (int)NGX_RTC_AUDIO_OPUS_CHANNELS,
                                  (int)NGX_RTC_AUDIO_FIFO_MAX_SAMPLES);
    if (NULL == a->fifo) {
        goto fail;
    }

    a->pcm_buf = av_malloc((size_t)NGX_RTC_AUDIO_OPUS_FRAME_SIZE
                           * NGX_RTC_AUDIO_OPUS_CHANNELS * 2u);
    if (NULL == a->pcm_buf) {
        goto fail;
    }

    /* Native libopus encoder, 48kHz stereo (WebRTC profile). */
    a->enc = opus_encoder_create((opus_int32)NGX_RTC_AUDIO_OPUS_RATE,
                                 (int)NGX_RTC_AUDIO_OPUS_CHANNELS,
                                 OPUS_APPLICATION_AUDIO, &err);
    if ((NULL == a->enc) || (OPUS_OK != err)) {
        goto fail;
    }
    (void)opus_encoder_ctl(a->enc, OPUS_SET_BITRATE(bitrate));
    (void)opus_encoder_ctl(a->enc, OPUS_SET_COMPLEXITY(1));

    return a;

fail:
    ngx_rtc_audio_destroy(a);
    return NULL;
}

void
ngx_rtc_audio_destroy(ngx_rtc_audio_t *a)
{
    if (NULL == a) {
        return;
    }

    if (NULL != a->enc) {
        opus_encoder_destroy(a->enc);
        a->enc = NULL;
    }
    if (NULL != a->fifo) {
        av_audio_fifo_free(a->fifo);
        a->fifo = NULL;
    }
    if (NULL != a->swr_data[0]) {
        av_freep(&a->swr_data[0]);
    }
    if (NULL != a->swr) {
        swr_free(&a->swr);
    }
    if (NULL != a->pcm_buf) {
        av_free(a->pcm_buf);
        a->pcm_buf = NULL;
    }
    if (NULL != a->dec_pkt) {
        av_packet_free(&a->dec_pkt);
    }
    if (NULL != a->dec_frame) {
        av_frame_free(&a->dec_frame);
    }
    if (NULL != a->dec) {
        avcodec_free_context(&a->dec);
    }

    free(a);
}

int32_t
ngx_rtc_audio_transcode(ngx_rtc_audio_t *a, const uint8_t *aac, uint32_t aac_len,
                        ngx_rtc_audio_frame_fn emit, void *opaque)
{
    int32_t ret;

    if ((NULL == a) || (NULL == aac) || (0 == aac_len) || (NULL == emit)) {
        return -1;
    }
    if (aac_len > NGX_RTC_AUDIO_AAC_MAX) {
        return -1;
    }

    /*
     * Decoders may read past the packet by AV_INPUT_BUFFER_PADDING_SIZE bytes,
     * so copy the RTMP payload into a zero-padded buffer first.
     */
    memcpy(a->aac_buf, aac, aac_len);
    memset(a->aac_buf + aac_len, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    a->dec_pkt->data = a->aac_buf;
    a->dec_pkt->size = (int)aac_len;

    ret = avcodec_send_packet(a->dec, a->dec_pkt);
    if (ret < 0) {
        return -1;
    }

    for (;;) {
        ret = avcodec_receive_frame(a->dec, a->dec_frame);
        if ((AVERROR(EAGAIN) == ret) || (AVERROR_EOF == ret)) {
            break;
        }
        if (ret < 0) {
            return -1;
        }

        /* The decoder output format is only known after the first frame. */
        if (0 == a->swr_ready) {
            if (ngx_rtc_audio_init_swr(a) < 0) {
                return -1;
            }
        }

        if (ngx_rtc_audio_pump(a) < 0) {
            return -1;
        }
    }

    return ngx_rtc_audio_encode(a, emit, opaque);
}

static int32_t
ngx_rtc_audio_init_swr(ngx_rtc_audio_t *a)
{
#if LIBAVCODEC_VERSION_MAJOR >= 59
    AVChannelLayout in_layout;
    AVChannelLayout out_layout;
    int             rc;

    in_layout = a->dec->ch_layout;
    av_channel_layout_default(&out_layout, (int)NGX_RTC_AUDIO_OPUS_CHANNELS);

    rc = swr_alloc_set_opts2(&a->swr,
                             &out_layout,
                             AV_SAMPLE_FMT_S16, (int)NGX_RTC_AUDIO_OPUS_RATE,
                             &in_layout, a->dec->sample_fmt, a->dec->sample_rate,
                             0, NULL);
    av_channel_layout_uninit(&out_layout);
    if (rc < 0) {
        return -1;
    }
#else
    uint64_t in_layout;

    in_layout = a->dec->channel_layout;
    if (0 == in_layout) {
        in_layout = (uint64_t)av_get_default_channel_layout(a->dec->channels);
    }

    /* FLTP (decoder) -> interleaved S16 48kHz stereo (libopus). */
    a->swr = swr_alloc_set_opts(NULL,
                                av_get_default_channel_layout((int)NGX_RTC_AUDIO_OPUS_CHANNELS),
                                AV_SAMPLE_FMT_S16, (int)NGX_RTC_AUDIO_OPUS_RATE,
                                (int64_t)in_layout, a->dec->sample_fmt, a->dec->sample_rate,
                                0, NULL);
    if (NULL == a->swr) {
        return -1;
    }
#endif
    if (swr_init(a->swr) < 0) {
        /* Must release before returning. swr_ready stays 0, so the next frame
         * calls this function again and swr_alloc_set_opts2() overwrites
         * a->swr -- the context allocated above would be lost for good. The
         * only other free is in ngx_rtc_audio_destroy(), which sees just the
         * last pointer. */
        swr_free(&a->swr);
        return -1;
    }

    memset(a->swr_data, 0, sizeof(a->swr_data));
    if (av_samples_alloc(a->swr_data, NULL, (int)NGX_RTC_AUDIO_OPUS_CHANNELS,
                         (int)NGX_RTC_AUDIO_OPUS_FRAME_SIZE,
                         AV_SAMPLE_FMT_S16, 0) < 0) {
        swr_free(&a->swr);
        return -1;
    }

    a->swr_ready = 1;
    return 0;
}

/* Resample one decoded frame (FLTP) and push the S16 samples into the FIFO. */
static int32_t
ngx_rtc_audio_pump(ngx_rtc_audio_t *a)
{
    const uint8_t **in_data;
    int32_t         in_samples;
    int32_t         out_samples;

    in_data = (const uint8_t **)a->dec_frame->extended_data;
    in_samples = a->dec_frame->nb_samples;

    do {
        out_samples = swr_convert(a->swr, a->swr_data,
                                  (int)NGX_RTC_AUDIO_OPUS_FRAME_SIZE,
                                  in_data, in_samples);
        if (out_samples < 0) {
            return -1;
        }

        in_data = NULL;
        in_samples = 0;

        if (out_samples > 0) {
            if (ngx_rtc_audio_fifo_write(a, out_samples) < 0) {
                return -1;
            }
        }
    } while (swr_get_out_samples(a->swr, 0) >= (int)NGX_RTC_AUDIO_OPUS_FRAME_SIZE);

    return 0;
}

static int32_t
ngx_rtc_audio_fifo_write(ngx_rtc_audio_t *a, int32_t nb_samples)
{
    /* The FIFO is pre-sized once (NGX_RTC_AUDIO_FIFO_MAX_SAMPLES) and the
     * decode loop drains it below one Opus frame before the next write, so it
     * never overruns. Skip av_audio_fifo_realloc: the hot transcode path must
     * not re-allocate on every AAC frame. */
    if (av_audio_fifo_size(a->fifo) + nb_samples
            > (int)NGX_RTC_AUDIO_FIFO_MAX_SAMPLES) {
        return -1;
    }
    if (av_audio_fifo_write(a->fifo, (void **)a->swr_data, nb_samples) < nb_samples) {
        return -1;
    }

    return 0;
}

/* Encode every full Opus frame buffered in the FIFO. */
static int32_t
ngx_rtc_audio_encode(ngx_rtc_audio_t *a, ngx_rtc_audio_frame_fn emit, void *opaque)
{
    uint8_t *out[1];
    int32_t  nb;
    int32_t  rc;

    while (av_audio_fifo_size(a->fifo) >= (int)NGX_RTC_AUDIO_OPUS_FRAME_SIZE) {
        out[0] = a->pcm_buf;
        if (av_audio_fifo_read(a->fifo, (void **)out,
                               (int)NGX_RTC_AUDIO_OPUS_FRAME_SIZE)
                < (int)NGX_RTC_AUDIO_OPUS_FRAME_SIZE) {
            return -1;
        }

        nb = opus_encode(a->enc, (const opus_int16 *)a->pcm_buf,
                         (int)NGX_RTC_AUDIO_OPUS_FRAME_SIZE,
                         a->opus_buf, (opus_int32)sizeof(a->opus_buf));
        if (nb < 0) {
            return -1;
        }

        rc = emit(opaque, a->opus_buf, (uint32_t)nb);
        if (rc < 0) {
            return rc;
        }
    }

    return 0;
}
