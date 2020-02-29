/*
* Scalable Video Technology for VP9 encoder library plugin
*
* Copyright (c) 2018 Intel Corporation
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
* License along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include <stdint.h>
#include "EbSvtVp9ErrorCodes.h"
#include "EbSvtVp9Time.h"
#include "EbSvtVp9Enc.h"

#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/opt.h"

#include "internal.h"
#include "avcodec.h"

typedef enum eos_status {
    EOS_NOT_REACHED = 0,
    EOS_REACHED,
    EOS_TOTRIGGER
}EOS_STATUS;

typedef struct SvtContext {
    AVClass     *class;

    EbSvtVp9EncConfiguration    enc_params;
    EbComponentType            *svt_handle;

    EbBufferHeaderType         *in_buf;
    int                         raw_size;

    AVBufferPool* pool;

    EOS_STATUS eos_flag;

    // User options.
    int enc_mode;
    int rc_mode;
    int tune;
    int qp;

    int forced_idr;

    int level;

    int base_layer_switch_mode;
} SvtContext;

static int error_mapping(EbErrorType svt_ret)
{
    int err;

    switch (svt_ret) {
    case EB_ErrorInsufficientResources:
        err = AVERROR(ENOMEM);
        break;

    case EB_ErrorUndefined:
    case EB_ErrorInvalidComponent:
    case EB_ErrorBadParameter:
        err = AVERROR(EINVAL);
        break;

    case EB_ErrorDestroyThreadFailed:
    case EB_ErrorSemaphoreUnresponsive:
    case EB_ErrorDestroySemaphoreFailed:
    case EB_ErrorCreateMutexFailed:
    case EB_ErrorMutexUnresponsive:
    case EB_ErrorDestroyMutexFailed:
        err = AVERROR_EXTERNAL;
            break;

    case EB_NoErrorEmptyQueue:
        err = AVERROR(EAGAIN);

    case EB_ErrorNone:
        err = 0;
        break;

    default:
        err = AVERROR_UNKNOWN;
    }

    return err;
}

static void free_buffer(SvtContext *svt_enc)
{
    if (svt_enc->in_buf) {
        EbSvtEncInput *in_data = (EbSvtEncInput *)svt_enc->in_buf->p_buffer;
        av_freep(&in_data);
        av_freep(&svt_enc->in_buf);
    }
    av_buffer_pool_uninit(&svt_enc->pool);
}

static int alloc_buffer(EbSvtVp9EncConfiguration *config, SvtContext *svt_enc)
{
    const size_t luma_size_8bit    =
        config->source_width * config->source_height;
    const size_t luma_size_10bit   =
        (config->encoder_bit_depth > 8) ? luma_size_8bit : 0;

    EbSvtEncInput *in_data;

    svt_enc->raw_size = (luma_size_8bit + luma_size_10bit) * 3 / 2;

    // allocate buffer for in and out
    svt_enc->in_buf           = av_mallocz(sizeof(*svt_enc->in_buf));
    if (!svt_enc->in_buf)
        goto failed;


    svt_enc->in_buf->p_buffer = (unsigned char *)av_mallocz(sizeof(*in_data));
    if (!svt_enc->in_buf->p_buffer)
        goto failed;

    svt_enc->in_buf->size        = sizeof(*svt_enc->in_buf);
    svt_enc->in_buf->p_app_private  = NULL;

    svt_enc->pool = av_buffer_pool_init(svt_enc->raw_size, NULL);
    if (!svt_enc->pool)
        goto failed;

    return 0;

failed:
    free_buffer(svt_enc);
    return AVERROR(ENOMEM);
}

static int config_enc_params(EbSvtVp9EncConfiguration *param,
                             AVCodecContext *avctx)
{
    SvtContext *svt_enc = avctx->priv_data;
    int             ret;
    int        ten_bits = 0;

    param->source_width     = avctx->width;
    param->source_height    = avctx->height;

    if (avctx->pix_fmt == AV_PIX_FMT_YUV420P10LE) {
        av_log(avctx, AV_LOG_DEBUG , "Encoder 10 bits depth input\n");
        // Disable Compressed 10-bit format default
        ten_bits = 1;
    }

    // Update param from options
    param->enc_mode                 = svt_enc->enc_mode;
    param->level                    = svt_enc->level;
    param->rate_control_mode        = svt_enc->rc_mode;
    param->tune                     = svt_enc->tune;
    param->base_layer_switch_mode   = svt_enc->base_layer_switch_mode;
    param->qp                       = svt_enc->qp;

    param->target_bit_rate          = avctx->bit_rate;
    if (avctx->gop_size > 0)
        param->intra_period  = avctx->gop_size - 1;

    if (avctx->framerate.num > 0 && avctx->framerate.den > 0) {
        param->frame_rate_numerator     = avctx->framerate.num;
        param->frame_rate_denominator   = avctx->framerate.den * avctx->ticks_per_frame;
    } else {
        param->frame_rate_numerator     = avctx->time_base.den;
        param->frame_rate_denominator   = avctx->time_base.num * avctx->ticks_per_frame;
    }

    if (param->rate_control_mode) {
        param->max_qp_allowed       = avctx->qmax;
        param->min_qp_allowed       = avctx->qmin;
    }

    param->intra_refresh_type       =
        !!(avctx->flags & AV_CODEC_FLAG_CLOSED_GOP) + 1;

    if (ten_bits) {
        param->encoder_bit_depth        = 10;
    }

    ret = alloc_buffer(param, svt_enc);

    return ret;
}

static void read_in_data(EbSvtVp9EncConfiguration *config,
                         const AVFrame *frame,
                         EbBufferHeaderType *headerPtr)
{
    uint8_t is16bit = config->encoder_bit_depth > 8;
    uint64_t luma_size =
        (uint64_t)config->source_width * config->source_height<< is16bit;
    EbSvtEncInput *in_data = (EbSvtEncInput *)headerPtr->p_buffer;

    // support yuv420p and yuv420p010
    in_data->luma = frame->data[0];
    in_data->cb   = frame->data[1];
    in_data->cr   = frame->data[2];

    // stride info
    in_data->y_stride  = frame->linesize[0] >> is16bit;
    in_data->cb_stride = frame->linesize[1] >> is16bit;
    in_data->cr_stride = frame->linesize[2] >> is16bit;

    headerPtr->n_filled_len   += luma_size * 3/2u;
}

static av_cold int eb_enc_init(AVCodecContext *avctx)
{
    SvtContext   *svt_enc = avctx->priv_data;
    EbErrorType svt_ret;

    svt_enc->eos_flag = EOS_NOT_REACHED;

    svt_ret = eb_vp9_svt_init_handle(&svt_enc->svt_handle, svt_enc, &svt_enc->enc_params);
    if (svt_ret != EB_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "Error init encoder handle\n");
        goto failed;
    }

    svt_ret = config_enc_params(&svt_enc->enc_params, avctx);
    if (svt_ret != EB_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "Error configure encoder parameters\n");
        goto failed_init_handle;
    }

    svt_ret = eb_vp9_svt_enc_set_parameter(svt_enc->svt_handle, &svt_enc->enc_params);
    if (svt_ret != EB_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "Error setting encoder parameters\n");
        goto failed_init_handle;
    }

    svt_ret = eb_vp9_init_encoder(svt_enc->svt_handle);
    if (svt_ret != EB_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "Error init encoder\n");
        goto failed_init_handle;
    }

 //   if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
 //       EbBufferHeaderType* headerPtr;
 //       headerPtr->size       = sizeof(headerPtr);
 //       headerPtr->n_filled_len  = 0; /* in/out */
 //       headerPtr->p_buffer     = av_malloc(10 * 1024 * 1024);
 //       headerPtr->n_alloc_len   = (10 * 1024 * 1024);
 //
 //       if (!headerPtr->p_buffer) {
 //           av_log(avctx, AV_LOG_ERROR,
 //                  "Cannot allocate buffer size %d.\n", headerPtr->n_alloc_len);
 //           svt_ret = EB_ErrorInsufficientResources;
 //           goto failed_init_enc;
 //       }
 //
 //       svt_ret = eb_svt_enc_stream_header(svt_enc->svt_handle, &headerPtr);
 //       if (svt_ret != EB_ErrorNone) {
 //           av_log(avctx, AV_LOG_ERROR, "Error when build stream header.\n");
 //           av_freep(&headerPtr->p_buffer);
 //           goto failed_init_enc;
 //       }
 //
 //       avctx->extradata_size = headerPtr->n_filled_len;
 //       avctx->extradata = av_mallocz(avctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
 //       if (!avctx->extradata) {
 //           av_log(avctx, AV_LOG_ERROR,
 //                  "Cannot allocate VP9 header of size %d.\n", avctx->extradata_size);
 //           av_freep(&headerPtr->p_buffer);
 //           svt_ret = EB_ErrorInsufficientResources;
 //           goto failed_init_enc;
 //       }
 //       memcpy(avctx->extradata, headerPtr->p_buffer, avctx->extradata_size);
 //
 //       av_freep(&headerPtr->p_buffer);
 //   }
    return 0;

//failed_init_enc:
//    eb_deinit_encoder(svt_enc->svt_handle);
failed_init_handle:
    eb_vp9_deinit_handle(svt_enc->svt_handle);
failed:
    free_buffer(svt_enc);
    return error_mapping(svt_ret);
}

static int eb_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
    SvtContext           *svt_enc = avctx->priv_data;
    EbBufferHeaderType  *headerPtr = svt_enc->in_buf;

    if (!frame) {
        EbBufferHeaderType headerPtrLast;
        headerPtrLast.n_alloc_len   = 0;
        headerPtrLast.n_filled_len  = 0;
        headerPtrLast.n_tick_count  = 0;
        headerPtrLast.p_app_private = NULL;
        headerPtrLast.p_buffer     = NULL;
        headerPtrLast.flags      = EB_BUFFERFLAG_EOS;

        eb_vp9_svt_enc_send_picture(svt_enc->svt_handle, &headerPtrLast);
        svt_enc->eos_flag = EOS_REACHED;
        av_log(avctx, AV_LOG_DEBUG, "Finish sending frames!!!\n");
        return 0;
    }

    read_in_data(&svt_enc->enc_params, frame, headerPtr);

    headerPtr->flags         = 0;
    headerPtr->p_app_private = NULL;
    headerPtr->pts           = frame->pts;
    switch (frame->pict_type) {
    case AV_PICTURE_TYPE_I:
        headerPtr->pic_type = svt_enc->forced_idr > 0 ? EB_IDR_PICTURE : EB_I_PICTURE;
        break;
    case AV_PICTURE_TYPE_P:
        headerPtr->pic_type = EB_P_PICTURE;
        break;
    case AV_PICTURE_TYPE_B:
        headerPtr->pic_type = EB_B_PICTURE;
        break;
    default:
        headerPtr->pic_type = EB_INVALID_PICTURE;
        break;
    }
    eb_vp9_svt_enc_send_picture(svt_enc->svt_handle, headerPtr);

    return 0;
}

static int eb_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    SvtContext  *svt_enc = avctx->priv_data;
    EbBufferHeaderType   *headerPtr;
    EbErrorType          svt_ret;
    AVBufferRef *ref;

    if (EOS_TOTRIGGER == svt_enc->eos_flag) {
        pkt = NULL;
        return AVERROR_EOF;
    }

    svt_ret = eb_vp9_svt_get_packet(svt_enc->svt_handle, &headerPtr, svt_enc->eos_flag);
    if (svt_ret == EB_NoErrorEmptyQueue)
        return AVERROR(EAGAIN);

    ref = av_buffer_pool_get(svt_enc->pool);
    if (!ref) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate output packet.\n");
        eb_vp9_svt_release_out_buffer(&headerPtr);
        return AVERROR(ENOMEM);
    }
    pkt->buf = ref;
    pkt->data = ref->data;

    memcpy(pkt->data, headerPtr->p_buffer, headerPtr->n_filled_len);
    pkt->size = headerPtr->n_filled_len;
    pkt->pts  = headerPtr->pts;
    pkt->dts  = headerPtr->dts;
    if (headerPtr->pic_type == EB_IDR_PICTURE || headerPtr->pic_type == EB_I_PICTURE)
        pkt->flags |= AV_PKT_FLAG_KEY;
    if (headerPtr->pic_type == EB_NON_REF_PICTURE)
        pkt->flags |= AV_PKT_FLAG_DISPOSABLE;

    if (headerPtr->flags & EB_BUFFERFLAG_SHOW_EXT)
        pkt->flags |= AV_PKT_FLAG_SVT_VP9_EXT_ON;
    else
        pkt->flags |= AV_PKT_FLAG_SVT_VP9_EXT_OFF;

    if (EB_BUFFERFLAG_EOS == headerPtr->flags)
        svt_enc->eos_flag = EOS_TOTRIGGER;

    eb_vp9_svt_release_out_buffer(&headerPtr);
    return 0;
}

static av_cold int eb_enc_close(AVCodecContext *avctx)
{
    SvtContext *svt_enc = avctx->priv_data;

    eb_vp9_deinit_encoder(svt_enc->svt_handle);
    eb_vp9_deinit_handle(svt_enc->svt_handle);

    free_buffer(svt_enc);

    return 0;
}

#define OFFSET(x) offsetof(SvtContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "preset", "Encoding preset [1, 1]",
      OFFSET(enc_mode), AV_OPT_TYPE_INT, { .i64 = 9 }, 0, 9, VE },

    { "level", "Set level (level_idc)", OFFSET(level),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 0xff, VE, "level" },

#define LEVEL(name, value) name, NULL, 0, AV_OPT_TYPE_CONST, \
      { .i64 = value }, 0, 0, VE, "level"
        { LEVEL("1",   10) },
        { LEVEL("2",   20) },
        { LEVEL("2.1", 21) },
        { LEVEL("3",   30) },
        { LEVEL("3.1", 31) },
        { LEVEL("4",   40) },
        { LEVEL("4.1", 41) },
        { LEVEL("5",   50) },
        { LEVEL("5.1", 51) },
        { LEVEL("5.2", 52) },
        { LEVEL("6",   60) },
        { LEVEL("6.1", 61) },
        { LEVEL("6.2", 62) },
#undef LEVEL

    { "tune", "Tune mode", OFFSET(tune),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 2, VE , "tune"},
        { "vq", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 },  INT_MIN, INT_MAX, VE, "tune" },
        { "ssim", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 },  INT_MIN, INT_MAX, VE, "tune" },
        { "vmaf", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 2 },  INT_MIN, INT_MAX, VE, "tune" },

    { "rc", "Bit rate control mode", OFFSET(rc_mode),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 2, VE , "rc"},
        { "cqp", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 },  INT_MIN, INT_MAX, VE, "rc" },
        { "vbr", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 },  INT_MIN, INT_MAX, VE, "rc" },
        { "cbr", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 2 },  INT_MIN, INT_MAX, VE, "rc" },

    { "qp", "QP value for intra frames", OFFSET(qp),
      AV_OPT_TYPE_INT, { .i64 = 32 }, 0, 51, VE },

    { "bl_mode", "Random Access Prediction Structure type setting", OFFSET(base_layer_switch_mode),
      AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },

    { "forced-idr", "If forcing keyframes, force them as IDR frames.", OFFSET(forced_idr),
      AV_OPT_TYPE_BOOL,   { .i64 = 0 }, -1, 1, VE },

    {NULL},
};

static const AVClass class = {
    .class_name = "libsvt_vp9",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault eb_enc_defaults[] = {
    { "b",         "7M"    },
    { "flags",     "-cgop" },
    { "qmin",      "10"    },
    { "qmax",      "48"    },
    { NULL },
};

AVCodec ff_libsvt_vp9_encoder = {
    .name           = "libsvt_vp9",
    .long_name      = NULL_IF_CONFIG_SMALL("SVT-VP9(Scalable Video Technology for VP9) encoder"),
    .priv_data_size = sizeof(SvtContext),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_VP9,
    .init           = eb_enc_init,
    .send_frame     = eb_send_frame,
    .receive_packet = eb_receive_packet,
    .close          = eb_enc_close,
    .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS,
    .pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_YUV420P,
                                                    AV_PIX_FMT_NONE },
    .priv_class     = &class,
    .defaults       = eb_enc_defaults,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    .wrapper_name   = "libsvt_vp9",
};
