//
// Created by yangbin on 2021/5/5.
//

#ifndef MEDIA_PLAYER_SRC_VIDEO_RENDERER_SINK_H_
#define MEDIA_PLAYER_SRC_VIDEO_RENDERER_SINK_H_

#include "video_frame.h"

namespace media {

class VideoRendererSink {

 public:
  class RenderCallback {

   public:

    virtual std::shared_ptr<VideoFrame> Render() = 0;

    virtual void OnFrameDrop() = 0;

    virtual ~RenderCallback() {}

  };

  virtual void Start(RenderCallback *callback) = 0;

  virtual void Stop() = 0;

  virtual ~VideoRendererSink() {}

};

} // namespace media

#endif //MEDIA_PLAYER_SRC_VIDEO_RENDERER_SINK_H_
