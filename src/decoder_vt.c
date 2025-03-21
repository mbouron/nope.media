/*
 * This file is part of nope.media.
 *
 * Copyright (c) 2015 Stupeflix
 * Copyright (c) 2012 Sebastien Zwickert
 *
 * nope.media is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * nope.media is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with nope.media; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <libavutil/avassert.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/mem.h>
#include <libavutil/avstring.h>
#include <VideoToolbox/VideoToolbox.h>

#include "mod_decoding.h"
#include "decoders.h"
#include "internal.h"
#include "log.h"
#include "pthread_compat.h"

#define BUFCOUNT_DEBUG 0

#if BUFCOUNT_DEBUG
static pthread_once_t g_bufcounter_initialized = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_mutex_bufcounter;
static int g_bufcount;
static int g_deccount;

static int initialize_bufcounter(void)
{
    return pthread_mutex_init(&g_mutex_bufcounter, NULL);
}

static int bufcounter_update(int n)
{
    pthread_mutex_lock(&g_mutex_bufcounter);
    g_bufcount += n;
    fprintf(stderr, "[BUFFER COUNTER] op:[%s%d] newtotal:[%d] nbdec:[%d]\n",
            n > 0 ? "+" : "", n, g_bufcount, g_deccount);
    pthread_mutex_unlock(&g_mutex_bufcounter);
}

static int deccounter_update(int n)
{
    pthread_mutex_lock(&g_mutex_bufcounter);
    g_deccount += n;
    pthread_mutex_unlock(&g_mutex_bufcounter);
}
#else
#define bufcounter_update(x) (void)(x)
#define deccounter_update(x) (void)(x)
#endif

struct async_frame {
    int64_t pts;
    CVPixelBufferRef cv_buffer;
    struct async_frame *next_frame;
};

struct vtdec_context {
    VTDecompressionSessionRef session;
    CMVideoFormatDescriptionRef cm_fmt_desc;
    struct async_frame *queue;
    int nb_frames;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int nb_queued;
    int out_w, out_h;
};

static CMVideoFormatDescriptionRef format_desc_create(CMVideoCodecType codec_type,
                                                      CFDictionaryRef decoder_spec,
                                                      int width, int height)
{
    CMFormatDescriptionRef cm_fmt_desc;
    OSStatus status;

    status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault,
                                            codec_type,
                                            width,
                                            height,
                                            decoder_spec, // Dictionary of extension
                                            &cm_fmt_desc);

    if (status)
        return NULL;

    return cm_fmt_desc;
}

static void dict_set_data(CFMutableDictionaryRef dict, CFStringRef key, uint8_t * value, uint64_t length)
{
    CFDataRef data;
    data = CFDataCreate(NULL, value, (CFIndex)length);
    CFDictionarySetValue(dict, key, data);
    CFRelease(data);
}

static CFDictionaryRef decoder_config_create(CMVideoCodecType codec_type,
                                             const AVCodecContext *avctx)
{
    CFMutableDictionaryRef config_info = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                                   2,
                                                                   &kCFTypeDictionaryKeyCallBacks,
                                                                   &kCFTypeDictionaryValueCallBacks);

    CFDictionarySetValue(config_info,
                         CFSTR("EnableHardwareAcceleratedVideoDecoder"),
                         kCFBooleanTrue);

    if (avctx->codec_id != AV_CODEC_ID_H264 && avctx->codec_id != AV_CODEC_ID_HEVC)
        return config_info;

    if (avctx->extradata_size) {
        CFMutableDictionaryRef avc_info;

        avc_info = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                             1,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);

        switch (avctx->codec_id) {
        case AV_CODEC_ID_H264:
            dict_set_data(avc_info, CFSTR("avcC"), avctx->extradata, avctx->extradata_size);
            break;
        case AV_CODEC_ID_HEVC:
            dict_set_data(avc_info, CFSTR("hvcC"), avctx->extradata, avctx->extradata_size);
            break;
        default:
            av_assert0(0);
        }

        CFDictionarySetValue(config_info,
                kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms,
                avc_info);

        CFRelease(avc_info);
    }
    return config_info;
}

static CFDictionaryRef buffer_attributes_create(int width, int height, OSType pix_fmt)
{
    CFMutableDictionaryRef buffer_attributes;
    CFMutableDictionaryRef io_surface_properties;
    CFNumberRef cv_pix_fmt;
    CFNumberRef w;
    CFNumberRef h;

    w = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &width);
    h = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &height);
    cv_pix_fmt = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pix_fmt);

    buffer_attributes = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                  4,
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    io_surface_properties = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                      0,
                                                      &kCFTypeDictionaryKeyCallBacks,
                                                      &kCFTypeDictionaryValueCallBacks);

    CFDictionarySetValue(buffer_attributes, kCVPixelBufferPixelFormatTypeKey, cv_pix_fmt);
    CFDictionarySetValue(buffer_attributes, kCVPixelBufferIOSurfacePropertiesKey, io_surface_properties);
    CFDictionarySetValue(buffer_attributes, kCVPixelBufferWidthKey, w);
    CFDictionarySetValue(buffer_attributes, kCVPixelBufferHeightKey, h);

    CFRelease(io_surface_properties);
    CFRelease(cv_pix_fmt);
    CFRelease(w);
    CFRelease(h);

    return buffer_attributes;
}

static void buffer_release(void *opaque, uint8_t *data)
{
    CVPixelBufferRef cv_buffer = (CVImageBufferRef)data;
    CVPixelBufferRelease(cv_buffer);
    bufcounter_update(-1);
}

static int push_async_frame(struct decoder_ctx *dec_ctx,
                            struct async_frame *async_frame)
{
    int ret;
    const AVCodecContext *avctx = dec_ctx->avctx;
    const struct vtdec_context *vt = dec_ctx->priv_data;
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

    frame->width   = vt->out_w;
    frame->height  = vt->out_h;
    frame->format  = avctx->pix_fmt;
    frame->pts     = async_frame->pts;
    frame->color_range     = avctx->color_range;
    frame->color_primaries = avctx->color_primaries;
    frame->color_trc       = avctx->color_trc;
    frame->colorspace      = avctx->colorspace;
    frame->data[3] = (uint8_t *)async_frame->cv_buffer;
    frame->buf[0]  = av_buffer_create(frame->data[3],
                                      sizeof(frame->data[3]),
                                      buffer_release,
                                      NULL,
                                      AV_BUFFER_FLAG_READONLY);
    if (!frame->buf[0]) {
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }
    TRACE(dec_ctx, "push frame pts=%"PRId64, frame->pts);
    ret = nmdi_decoding_queue_frame(dec_ctx->decoding_ctx, frame);
    if (ret < 0)
        av_frame_free(&frame);
    return ret;
}

static void update_nb_queue(struct decoder_ctx *dec_ctx,
                            struct vtdec_context *vt, int diff)
{
    pthread_mutex_lock(&vt->lock);
    TRACE(dec_ctx, "frame counter %d: %d -> %d",
          diff, vt->nb_queued, vt->nb_queued + diff);
    vt->nb_queued += diff;
    pthread_cond_signal(&vt->cond);
    pthread_mutex_unlock(&vt->lock);
}

static void decode_callback(void *opaque,
                            void *sourceFrameRefCon,
                            OSStatus status,
                            VTDecodeInfoFlags flags,
                            CVImageBufferRef image_buffer,
                            CMTime pts,
                            CMTime duration)
{
    struct decoder_ctx *dec_ctx = opaque;
    struct vtdec_context *vt = dec_ctx->priv_data;
    struct async_frame *new_frame;
    struct async_frame *queue_walker;

    TRACE(dec_ctx, "entering decode callback");

    if (!image_buffer) {
        TRACE(dec_ctx, "decode cb received NULL output image buffer");
        update_nb_queue(dec_ctx, vt, -1);
        return;
    }

    new_frame = av_mallocz(sizeof(struct async_frame));
    new_frame->next_frame = NULL;
    new_frame->cv_buffer = CVPixelBufferRetain(image_buffer);
    new_frame->pts = pts.value;

    bufcounter_update(1);

    queue_walker = vt->queue;

    if (!queue_walker || (new_frame->pts < queue_walker->pts)) {
        /* we have an empty queue, or this frame earlier than the current queue head */
        new_frame->next_frame = queue_walker;
        vt->queue = new_frame;
        TRACE(dec_ctx, "queueing frame pts=%"PRId64" at pos=%d",
              new_frame->pts, vt->nb_frames);
        vt->nb_frames++;
    } else {
        /* walk the queue and insert this frame where it belongs in display order */
        struct async_frame *next_frame;

        while (1) {
            next_frame = queue_walker->next_frame;

            if (!next_frame || (new_frame->pts < next_frame->pts)) {
                new_frame->next_frame = next_frame;
                queue_walker->next_frame = new_frame;
                TRACE(dec_ctx, "queueing frame pts=%"PRId64" at pos=%d",
                      new_frame->pts, vt->nb_frames);
                vt->nb_frames++;
                break;
            }

            /* We passed a frame, which as a result becomes a valid frame to push */
            push_async_frame(dec_ctx, queue_walker);
            av_free(queue_walker);
            vt->nb_frames--;
            vt->queue = queue_walker = next_frame;
        }
    }

    update_nb_queue(dec_ctx, vt, -1);
}

static int pix_fmt_ff2vt(enum AVPixelFormat ff_pix_fmt, OSType *cv_pix_fmt, int color_range)
{
    switch (ff_pix_fmt) {
    case AV_PIX_FMT_NV12:
        *cv_pix_fmt = color_range == AVCOL_RANGE_JPEG ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                                      : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        break;
    case AV_PIX_FMT_NV16:
        *cv_pix_fmt = kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange;
        break;
    case AV_PIX_FMT_NV20:
        *cv_pix_fmt = kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange;
        break;
    case AV_PIX_FMT_P010:
        *cv_pix_fmt = color_range == AVCOL_RANGE_JPEG ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
                                                      : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
        break;
    case AV_PIX_FMT_P210:
        *cv_pix_fmt = kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange;
        break;
    case AV_PIX_FMT_P410:
        *cv_pix_fmt = kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange;
        break;
    case AV_PIX_FMT_BGRA:
        *cv_pix_fmt = kCVPixelFormatType_32BGRA;
        break;
    default:
        return AVERROR(EINVAL);
    }
    return 0;
}

static int parse_allowed_pix_fmts(const char *pix_fmts_str,
                                  enum AVPixelFormat **pix_fmtsp,
                                  int *nb_pix_fmtsp)
{
    enum AVPixelFormat *pix_fmts = NULL;
    int nb_pix_fmts = 0;

    char *tmp = av_strdup(pix_fmts_str);
    if (!tmp)
        return AVERROR(ENOMEM);

    char *s = tmp;
    char *ptr = NULL;
    char *pix_fmt_str = NULL;
    while ((pix_fmt_str = av_strtok(s, ", ", &ptr)) != NULL) {
        s = NULL;
        const enum AVPixelFormat pix_fmt = av_get_pix_fmt(pix_fmt_str);
        if (pix_fmt == AV_PIX_FMT_NONE)
            continue;
        OSType cv_pix_fmt;
        int ret = pix_fmt_ff2vt(pix_fmt, &cv_pix_fmt, 0);
        if (ret < 0)
            continue;
        pix_fmts = av_realloc_f(pix_fmts, nb_pix_fmts + 1, sizeof(pix_fmt));
        if (!pix_fmts) {
            av_freep(&tmp);
            return AVERROR(ENOMEM);
        }
        pix_fmts[nb_pix_fmts++] = pix_fmt;
    }

    *pix_fmtsp = pix_fmts;
    *nb_pix_fmtsp = nb_pix_fmts;

    av_freep(&tmp);

    return 0;
}


static int is_pix_fmt_allowed(const enum AVPixelFormat *pix_fmts,
                              int nb_pix_fmts,
                              enum AVPixelFormat pix_fmt)
{
    for (int i = 0; i < nb_pix_fmts; i++) {
        if (pix_fmts[i] == pix_fmt)
            return 1;
    }
    return 0;
}


static enum AVPixelFormat select_pix_fmt(const enum AVPixelFormat *pix_fmts,
                                         int nb_pix_fmts,
                                         enum AVPixelFormat in_pix_fmt)
{
    /* If file probing wasn't able to detect the pixel format, we force it to NV12 if available */
    if (in_pix_fmt == AV_PIX_FMT_NONE && is_pix_fmt_allowed(pix_fmts, nb_pix_fmts, AV_PIX_FMT_NV12))
        return AV_PIX_FMT_NV12;

    static const enum AVPixelFormat supported_pix_fmts[] = {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_NV16,
        AV_PIX_FMT_NV20,
        AV_PIX_FMT_P010,
        AV_PIX_FMT_P210,
        AV_PIX_FMT_P410,
        AV_PIX_FMT_BGRA,
    };
    enum AVPixelFormat best = AV_PIX_FMT_NONE;
    for (size_t i = 0; i < FF_ARRAY_ELEMS(supported_pix_fmts); i++) {
        const enum AVPixelFormat p = supported_pix_fmts[i];
        if (is_pix_fmt_allowed(pix_fmts, nb_pix_fmts, p))
            best = av_find_best_pix_fmt_of_2(best, p, in_pix_fmt, 0, NULL);
    }
    return best;
}

static int vtdec_init(struct decoder_ctx *dec_ctx, const struct nmdi_opts *opts)
{
    int ret;
    AVCodecContext *avctx = dec_ctx->avctx;
    struct vtdec_context *vt = dec_ctx->priv_data;
    int cm_codec_type;
    OSStatus status;
    VTDecompressionOutputCallbackRecord decoder_cb;
    CFDictionaryRef decoder_spec;
    CFDictionaryRef buf_attr;

#if BUFCOUNT_DEBUG
    ret = pthread_once(&g_bufcounter_initialized, initialize_bufcounter);
    if (ret < 0)
        return AVERROR(ret);
    deccounter_update(1);
#endif

    TRACE(dec_ctx, "init");

    const enum AVPixelFormat hw_subfmt = avctx->pix_fmt;
    avctx->pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;

    pthread_mutex_init(&vt->lock, NULL);
    pthread_cond_init(&vt->cond, NULL);

    switch (avctx->codec_id) {
    case AV_CODEC_ID_H264:       cm_codec_type = kCMVideoCodecType_H264;       break;
    case AV_CODEC_ID_HEVC:       cm_codec_type = kCMVideoCodecType_HEVC;       break;
    case AV_CODEC_ID_PRORES:
        switch (avctx->codec_tag) {
        case MKTAG('a','p','c','n'): cm_codec_type = kCMVideoCodecType_AppleProRes422;      break;
        case MKTAG('a','p','c','h'): cm_codec_type = kCMVideoCodecType_AppleProRes422HQ;    break;
        case MKTAG('a','p','c','s'): cm_codec_type = kCMVideoCodecType_AppleProRes422LT;    break;
        case MKTAG('a','p','c','o'): cm_codec_type = kCMVideoCodecType_AppleProRes422Proxy; break;
        case MKTAG('a','p','4','x'):
        case MKTAG('a','p','4','h'): cm_codec_type = kCMVideoCodecType_AppleProRes4444;     break;
        default:
            return AVERROR_DECODER_NOT_FOUND;
        }
        break;
    default:
        return AVERROR_DECODER_NOT_FOUND;
    }

    decoder_spec = decoder_config_create(cm_codec_type, avctx);

    vt->cm_fmt_desc = format_desc_create(cm_codec_type, decoder_spec,
                                         avctx->width, avctx->height);
    if (!vt->cm_fmt_desc) {
        if (decoder_spec)
            CFRelease(decoder_spec);

        LOG(dec_ctx, ERROR, "format description creation failed");
        return AVERROR_EXTERNAL;
    }

    vt->out_w = avctx->width;
    vt->out_h = avctx->height;
    nmdi_update_dimensions(&vt->out_w, &vt->out_h, opts->max_pixels);
    TRACE(dec_ctx, "dimensions: %dx%d -> %dx%d (nb pixels: %d -> %d for a max of %d)\n",
          avctx->width, avctx->height, vt->out_w, vt->out_h,
          avctx->width * avctx->height, vt->out_w * vt->out_h,
          opts->max_pixels);

    enum AVPixelFormat *pix_fmts;
    int nb_pix_fmts;
    ret = parse_allowed_pix_fmts(opts->vt_pix_fmt, &pix_fmts, &nb_pix_fmts);
    if (ret < 0)
        return ret;

    const enum AVPixelFormat pix_fmt = select_pix_fmt(pix_fmts, nb_pix_fmts, hw_subfmt);
    av_freep(&pix_fmts);

    if (pix_fmt == AV_PIX_FMT_NONE) {
        LOG(dec_ctx, ERROR, "could not select a supported pixel format from vt_pix_fmt list '%s'",
            opts->vt_pix_fmt);
        return AVERROR(EINVAL);
    }

    OSType vt_pix_fmt;
    ret = pix_fmt_ff2vt(pix_fmt, &vt_pix_fmt, avctx->color_range);
    av_assert0(ret == 0);

    buf_attr = buffer_attributes_create(vt->out_w, vt->out_h, vt_pix_fmt);

    decoder_cb.decompressionOutputCallback = decode_callback;
    decoder_cb.decompressionOutputRefCon   = dec_ctx;

    status = VTDecompressionSessionCreate(NULL,
                                          vt->cm_fmt_desc,
                                          decoder_spec,
                                          buf_attr,
                                          &decoder_cb,
                                          &vt->session);

    if (decoder_spec)
        CFRelease(decoder_spec);
    if (buf_attr)
        CFRelease(buf_attr);

    switch (status) {
    case kVTVideoDecoderNotAvailableNowErr:
        LOG(dec_ctx, ERROR, "Video decoder not available now");
        return AVERROR(ENOSYS);
    case kVTVideoDecoderUnsupportedDataFormatErr:
        LOG(dec_ctx, ERROR, "Unsupported data format");
        return AVERROR(ENOSYS);
    case kVTVideoDecoderMalfunctionErr:
        LOG(dec_ctx, ERROR, "Malfunction detected");
        return AVERROR(EINVAL);
    case kVTVideoDecoderBadDataErr:
        LOG(dec_ctx, ERROR, "Bad Data");
        return AVERROR_INVALIDDATA;
    case kVTCouldNotFindVideoDecoderErr:
        LOG(dec_ctx, ERROR, "Could not find video decoder");
        return AVERROR_DECODER_NOT_FOUND;
    case 0:
        return 0;
    default:
        LOG(dec_ctx, ERROR, "Unknown error %d", status);
        return AVERROR_UNKNOWN;
    }
}

static CMSampleBufferRef sample_buffer_create(CMFormatDescriptionRef fmt_desc,
                                                           void *buffer,
                                                           int size,
                                                           int64_t frame_pts)
{
    OSStatus status;
    CMBlockBufferRef  block_buf;
    CMSampleBufferRef sample_buf;
    CMSampleTimingInfo timeInfoArray[1] = {0};

    timeInfoArray[0].presentationTimeStamp = CMTimeMake(frame_pts, 1);
    timeInfoArray[0].decodeTimeStamp = kCMTimeInvalid;

    block_buf  = NULL;
    sample_buf = NULL;

    status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault,// structureAllocator
                                                buffer,             // memoryBlock
                                                size,               // blockLength
                                                kCFAllocatorNull,   // blockAllocator
                                                NULL,               // customBlockSource
                                                0,                  // offsetToData
                                                size,               // dataLength
                                                0,                  // flags
                                                &block_buf);

    if (!status) {
        status = CMSampleBufferCreate(kCFAllocatorDefault,  // allocator
                                      block_buf,            // dataBuffer
                                      TRUE,                 // dataReady
                                      0,                    // makeDataReadyCallback
                                      0,                    // makeDataReadyRefcon
                                      fmt_desc,             // formatDescription
                                      1,                    // numSamples
                                      1,                    // numSampleTimingEntries
                                      timeInfoArray,        // sampleTimingArray
                                      0,                    // numSampleSizeEntries
                                      NULL,                 // sampleSizeArray
                                      &sample_buf);
    }

    if (block_buf)
        CFRelease(block_buf);

    return sample_buf;
}

static int vtdec_push_packet(struct decoder_ctx *dec_ctx, const AVPacket *pkt)
{
    int status;
    struct vtdec_context *vt = dec_ctx->priv_data;

    /* For some insane reason, pushing more than 3 packets to VT will cause a
     * fatal deadlock when the application is going in background on iOS. */
    pthread_mutex_lock(&vt->lock);
    while (vt->nb_queued >= 3)
        pthread_cond_wait(&vt->cond, &vt->lock);
    pthread_mutex_unlock(&vt->lock);

    if (!pkt || !pkt->size) {
        VTDecompressionSessionFinishDelayedFrames(vt->session);
        return AVERROR_EOF;
    }

    VTDecodeFrameFlags decodeFlags = kVTDecodeFrame_EnableAsynchronousDecompression;
    CMSampleBufferRef sample_buf = sample_buffer_create(vt->cm_fmt_desc, pkt->data, pkt->size, pkt->pts);

    if (!sample_buf)
        return AVERROR_EXTERNAL;

    update_nb_queue(dec_ctx, vt, 1);
    status = VTDecompressionSessionDecodeFrame(vt->session,
                                               sample_buf,
                                               decodeFlags,
                                               NULL,    // sourceFrameRefCon
                                               0);      // infoFlagsOut

#if 0
    if (status == noErr)
        status = VTDecompressionSessionWaitForAsynchronousFrames(vt->session);
#endif

    CFRelease(sample_buf);

    if (status) {
        LOG(dec_ctx, ERROR, "Failed to decode frame (%d)", status);
        pthread_mutex_lock(&vt->lock);
        vt->nb_queued = 0;
        pthread_cond_signal(&vt->cond);
        pthread_mutex_unlock(&vt->lock);
        return AVERROR_EXTERNAL;
    }

    return pkt->size;
}

static inline void process_queued_frames(struct decoder_ctx *dec_ctx, int push)
{
    struct vtdec_context *vt = dec_ctx->priv_data;

    TRACE(dec_ctx, "%sing %d frames", push ? "push" : "dropp", vt->nb_frames);
    while (vt->queue != NULL) {
        struct async_frame *top_frame = vt->queue;
        vt->queue = top_frame->next_frame;
        if (push)
            push_async_frame(dec_ctx, top_frame);
        av_freep(&top_frame);
    }
    vt->nb_frames = 0;
}

static void drop_queued_frames(struct decoder_ctx *dec_ctx)
{
    process_queued_frames(dec_ctx, 0);
}

static void send_queued_frames(struct decoder_ctx *dec_ctx)
{
    process_queued_frames(dec_ctx, 1);
}

static void vtdec_flush(struct decoder_ctx *dec_ctx)
{
    struct vtdec_context *vt = dec_ctx->priv_data;

    TRACE(dec_ctx, "flushing");
    if (vt->session) {
        VTDecompressionSessionFinishDelayedFrames(vt->session);
        VTDecompressionSessionWaitForAsynchronousFrames(vt->session);
    }

    // The decode callback can actually be called after
    // VTDecompressionSessionWaitForAsynchronousFrames(), so we need this kind
    // of shit. Yes, fuck you Apple. Fuck you hard.
    pthread_mutex_lock(&vt->lock);
    while (vt->nb_queued > 0)
        pthread_cond_wait(&vt->cond, &vt->lock);
    pthread_mutex_unlock(&vt->lock);

    TRACE(dec_ctx, "decompression session finished delaying frames");
    send_queued_frames(dec_ctx);
    nmdi_decoding_queue_frame(dec_ctx->decoding_ctx, NULL);
    TRACE(dec_ctx, "queue cleared, flush ends");
}

static void vtdec_uninit(struct decoder_ctx *dec_ctx)
{
    struct vtdec_context *vt = dec_ctx->priv_data;

    TRACE(dec_ctx, "uninit");

    pthread_mutex_destroy(&vt->lock);
    pthread_cond_destroy(&vt->cond);

    if (vt->cm_fmt_desc)
        CFRelease(vt->cm_fmt_desc);

    drop_queued_frames(dec_ctx);

    if (vt->session) {
        VTDecompressionSessionInvalidate(vt->session);
        CFRelease(vt->session);
        vt->session = NULL;
    }

    deccounter_update(-1);
}

const struct decoder nmdi_decoder_vt = {
    .name             = "videotoolbox",
    .init             = vtdec_init,
    .push_packet      = vtdec_push_packet,
    .flush            = vtdec_flush,
    .uninit           = vtdec_uninit,
    .priv_data_size   = sizeof(struct vtdec_context),
};
