//
// Created by yangbin on 2021/5/2.
//

#ifndef MEDIA_PLAYER_SRC_AUDIO_BUFFER_H_
#define MEDIA_PLAYER_SRC_AUDIO_BUFFER_H_

#include "base/basictypes.h"

namespace media {

class AudioBuffer {

 public:

  AudioBuffer(uint8 *data, int size, double pts, int bytes_per_sec);

  virtual ~AudioBuffer();

  /**
   * Read data to stream.
   *
   * @param dest Output destination.
   * @param size  The size to read.
   * @return The size read done.
   */
  int Read(uint8 *dest, int size, double volume);

  int size() const {
    return size_;
  }

  /**
   * Presentation timestamp. Could be NAN.
   */
  double pts() const {
    return pts_;
  }

  /**
   * @return true if all data has been read by [Read].
   */
  bool IsConsumed() const {
    return size_ == read_cursor_;
  }

  /**
   * The presentation time of start of cursor.
   */
  double PtsFromCursor() const {
    return pts_ + double(read_cursor_) / bytes_per_sec_;
  }

 private:

  uint8 *data_;
  int size_;

  double pts_;

  int read_cursor_ = 0;

  int bytes_per_sec_;

};

}

#endif //MEDIA_PLAYER_SRC_AUDIO_BUFFER_H_
