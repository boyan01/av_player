//
// Created by yangbin on 2021/5/9.
//

#ifndef MEDIA_PLAYER_SRC_DECODER_BUFFER_H_
#define MEDIA_PLAYER_SRC_DECODER_BUFFER_H_

#include <base/basictypes.h>

#include "ffmpeg_deleters.h"

namespace media {

class DecoderBuffer {

 public:
  explicit DecoderBuffer(std::unique_ptr<AVPacket, AVPacketDeleter> av_packet);

  static std::shared_ptr<DecoderBuffer> CreateEOSBuffer();

  size_t data_size();

  double timestamp() const {
    return timestamp_;
  }

  void set_timestamp(double timestamp) {
    timestamp_ = timestamp;
  }

  AVPacket *av_packet() {
    return av_packet_;
  }

  bool end_of_stream();
  virtual ~DecoderBuffer();

 private:

  AVPacket *av_packet_;

  // pts.
  double timestamp_;

  DELETE_COPY_AND_ASSIGN(DecoderBuffer);

};

}

#endif //MEDIA_PLAYER_SRC_DECODER_BUFFER_H_
