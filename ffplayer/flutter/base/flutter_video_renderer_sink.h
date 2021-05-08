//
// Created by yangbin on 2021/5/6.
//

#ifndef MEDIA_FLUTTER_BASE_FLUTTER_VIDEO_RENDERER_SINK_H_
#define MEDIA_FLUTTER_BASE_FLUTTER_VIDEO_RENDERER_SINK_H_

#include "video_renderer_sink.h"
#include "task_runner.h"

namespace media {

class FlutterVideoRendererSink : public VideoRendererSink {

 public:

  FlutterVideoRendererSink();

  void Start(RenderCallback *callback) override;

  void Stop() override;

 protected:

  virtual void DoRender(std::shared_ptr<VideoFrame> frame) = 0;

 private:

  enum State { kIdle, kRunning };
  State state_ = kIdle;

  TaskRunner *task_runner_;
  RenderCallback* render_callback_;

  void RenderTask();

  DISALLOW_COPY_AND_ASSIGN(FlutterVideoRendererSink);

};

} // namespace media

#endif //MEDIA_FLUTTER_BASE_FLUTTER_VIDEO_RENDERER_SINK_H_
