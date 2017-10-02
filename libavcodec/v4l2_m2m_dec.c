/*
 * V4L2 mem2mem decoders
 *
 * Copyright (C) 2017 Alexis Ballier <aballier@gentoo.org>
 * Copyright (C) 2017 Jorge Ramirez <jorge.ramirez-ortiz@linaro.org>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include "libavutil/pixfmt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"
#include "libavcodec/avcodec.h"

#include "v4l2_context.h"
#include "v4l2_m2m.h"
#include "v4l2_fmt.h"

static int v4l2_try_start(AVCodecContext *avctx)
{
    V4L2m2mContext *s = avctx->priv_data;
    V4L2Context *const capture = &s->capture;
    V4L2Context *const output = &s->output;
    struct v4l2_selection selection;
    int ret;

    /* 1. start the output process */
    if (!output->streamon) {
        ret = ff_v4l2_context_set_status(output, VIDIOC_STREAMON);
        if (ret < 0) {
            av_log(avctx, AV_LOG_DEBUG, "VIDIOC_STREAMON on output context\n");
            return ret;
        }
    }

    if (capture->streamon)
        return 0;

    /* 2. get the capture format */
    capture->format.type = capture->type;
    ret = ioctl(s->fd, VIDIOC_G_FMT, &capture->format);
    if (ret) {
        av_log(avctx, AV_LOG_WARNING, "VIDIOC_G_FMT ioctl\n");
        return ret;
    }

    /* 2.1 update the AVCodecContext */
    avctx->pix_fmt = ff_v4l2_format_v4l2_to_avfmt(capture->format.fmt.pix_mp.pixelformat, AV_CODEC_ID_RAWVIDEO);
    capture->av_pix_fmt = avctx->pix_fmt;

    /* 3. set the crop parameters */
    selection.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    selection.r.height = avctx->coded_height;
    selection.r.width = avctx->coded_width;
    ret = ioctl(s->fd, VIDIOC_S_SELECTION, &selection);
    if (!ret) {
        ret = ioctl(s->fd, VIDIOC_G_SELECTION, &selection);
        if (ret) {
            av_log(avctx, AV_LOG_WARNING, "VIDIOC_G_SELECTION ioctl\n");
        } else {
            av_log(avctx, AV_LOG_DEBUG, "crop output %dx%d\n", selection.r.width, selection.r.height);
            /* update the size of the resulting frame */
            capture->height = selection.r.height;
            capture->width  = selection.r.width;
        }
    }

    /* 4. init the capture context now that we have the capture format */
    if (!capture->buffers) {
        ret = ff_v4l2_context_init(capture);
        if (ret) {
            av_log(avctx, AV_LOG_DEBUG, "can't request output buffers\n");
            return ret;
        }
    }

    /* 5. start the capture process */
    ret = ff_v4l2_context_set_status(capture, VIDIOC_STREAMON);
    if (ret) {
        av_log(avctx, AV_LOG_DEBUG, "VIDIOC_STREAMON, on capture context\n");
        return ret;
    }

    return 0;
}

static int v4l2_prepare_decoder(V4L2m2mContext *s)
{
    struct v4l2_event_subscription sub;
    V4L2Context *output = &s->output;
    int ret;

    /**
     * requirements
     */
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    ret = ioctl(s->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    if ( ret < 0) {
        if (output->height == 0 || output->width == 0) {
            av_log(s->avctx, AV_LOG_ERROR,
                "the v4l2 driver does not support VIDIOC_SUBSCRIBE_EVENT\n"
                "you must provide codec_height and codec_width on input\n");
            return ret;
        }
    }

    return 0;
}

static int v4l2_send_packet(AVCodecContext *avctx, const AVPacket *avpkt)
{
    V4L2m2mContext *s = avctx->priv_data;
    V4L2Context *const output = &s->output;
    AVPacket filtered_packet = { 0 };
    int ret = 0;

    if (s->draining)
        goto done;

    if (s->bsfc) {
        AVPacket filter_packet = { 0 };

        ret = av_packet_ref(&filter_packet, avpkt);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "filter failed to ref input packet\n");
            goto done;
        }

        ret = av_bsf_send_packet(s->bsfc, &filter_packet);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "filter failed to send input packet\n");
            goto done;
        }

        ret = av_bsf_receive_packet(s->bsfc, &filtered_packet);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "filter failed to receive output packet\n");
            goto done;
        }

        avpkt = &filtered_packet;
        av_packet_unref(&filter_packet);
    }

    ret = ff_v4l2_context_enqueue_packet(output, avpkt);
    if (ret < 0) {
        if (ret != AVERROR(ENOMEM))
           return ret;
        /* no input buffers available, continue dequeing */
    }

    v4l2_try_start(avctx);

done:
    av_packet_unref(&filtered_packet);
    return ret;
}

static int v4l2_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    V4L2m2mContext *s = avctx->priv_data;
    V4L2Context *capture = &s->capture;

    return ff_v4l2_context_dequeue_frame(capture, frame);
}

static int v4l2_decode_frame(AVCodecContext *avctx, void *frame, int *got_frame, AVPacket *pkt)
{
    int ret;

    *got_frame = 0;

    if (pkt) {
        ret = avcodec_send_packet(avctx, pkt);
        if (ret < 0 && ret != AVERROR_EOF)
            return ret;
    }

    ret = avcodec_receive_frame(avctx, (AVFrame *) frame);
    if (ret < 0 && ret != AVERROR(EAGAIN))
        return ret;
    if (ret >= 0)
        *got_frame = 1;

    return 0;
}

static av_cold int v4l2_init_bsf(AVCodecContext *avctx, const char *bsf_name)
{
    V4L2m2mContext *s = avctx->priv_data;
    const AVBitStreamFilter *bsf;
    void *extradata = NULL;
    size_t size = 0;
    int avret;

    bsf = av_bsf_get_by_name(bsf_name);
    if (!bsf) {
        av_log(avctx, AV_LOG_ERROR, "Cannot open the %s BSF!\n", bsf_name);
        return AVERROR_BSF_NOT_FOUND;
    }

    avret = av_bsf_alloc(bsf, &s->bsfc);
    if (avret != 0)
        return avret;

    avret = avcodec_parameters_from_context(s->bsfc->par_in, avctx);
    if (avret != 0)
        return avret;

    avret = av_bsf_init(s->bsfc);
    if (avret != 0)
        return avret;

    /* Back up the extradata so it can be restored at close time. */
    s->orig_extradata = avctx->extradata;
    s->orig_extradata_size = avctx->extradata_size;

    size = s->bsfc->par_out->extradata_size;
    extradata = av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!extradata) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate copy of extradata\n");
        return AVERROR(ENOMEM);
    }

    memcpy(extradata, s->bsfc->par_out->extradata, size);

    avctx->extradata = extradata;
    avctx->extradata_size = size;

    return 0;
}

static av_cold int v4l2_decode_init(AVCodecContext *avctx)
{
    V4L2m2mContext *s = avctx->priv_data;
    V4L2Context *capture = &s->capture;
    V4L2Context *output = &s->output;
    int ret;

    /* if these dimensions are invalid (ie, 0 or too small) an event will be raised
     * by the v4l2 driver; this event will trigger a full pipeline reconfig and
     * the proper values will be retrieved from the kernel driver.
     */
    output->height = capture->height = avctx->coded_height;
    output->width = capture->width = avctx->coded_width;

    output->av_codec_id = avctx->codec_id;
    output->av_pix_fmt  = AV_PIX_FMT_NONE;

    capture->av_codec_id = AV_CODEC_ID_RAWVIDEO;
    capture->av_pix_fmt = avctx->pix_fmt;

    ret = ff_v4l2_m2m_codec_init(avctx);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "can't configure decoder\n");
        return ret;
    }

    if (output->av_codec_id == AV_CODEC_ID_H264)
        v4l2_init_bsf(avctx, "h264_mp4toannexb");

    if (output->av_codec_id == AV_CODEC_ID_HEVC)
        v4l2_init_bsf(avctx, "hevc_mp4toannexb");

    return v4l2_prepare_decoder(s);
}

#define OFFSET(x) offsetof(V4L2m2mContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM

static const AVOption options[] = {
    V4L_M2M_DEFAULT_OPTS,
    { "num_capture_buffers", "Number of buffers in the capture context",
        OFFSET(capture.num_buffers), AV_OPT_TYPE_INT, {.i64 = 20}, 20, INT_MAX, FLAGS },
    { NULL},
};

#define M2MDEC(NAME, LONGNAME, CODEC) \
static const AVClass v4l2_m2m_ ## NAME ## _dec_class = {\
    .class_name = #NAME "_v4l2_m2m_decoder",\
    .item_name  = av_default_item_name,\
    .option     = options,\
    .version    = LIBAVUTIL_VERSION_INT,\
};\
\
AVCodec ff_ ## NAME ## _v4l2m2m_decoder = {                                             \
    .name           = #NAME "_v4l2m2m" ,                                                \
    .long_name      = NULL_IF_CONFIG_SMALL("V4L2 mem2mem " LONGNAME " decoder wrapper"),\
    .type           = AVMEDIA_TYPE_VIDEO,                                               \
    .id             = CODEC ,                                                           \
    .priv_data_size = sizeof(V4L2m2mContext),                                           \
    .priv_class     = &v4l2_m2m_ ## NAME ## _dec_class,                                 \
    .init           = v4l2_decode_init,                                                 \
    .decode         = v4l2_decode_frame,  /* for ffplay on 3.3.3 */                     \
    .send_packet    = v4l2_send_packet,                                                 \
    .receive_frame  = v4l2_receive_frame,                                               \
    .close          = ff_v4l2_m2m_codec_end,                                            \
};                                                                                      \

M2MDEC(mpeg1, "MPEG1", AV_CODEC_ID_MPEG1VIDEO);
M2MDEC(mpeg2, "MPEG2", AV_CODEC_ID_MPEG2VIDEO);
M2MDEC(mpeg4, "MPEG4", AV_CODEC_ID_MPEG4);
M2MDEC(h263,  "H.263", AV_CODEC_ID_H263);
M2MDEC(h264,  "H.264", AV_CODEC_ID_H264);
M2MDEC(hevc,  "HEVC",  AV_CODEC_ID_HEVC);
M2MDEC(vc1 ,  "VC1",   AV_CODEC_ID_VC1);
M2MDEC(vp8,   "VP8",   AV_CODEC_ID_VP8);
M2MDEC(vp9,   "VP9",   AV_CODEC_ID_VP9);
