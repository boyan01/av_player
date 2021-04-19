//
// Created by boyan on 2021/2/9.
//

#ifndef FFPLAYER_FFP_DECODER_H
#define FFPLAYER_FFP_DECODER_H

#include <functional>
#include <condition_variable>

#include "ffp_packet_queue.h"
#include "ffp_frame_queue.h"
#include "ffp_define.h"
#include "render_base.h"
#include "decoder_video.h"
#include "decoder_audio.h"
#include "decoder_base.h"
#include "ffmpeg_deleters.h"
#include "demuxer_stream.h"
#include "ffmpeg_decoding_loop.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
};

namespace media {
class DecoderContext {
 public:
  /**
   * low resolution decoding, 1-> 1/2 size, 2->1/4 size
   */
  int low_res = 0;
  bool fast = false;

 private:
  AudioDecoder *audio_decoder = nullptr;
  VideoDecoder *video_decoder = nullptr;

  std::shared_ptr<BasicAudioRender> audio_render;
  std::shared_ptr<VideoRenderBase> video_render;

  std::shared_ptr<MediaClock> clock_ctx;

  /**
   * callback when decoder blocking.
   */
  std::function<void()> on_decoder_blocking_;

 private:
  int StartAudioDecoder(unique_ptr_d<AVCodecContext> codec_ctx, std::unique_ptr<DecodeParams> decode_params);

 public:

  DecoderContext(
      std::shared_ptr<BasicAudioRender> audio_render_,
      std::shared_ptr<VideoRenderBase> video_render_,
      std::shared_ptr<MediaClock> clock_ctx_,
      std::function<void()> on_decoder_blocking
  );

  ~DecoderContext();

  int StartDecoder(std::unique_ptr<DecodeParams> decode_params);

  int InitVideoDecoder(VideoDecodeConfig config);

  void StartVideoDecoder(std::shared_ptr<DemuxerStream> stream);

 private:

  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> video_codec_context_;
  TaskRunner *decode_task_runner_ = nullptr;
  std::shared_ptr<DemuxerStream> video_stream_;
  AVFrame *video_temp_frame_ = nullptr;
  VideoDecodeConfig video_decode_config_;
  std::unique_ptr<FFmpegDecodingLoop> video_decoding_loop_;

  void VideoDecodeTask();

};
}

#endif //FFPLAYER_FFP_DECODER_H
