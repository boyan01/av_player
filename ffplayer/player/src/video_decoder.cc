//
// Created by yangbin on 2021/5/3.
//

#include "video_decoder.h"

#include "base/logging.h"

#include "ffp_utils.h"

namespace media {

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() = default;

int VideoDecoder::Initialize(VideoDecodeConfig config,
                             DemuxerStream *stream,
                             VideoDecoder::OutputCallback output_callback) {
  DCHECK(!codec_context_);
  DCHECK(output_callback);

  codec_context_ = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>(avcodec_alloc_context3(nullptr));
  output_callback_ = std::move(output_callback);

  auto ret = avcodec_parameters_to_context(codec_context_.get(), &config.codec_parameters());
  DCHECK_GE(ret, 0);

  codec_context_->codec_id = config.codec_id();
  codec_context_->pkt_timebase = config.time_base();

  auto *codec = avcodec_find_decoder(config.codec_id());
  DCHECK(codec) << "No decoder could be found for CodecId:" << avcodec_get_name(config.codec_id());

  if (codec == nullptr) {
    codec_context_.reset();
    return -1;
  }

  codec_context_->codec_id = codec->id;
  int stream_lower = config.low_res();
  DCHECK_LE(stream_lower, codec->max_lowres)
      << "The maximum value for lowres supported by the decoder is " << codec->max_lowres
      << ", but is " << stream_lower;
  if (stream_lower > codec->max_lowres) {
    stream_lower = codec->max_lowres;
  }
  codec_context_->lowres = stream_lower;
  if (config.fast()) {
    codec_context_->flags2 |= AV_CODEC_FLAG2_FAST;
  }

  ret = avcodec_open2(codec_context_.get(), codec, nullptr);
  DCHECK_GE(ret, 0) << "can not open avcodec, reason: " << av_err_to_str(ret);
  if (ret < 0) {
    codec_context_.reset();
    return ret;
  }

  ffmpeg_decoding_loop_ = std::make_unique<FFmpegDecodingLoop>(codec_context_.get(), true);
//  video_render->SetMaxFrameDuration(video_decode_config_.max_frame_duration());
  video_decode_config_ = config;

  stream_ = stream;

  return 0;
}

void VideoDecoder::Decode(std::shared_ptr<DecoderBuffer> decoder_buffer) {
  DCHECK(!decoder_buffer->end_of_stream());
  switch (ffmpeg_decoding_loop_->DecodePacket(
      decoder_buffer->av_packet(), std::bind(&VideoDecoder::OnFrameAvailable, this, std::placeholders::_1))) {
    case FFmpegDecodingLoop::DecodeStatus::kFrameProcessingFailed :return;
    case FFmpegDecodingLoop::DecodeStatus::kSendPacketFailed: {
      DLOG(ERROR) << "Failed to send video packet for decoding";
      return;
    }
    case FFmpegDecodingLoop::DecodeStatus::kDecodeFrameFailed: {
      DLOG(ERROR) << " failed to decode a video frame: "
                  << av_err_to_str(ffmpeg_decoding_loop_->last_av_error_code());
      return;
    }
    case FFmpegDecodingLoop::DecodeStatus::kOkay:break;
  }
}

bool VideoDecoder::OnFrameAvailable(AVFrame *frame) {
  auto frame_rate = video_decode_config_.frame_rate();
  auto duration = (frame_rate.num && frame_rate.den ? av_q2d(AVRational{frame_rate.den, frame_rate.num}) : 0);
  auto pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : double(frame->pts) * av_q2d(video_decode_config_.time_base());

  std::shared_ptr<VideoFrame> video_frame = std::make_shared<VideoFrame>(frame, pts, duration, 0);
  output_callback_(std::move(video_frame));
  return false;
}

void VideoDecoder::Flush() {
  avcodec_flush_buffers(codec_context_.get());
}

} // namespace media