//
// Created by yangbin on 2021/3/6.
//

#ifndef MEDIA_AUDIO_AUDIO_RENDER_SDL_2_H_
#define MEDIA_AUDIO_AUDIO_RENDER_SDL_2_H_

#include "audio_render_basic.h"

class AudioRenderSdl2 : public BasicAudioRender {

 private:
  SDL_AudioDeviceID audio_device_id_ = -1;
 public:

  void Start() const override;

  void Pause() const override;

 protected:

  int OpenAudioDevice(int64_t wanted_channel_layout,
                      int wanted_nb_channels,
                      int wanted_sample_rate,
                      AudioParams &device_output) override;

};

#endif //MEDIA_AUDIO_AUDIO_RENDER_SDL_2_H_
