/*
 * This file is part of Moonlight Embedded.
 *
 * Based on Moonlight Pc implementation
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "ffmpeg.h"

#ifdef HAVE_VAAPI
#include "ffmpeg_vaapi.h"
#endif

#include <Limelight.h>
#include <libavcodec/avcodec.h>

#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>

// General decoder and renderer state
static AVPacket* pkt;
static const AVCodec* decoder;
static AVCodecContext* decoder_ctx;
static AVFrame** dec_frames;
static AVBufferRef *hw_device_ctx = NULL;
static enum AVPixelFormat hw_pix_fmt;
static AVFrame *sw_frame;

static int dec_frames_cnt;
static int current_frame, next_frame;

enum decoders ffmpeg_decoder;

#define BYTES_PER_PIXEL 4

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
  const enum AVPixelFormat *p;
  for (p = pix_fmts; *p != -1; p++) {
    if (*p == hw_pix_fmt)
      return *p;
  }

  fprintf(stderr, "Failed to get HW surface format.\n");
  return AV_PIX_FMT_NONE;
}

static bool hwaccel_setup(const AVCodec* decoder) {
  enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;

  for (int i = 0;; i++) {
    const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
    if (!config)
      break;

    if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
      fprintf(stderr, "Using hwaccel type %s\n", av_hwdevice_get_type_name(config->device_type));
      type = config->device_type;
      hw_pix_fmt = config->pix_fmt;

      int err = av_hwdevice_ctx_create(&hw_device_ctx, type, NULL, NULL, 0);
      if (err < 0) {
        fprintf(stderr, "Failed to create specified HW device.\n");
        hw_pix_fmt = AV_PIX_FMT_NONE;
        type = AV_HWDEVICE_TYPE_NONE;
        continue;
      }

      decoder_ctx->get_format = get_hw_format;
      decoder_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
      break;
    }
  }

  if (type == AV_HWDEVICE_TYPE_NONE) {
    fprintf(stderr, "Decoder %s does not support any of the available hwaccel methods\n", decoder->name);
    return false;
  }

  return true;
}

// This function must be called before
// any other decoding functions
int ffmpeg_init(int videoFormat, int width, int height, int perf_lvl, int buffer_count, int thread_count) {
  // Initialize the avcodec library and register codecs
  av_log_set_level(AV_LOG_QUIET);
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,10,100)
  avcodec_register_all();
#endif

  pkt = av_packet_alloc();
  if (pkt == NULL) {
    printf("Couldn't allocate packet\n");
    return -1;
  }

  ffmpeg_decoder = perf_lvl & VAAPI_ACCELERATION ? VAAPI : SOFTWARE;

  for (int try = 0; try < 5; try++) {
    if (videoFormat & VIDEO_FORMAT_MASK_H264) {
      if (ffmpeg_decoder == SOFTWARE) {
        if (try == 0) decoder = avcodec_find_decoder_by_name("h264_nvv4l2"); // Tegra
        if (try == 1) decoder = avcodec_find_decoder_by_name("h264_nvmpi"); // Tegra
        if (try == 2) decoder = avcodec_find_decoder_by_name("h264_omx"); // VisionFive
        if (try == 3) decoder = avcodec_find_decoder_by_name("h264_v4l2m2m"); // Stateful V4L2
      }
      if (try == 4) decoder = avcodec_find_decoder_by_name("h264"); // Software and hwaccel
    } else if (videoFormat & VIDEO_FORMAT_MASK_H265) {
      if (ffmpeg_decoder == SOFTWARE) {
        if (try == 0) decoder = avcodec_find_decoder_by_name("hevc_nvv4l2"); // Tegra
        if (try == 1) decoder = avcodec_find_decoder_by_name("hevc_nvmpi"); // Tegra
        if (try == 2) decoder = avcodec_find_decoder_by_name("hevc_omx"); // VisionFive
        if (try == 3) decoder = avcodec_find_decoder_by_name("hevc_v4l2m2m"); // Stateful V4L2
      }
      if (try == 4) decoder = avcodec_find_decoder_by_name("hevc"); // Software and hwaccel
    } else if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
      if (ffmpeg_decoder == SOFTWARE) {
        if (try == 0) decoder = avcodec_find_decoder_by_name("libdav1d");
      }
      if (try == 1) decoder = avcodec_find_decoder_by_name("av1"); // Hwaccel
    } else {
      printf("Video format not supported\n");
      return -1;
    }

    // Skip this decoder if it isn't compiled into FFmpeg
    if (!decoder) {
      continue;
    }
    fprintf(stderr, "Trying decoder %s\n", decoder->name);

    decoder_ctx = avcodec_alloc_context3(decoder);
    if (decoder_ctx == NULL) {
      printf("Couldn't allocate context\n");
      return -1;
    }

    // Use low delay decoding
    decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    // Allow display of corrupt frames and frames missing references
    decoder_ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    decoder_ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;

    // Report decoding errors to allow us to request a key frame
    decoder_ctx->err_recognition = AV_EF_EXPLODE;

    if (perf_lvl & SLICE_THREADING) {
      decoder_ctx->thread_type = FF_THREAD_SLICE;
      decoder_ctx->thread_count = thread_count;
    } else {
      decoder_ctx->thread_count = 1;
    }

    decoder_ctx->width = width;
    decoder_ctx->height = height;

    // Configure hardware decoding if supported
    bool hwaccel = false;
    if (avcodec_get_hw_config(decoder, 0)) {
      if (hwaccel_setup(decoder)) {
        hwaccel = true;
      }
    }

    if (!hwaccel) {
      decoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    }

    int err = avcodec_open2(decoder_ctx, decoder, NULL);
    if (err < 0) {
      printf("Couldn't open codec: %s\n", decoder->name);
      avcodec_free_context(&decoder_ctx);
      continue;
    }
  }

  if (decoder == NULL) {
    printf("Couldn't find decoder\n");
    return -1;
  }

  fprintf(stderr, "Using FFmpeg decoder: %s\n", decoder->name);

  dec_frames_cnt = buffer_count;
  dec_frames = malloc(buffer_count * sizeof(AVFrame*));
  if (dec_frames == NULL) {
    fprintf(stderr, "Couldn't allocate frames");
    return -1;
  }

  for (int i = 0; i < buffer_count; i++) {
    dec_frames[i] = av_frame_alloc();
    if (dec_frames[i] == NULL) {
      fprintf(stderr, "Couldn't allocate frame");
      return -1;
    }
  }
  if (hw_device_ctx) {
    sw_frame = av_frame_alloc();
  }

  #ifdef HAVE_VAAPI
  if (ffmpeg_decoder == VAAPI)
    vaapi_init(decoder_ctx);
  #endif

  return 0;
}

// This function must be called after
// decoding is finished
void ffmpeg_destroy(void) {
  if (sw_frame) {
    av_frame_free(&sw_frame);
  }
  if (hw_device_ctx) {
    av_buffer_unref(&hw_device_ctx);
  }
  av_packet_free(&pkt);
  if (decoder_ctx) {
    avcodec_free_context(&decoder_ctx);
  }
  if (dec_frames) {
    for (int i = 0; i < dec_frames_cnt; i++) {
      if (dec_frames[i])
        av_frame_free(&dec_frames[i]);
    }
  }
}

AVFrame* ffmpeg_get_frame(bool native_frame) {
  AVFrame *frame = dec_frames[next_frame];
  int err = avcodec_receive_frame(decoder_ctx, frame);
  if (err == 0) {
    current_frame = next_frame;
    next_frame = (current_frame+1) % dec_frames_cnt;

    if (ffmpeg_decoder == SOFTWARE || native_frame) {
      AVFrame *tmp_frame = NULL;
      if (frame->format == hw_pix_fmt) {
        /* retrieve data from GPU to CPU */
        if (av_hwframe_transfer_data(sw_frame, frame, 0) < 0) {
          fprintf(stderr, "Error transferring the data to system memory\n");
          return NULL;
        }
        tmp_frame = sw_frame;
      } else {
        tmp_frame = frame;
      }
      return tmp_frame;
    }
  } else if (err != AVERROR(EAGAIN)) {
    char errorstring[512];
    av_strerror(err, errorstring, sizeof(errorstring));
    fprintf(stderr, "Receive failed - %d/%s\n", err, errorstring);
  }
  return NULL;
}

// packets must be decoded in order
// indata must be inlen + AV_INPUT_BUFFER_PADDING_SIZE in length
int ffmpeg_decode(unsigned char* indata, int inlen) {
  int err;

  pkt->data = indata;
  pkt->size = inlen;

  err = avcodec_send_packet(decoder_ctx, pkt);
  if (err < 0) {
    char errorstring[512];
    av_strerror(err, errorstring, sizeof(errorstring));
    fprintf(stderr, "Decode failed - %s\n", errorstring);
  }

  return err < 0 ? err : 0;
}
