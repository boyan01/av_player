//
// Created by yangbin on 2021/2/13.
//

#ifndef MEDIA_PLAYER_MEDIA_PLAYER_H
#define MEDIA_PLAYER_MEDIA_PLAYER_H

#include <memory>
#include <functional>

#include "ffplayer.h"
#include "ffp_packet_queue.h"
#include "media_clock.h"
#include "data_source.h"
#include "render_audio_base.h"
#include "render_video_base.h"

enum FFPlayerState {
  FFP_STATE_IDLE = 0,
  FFP_STATE_READY,
  FFP_STATE_BUFFERING,
  FFP_STATE_END
};

class MediaPlayer {

 private:

  std::shared_ptr<PacketQueue> audio_pkt_queue;
  std::shared_ptr<PacketQueue> video_pkt_queue;
  std::shared_ptr<PacketQueue> subtitle_pkt_queue;

  std::shared_ptr<MediaClock> clock_context;

  std::unique_ptr<DataSource> data_source{nullptr};

  std::shared_ptr<DecoderContext> decoder_context;

  std::shared_ptr<AudioRenderBase> audio_render_;
  std::shared_ptr<VideoRenderBase> video_render_;

  std::shared_ptr<MessageContext> message_context;

 public:

  MediaPlayer(std::shared_ptr<VideoRenderBase> video_render, std::shared_ptr<AudioRenderBase> audio_render);

  ~MediaPlayer();

  static void GlobalInit();

 public:
  PlayerConfiguration start_configuration{};

  // buffered position in seconds. -1 if not avalible
  int64_t buffered_position = -1;

  FFPlayerState state = FFP_STATE_IDLE;

  bool paused = false;

  void TogglePause();

  int OpenDataSource(const char *filename);

  double GetCurrentPosition();

  bool IsPaused() const;

  int GetVolume();

  void SetVolume(int volume);

  void SetMute(bool mute);

  bool IsMuted();

  double GetDuration();

  void Seek(double position);

  void SeekToChapter(int chapter);

  int GetCurrentChapter();

  int GetChapterCount();

  void SetMessageHandleCallback(std::function<void(int what, int64_t arg1, int64_t arg2)> message_callback);

  double GetVideoAspectRatio();

  const char *GetUrl();

  const char *GetMetadataDict(const char *key);

  VideoRenderBase *GetVideoRender();

  /**
   * Dump player status information to console.
   */
  void DumpStatus();

};

#endif //MEDIA_PLAYER_MEDIA_PLAYER_H
