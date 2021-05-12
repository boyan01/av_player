//
// Created by yangbin on 2021/2/13.
//

#include "base/logging.h"
#include "base/lambda.h"

#include "media_player.h"

#include "ffp_utils.h"
#include "ffp_define.h"

extern "C" {
#include "libavutil/bprint.h"
}

#include "blocking_url_protocol.h"
#include "file_data_source.h"

namespace media {

MediaPlayer::MediaPlayer(
    std::unique_ptr<VideoRendererSink> video_renderer_sink,
    std::shared_ptr<AudioRendererSink> audio_renderer_sink
) {
  task_runner_ = TaskRunner::prepare_looper("media_player");
  task_runner_->PostTask(FROM_HERE, [&]() {
    Initialize();
  });
  decoder_task_runner_ = TaskRunner::prepare_looper("decoder");
  audio_renderer_ = std::make_shared<AudioRenderer>(decoder_task_runner_, std::move(audio_renderer_sink));
  video_renderer_ = std::make_shared<VideoRenderer>(decoder_task_runner_, std::move(video_renderer_sink));
}

void MediaPlayer::Initialize() {
  DCHECK_EQ(state_, kUninitialized);
  DCHECK(task_runner_->BelongsToCurrentThread());

  audio_pkt_queue = std::make_shared<PacketQueue>();
  video_pkt_queue = std::make_shared<PacketQueue>();
  subtitle_pkt_queue = std::make_shared<PacketQueue>();

  auto sync_type_confirm = [this](int av_sync_type) -> int {
    return AV_SYNC_AUDIO_MASTER;
//    if (data_source == nullptr) {
//      return av_sync_type;
//    }
//    if (av_sync_type == AV_SYNC_VIDEO_MASTER) {
//      if (data_source->ContainVideoStream()) {
//        return AV_SYNC_VIDEO_MASTER;
//      } else {
//        return AV_SYNC_AUDIO_MASTER;
//      }
//    } else if (av_sync_type == AV_SYNC_AUDIO_MASTER) {
//      if (data_source->ContainAudioStream()) {
//        return AV_SYNC_AUDIO_MASTER;
//      } else {
//        return AV_SYNC_EXTERNAL_CLOCK;
//      }
//    } else {
//      return AV_SYNC_EXTERNAL_CLOCK;
//    }
  };
  clock_context = std::make_shared<MediaClock>(&audio_pkt_queue->serial, &video_pkt_queue->serial,
                                               sync_type_confirm);
  task_runner_->PostTask(FROM_HERE, std::bind(&MediaPlayer::DumpMediaClockStatus, this));

  state_ = kIdle;

}

MediaPlayer::~MediaPlayer() {
  task_runner_->Quit();
}

void MediaPlayer::SetPlayWhenReady(bool play_when_ready) {
  std::lock_guard<std::mutex> lock_guard(player_mutex_);
  if (play_when_ready_pending_ == play_when_ready) {
    return;
  }
  play_when_ready_pending_ = play_when_ready;
  task_runner_->PostTask(FROM_HERE, [&]() {
    SetPlayWhenReadyTask(play_when_ready_pending_);
  });
}

void MediaPlayer::SetPlayWhenReadyTask(bool play_when_ready) {
  if (play_when_ready_ == play_when_ready) {
    return;
  }
  play_when_ready_ = play_when_ready;
  if (demuxer_) {
//    data_source->paused = !play_when_ready;
  }
  if (state_ != kPrepared) {
    return;
  }
  if (!play_when_ready) {
    StopRenders();
  } else {
    StartRenders();
  }
}

void MediaPlayer::PauseClock(bool pause) {
  if (clock_context->paused == pause) {
    return;
  }
  if (clock_context->paused) {
    if (video_renderer_) {
//      video_renderer_->frame_timer_ += get_relative_time() - clock_context->GetVideoClock()->last_updated;
    }
//    if (data_source && data_source->read_pause_return != AVERROR(ENOSYS)) {
//      clock_context->GetVideoClock()->paused = 0;
//    }
    clock_context->GetVideoClock()->SetClock(clock_context->GetVideoClock()->GetClock(),
                                             clock_context->GetVideoClock()->serial);
  }
  clock_context->GetExtClock()->SetClock(clock_context->GetExtClock()->GetClock(),
                                         clock_context->GetExtClock()->serial);
  clock_context->paused = pause;
  clock_context->GetExtClock()->paused = pause;
  clock_context->GetAudioClock()->paused = pause;
  clock_context->GetVideoClock()->paused = pause;
}

int MediaPlayer::OpenDataSource(const char *filename) {
  if (demuxer_) {
    av_log(nullptr, AV_LOG_ERROR, "can not open file multi-times.\n");
    return -1;
  }
  task_runner_->PostTask(FROM_HERE, [&, filename]() {
    OpenDataSourceTask(filename);
  });
  return 0;
}

void MediaPlayer::OpenDataSourceTask(const char *filename) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK_EQ(state_, kIdle);

  DLOG(INFO) << "open file: " << filename;

  auto file_data_source = new FileDataSource();
  auto ret = file_data_source->Initialize(filename);
  DCHECK(ret) << "open file failed";

  state_ = kPreparing;
  demuxer_ = std::make_shared<Demuxer>(decoder_task_runner_, file_data_source, [](std::unique_ptr<MediaTracks> tracks) {
    DLOG(INFO) << "on tracks update.";
  });
  demuxer_->Initialize(this, bind_weak(&MediaPlayer::OnDataSourceOpen, shared_from_this()));
}

void MediaPlayer::OnDataSourceOpen(int open_status) {
  DCHECK_EQ(state_, kPreparing);
  if (open_status >= 0) {
    DLOG(INFO) << "Open DataSource Succeed";
    task_runner_->PostTask(FROM_HERE, bind_weak(&MediaPlayer::InitVideoRender, shared_from_this()));
  } else {
    state_ = kIdle;
    DLOG(ERROR) << "Open DataSource Failed";
  }
}

void MediaPlayer::InitVideoRender() {
  auto stream = demuxer_->GetFirstStream(DemuxerStream::Video);
  if (stream) {
    video_renderer_->Initialize(stream,
                                clock_context,
                                bind_weak(&MediaPlayer::OnVideoRendererInitialized, shared_from_this()));
  } else {
    DLOG(WARNING) << "data source does not contains video stream";
    task_runner_->PostTask(FROM_HERE, bind_weak(&MediaPlayer::InitAudioRender, shared_from_this()));
  }
}

void MediaPlayer::OnVideoRendererInitialized(bool success) {
  if (!success) {
    state_ = kIdle;
    return;
  }
  task_runner_->PostTask(FROM_HERE, bind_weak(&MediaPlayer::InitAudioRender, shared_from_this()));
}

void MediaPlayer::InitAudioRender() {
  auto stream = demuxer_->GetFirstStream(DemuxerStream::Audio);
  if (stream) {
    audio_renderer_->Initialize(stream, clock_context,
                                bind_weak(&MediaPlayer::OnAudioRendererInitialized, shared_from_this()));
  }
}

void MediaPlayer::OnAudioRendererInitialized(bool success) {
  DLOG(INFO) << __func__ << " : " << success;
  if (success) {
    state_ = kPrepared;
    if (play_when_ready_) {
      StartRenders();
    }
  } else {
    state_ = kIdle;
  }
}

void MediaPlayer::DumpStatus() {

}

double MediaPlayer::GetCurrentPosition() {
  if (state_ == kUninitialized) {
    return 0;
  }

  double position = clock_context->GetMasterClock();
  if (isnan(position)) {
    if (demuxer_) {
//      position = (double) data_source->GetSeekPosition() / AV_TIME_BASE;
    } else {
      position = 0;
    }
  }
  return position;
}

int MediaPlayer::GetVolume() {
  CHECK_VALUE_WITH_RETURN(audio_renderer_, 0);
//  return audio_render_->GetVolume();
}

void MediaPlayer::SetVolume(int volume) {
  CHECK_VALUE(audio_renderer_);
//  audio_render_->SetVolume(volume);
}

void MediaPlayer::SetMute(bool mute) {
  CHECK_VALUE(audio_renderer_);
//  audio_render_->SetMute(mute);
}

bool MediaPlayer::IsMuted() {
  CHECK_VALUE_WITH_RETURN(audio_renderer_, true);
//  return audio_render_->IsMute();
}

double MediaPlayer::GetDuration() {
//  CHECK_VALUE_WITH_RETURN(data_source, -1);
//  return data_source->GetDuration();
  return 100;
}

void MediaPlayer::Seek(double position) {
//  CHECK_VALUE(data_source);
  ChangePlaybackState(MediaPlayerState::BUFFERING);
//  data_source->Seek(position);
}

void MediaPlayer::SeekToChapter(int chapter) {
//  CHECK_VALUE(data_source);
//  data_source->SeekToChapter(chapter);
}

int MediaPlayer::GetCurrentChapter() {
//  CHECK_VALUE_WITH_RETURN(data_source, -1);
  int64_t pos = GetCurrentPosition() * AV_TIME_BASE;
//  return data_source->GetChapterByPosition(pos);
  return -1;
}

int MediaPlayer::GetChapterCount() {
//  CHECK_VALUE_WITH_RETURN(data_source, -1);
//  return data_source->GetChapterCount();
  return -1;
}

void MediaPlayer::SetMessageHandleCallback(std::function<void(int what, int64_t arg1, int64_t arg2)> message_callback) {
  message_callback_external_ = std::move(message_callback);
}

const char *MediaPlayer::GetUrl() {
//  CHECK_VALUE_WITH_RETURN(data_source, nullptr);
//  return data_source->GetFileName();
  return "";
}

const char *MediaPlayer::GetMetadataDict(const char *key) {
//  CHECK_VALUE_WITH_RETURN(data_source, nullptr);
//  return data_source->GetMetadataDict(key);
  return "";
}

void MediaPlayer::GlobalInit() {
  av_log_set_flags(AV_LOG_SKIP_REPEATED);
  av_log_set_level(AV_LOG_INFO);
  /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
  avdevice_register_all();
#endif
  avformat_network_init();

}

void MediaPlayer::ChangePlaybackState(MediaPlayerState state) {
  if (player_state_ == state) {
    return;
  }
  player_state_ = state;
}

void MediaPlayer::StopRenders() {
  av_log(nullptr, AV_LOG_INFO, "StopRenders\n");
  PauseClock(true);
  if (audio_renderer_) {
//    audio_render_->Stop();
  }
  if (video_renderer_) {
    video_renderer_->Stop();
  }
}

void MediaPlayer::StartRenders() {
  DLOG(INFO) << "StartRenders";
  PauseClock(false);
  if (audio_renderer_) {
    audio_renderer_->Start();
  }
  // TODO fixme
  if (video_renderer_ && demuxer_->GetFirstStream(DemuxerStream::Video)) {
    video_renderer_->Start();
  }
}

static inline bool check_queue_is_ready(const std::shared_ptr<PacketQueue> &queue, bool has_stream) {
  static const int min_frames = 2;
  return queue->nb_packets > min_frames || !has_stream || queue->abort_request;
}

void MediaPlayer::OnFirstFrameLoaded(int width, int height) {
  if (on_video_size_changed_) {
    on_video_size_changed_(width, height);
  }
}

void MediaPlayer::OnFirstFrameRendered(int width, int height) {
  if (on_video_size_changed_) {
    on_video_size_changed_(width, height);
  }

}

void MediaPlayer::DumpMediaClockStatus() {

  DLOG(INFO) << "DumpMediaClockStatus: master clock = "
             << clock_context->GetMasterClock();

  task_runner_->PostDelayedTask(FROM_HERE, TimeDelta(1000000), std::bind(&MediaPlayer::DumpMediaClockStatus, this));
}

void MediaPlayer::SetDuration(double duration) {

}

void MediaPlayer::OnDemuxerError(PipelineStatus error) {

}

}
