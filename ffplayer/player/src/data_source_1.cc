//
// Created by boyan on 2021/2/10.
//

#include "ffplayer.h"
#include "data_source_1.h"
#include "ffp_utils.h"

#include "media_player.h"
#include "ffp_define.h"

#include "base/logging.h"

namespace media {

#define MIN_FRAMES 25
#define MAX_QUEUE_SIZE (15 * 1024 * 1024)

static const AVRational av_time_base_q_ = {1, AV_TIME_BASE};

static inline int stream_has_enough_packets(AVStream *st, int stream_id, const std::shared_ptr<PacketQueue> &queue) {
  return stream_id < 0 ||
      queue->abort_request ||
      (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
      queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

static int is_realtime(AVFormatContext *s) {
  if (!strcmp(s->iformat->name, "rtp") || !strcmp(s->iformat->name, "rtsp") || !strcmp(s->iformat->name, "sdp"))
    return 1;

  if (s->pb && (!strncmp(s->url, "rtp:", 4) || !strncmp(s->url, "udp:", 4)))
    return 1;
  return 0;
}

DataSource1::DataSource1(const char *filename, AVInputFormat *format)
    : in_format(format), video_decode_config_(), audio_decode_config_() {
  memset(wanted_stream_spec, 0, sizeof wanted_stream_spec);
  this->filename = av_strdup(filename);
  continue_read_thread_ = std::make_shared<std::condition_variable_any>();
}

void DataSource1::Open(DataSource1::OpenCallback open_callback) {
  DCHECK(filename);
  open_callback_ = std::move(open_callback);
  read_tid = new std::thread(&DataSource1::ReadThread, this);
}

DataSource1::~DataSource1() {
  av_free(filename);
  if (read_tid && read_tid->joinable()) {
    abort_request = true;
    continue_read_thread_->notify_all();
    read_tid->join();
  }
  if (format_ctx_) {
    avformat_free_context(format_ctx_);
    format_ctx_ = nullptr;
  }
}

void DataSource1::ReadThread() {
  DCHECK(open_callback_);

  update_thread_name("read_source");
  av_log(nullptr, AV_LOG_DEBUG, "DataSource1 Read OnStart: %s \n", filename);
  int st_index[AVMEDIA_TYPE_NB] = {-1, -1, -1, -1, -1};
  std::mutex wait_mutex;

  if (PrepareFormatContext() < 0) {
    std::move(open_callback_)(-1);
    return;
  }
  OnFormatContextOpen();

  ReadStreamInfo(st_index);
  OnStreamInfoLoad(st_index);

  if (OpenStreams(st_index) < 0) {
    // todo destroy streams;
    std::move(open_callback_)(-1);
    return;
  }

  std::move(open_callback_)(0);

  ReadStreams(wait_mutex);

  av_log(nullptr, AV_LOG_INFO, "thread: read_source done.\n");
}

int DataSource1::PrepareFormatContext() {
  format_ctx_ = avformat_alloc_context();
  if (!format_ctx_) {
    av_log(nullptr, AV_LOG_FATAL, "Could not allocate context.\n");
    return -1;
  }
  format_ctx_->interrupt_callback.opaque = this;
  format_ctx_->interrupt_callback.callback = [](void *ctx) -> int {
    auto *source = static_cast<DataSource1 *>(ctx);
    return source->abort_request;
  };
  auto err = avformat_open_input(&format_ctx_, filename, in_format, nullptr);
  if (err < 0) {
    av_log(nullptr, AV_LOG_ERROR, "can not open file %s: %s \n", filename, av_err_to_str(err));
    return -1;
  }

  if (gen_pts) {
    format_ctx_->flags |= AVFMT_FLAG_GENPTS;
  }

  av_format_inject_global_side_data(format_ctx_);

  // find stream info for av file. this is useful for formats with no headers such as MPEG.
  if (find_stream_info && avformat_find_stream_info(format_ctx_, nullptr) < 0) {
    avformat_free_context(format_ctx_);
    format_ctx_ = nullptr;
    return -1;
  }

  if (format_ctx_->pb) {
    format_ctx_->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the END
  }

  if (seek_by_bytes < 0) {
    seek_by_bytes = (format_ctx_->iformat->flags & AVFMT_TS_DISCONT) != 0
        && strcmp("ogg", format_ctx_->iformat->name) != 0;
  }

  return 0;
}

void DataSource1::OnFormatContextOpen() {
//  msg_ctx->NotifyMsg(FFP_MSG_AV_METADATA_LOADED);

  /* if seeking requested, we execute it */
  if (start_time != AV_NOPTS_VALUE) {
    auto timestamp = start_time;
    if (format_ctx_->start_time != AV_NOPTS_VALUE) {
      timestamp += format_ctx_->start_time;
    }
    auto ret = avformat_seek_file(format_ctx_, -1, INT16_MIN, timestamp, INT64_MAX, 0);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_WARNING, "%s: could not seek to position %0.3f. err = %s\n",
             filename, (double) timestamp / AV_TIME_BASE, av_err_to_str(ret));
    }
  }

  realtime_ = is_realtime(format_ctx_);
  if (!infinite_buffer && realtime_) {
    infinite_buffer = true;
  }

  if (configuration.show_status) {
    av_dump_format(format_ctx_, 0, filename, 0);
  }
}

int DataSource1::ReadStreamInfo(int st_index[AVMEDIA_TYPE_NB]) {
  for (int i = 0; i < format_ctx_->nb_streams; ++i) {
    auto *st = format_ctx_->streams[i];
    auto type = st->codecpar->codec_type;
    st->discard = AVDISCARD_ALL;
    if (type > 0 && wanted_stream_spec[type] && st_index[type] == -1) {
      if (avformat_match_stream_specifier(format_ctx_, st, wanted_stream_spec[type]) > 0) {
        st_index[type] = i;
      }
    }
  }

  for (int i = 0; i < AVMEDIA_TYPE_NB; ++i) {
    if (wanted_stream_spec[i] && st_index[i] == -1) {
      av_log(nullptr, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n",
             wanted_stream_spec[i], av_get_media_type_string(static_cast<AVMediaType>(i)));
      st_index[i] = INT_MAX;
    }
  }

  if (!configuration.video_disable) {
    st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO,
                                                       st_index[AVMEDIA_TYPE_VIDEO], -1,
                                                       nullptr, 0);
  }
  if (!configuration.audio_disable) {
    st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_AUDIO,
                                                       st_index[AVMEDIA_TYPE_AUDIO],
                                                       st_index[AVMEDIA_TYPE_VIDEO],
                                                       nullptr, 0);
  }
  if (!configuration.video_disable && !configuration.subtitle_disable) {
    st_index[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_SUBTITLE,
                                                          st_index[AVMEDIA_TYPE_SUBTITLE],
                                                          (st_index[AVMEDIA_TYPE_AUDIO] >= 0
                                                           ? st_index[AVMEDIA_TYPE_AUDIO]
                                                           : st_index[AVMEDIA_TYPE_VIDEO]),
                                                          nullptr, 0);
  }

  return 0;
}

void DataSource1::OnStreamInfoLoad(const int st_index[AVMEDIA_TYPE_NB]) {
  if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
    auto *st = format_ctx_->streams[st_index[AVMEDIA_TYPE_VIDEO]];
    auto sar = av_guess_sample_aspect_ratio(format_ctx_, st, nullptr);
    if (st->codecpar->width) {
      // TODO: frame size available...
    }
  }
}

int DataSource1::OpenStreams(const int st_index[AVMEDIA_TYPE_NB]) {
  if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
    InitAudioDecoder(st_index[AVMEDIA_TYPE_AUDIO]);
  }
  if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
    InitVideoDecoder(st_index[AVMEDIA_TYPE_VIDEO]);
  }
  if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
    // ignore.
  }
  if (video_stream_index < 0 && audio_stream_index < 0) {
    av_log(nullptr, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n", filename);
    return -1;
  }
  return 0;
}

void DataSource1::InitVideoDecoder(int stream_index) {
  DCHECK_GE(stream_index, 0);
  DCHECK_LT(stream_index, format_ctx_->nb_streams);
  if (stream_index < 0 || stream_index >= format_ctx_->nb_streams) {
    return;
  }
  auto *stream = format_ctx_->streams[stream_index];
  DCHECK(stream);

  double max_frame_duration = (format_ctx_->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
  video_stream_index = stream_index;
  video_stream_ = stream;
  video_queue->time_base = stream->time_base;

  auto video_decode_config = std::make_unique<VideoDecodeConfig>(
      *stream->codecpar, stream->time_base,
      av_guess_frame_rate(format_ctx_, stream, nullptr),
      max_frame_duration);
  video_decode_config_ = *video_decode_config;
//  video_demuxer_stream_ = std::make_shared<DemuxerStream>(
//      stream, video_queue.get(), DemuxerStream::Video, nullptr,
//      std::move(video_decode_config), continue_read_thread_);
}

void DataSource1::InitAudioDecoder(int stream_index) {
  DCHECK_GE(stream_index, 0);
  DCHECK_LT(stream_index, format_ctx_->nb_streams);
  if (stream_index < 0 || stream_index >= format_ctx_->nb_streams) {
    return;
  }
  auto *stream = format_ctx_->streams[stream_index];
  DCHECK(stream);

  audio_stream_index = stream_index;
  audio_stream_ = stream;
  audio_queue->time_base = stream->time_base;

//  audio_decode_config_ = AudioDecodeConfig(*stream->codecpar, stream->time_base);
//  audio_demuxer_stream_ = std::make_shared<DemuxerStream>(
//      stream, audio_queue.get(), DemuxerStream::Audio,
//      std::make_unique<AudioDecodeConfig>(*stream->codecpar, stream->time_base),
//      nullptr, continue_read_thread_);

}

void DataSource1::ReadStreams(std::mutex &read_mutex) {
  bool last_paused = false;
  AVPacket pkt_data, *pkt = &pkt_data;
  for (;;) {
    if (abort_request) {
      break;
    }
    if (paused != last_paused) {
      last_paused = paused;
      if (paused) {
        av_read_pause(format_ctx_);
      } else {
        av_read_play(format_ctx_);
      }
    }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
    if (play_when_ready_ && (!strcmp(format_ctx_->iformat->name, "rtsp")
                   || (format_ctx_->pb && !strncmp(filename, "mmsh:", 5)))) {
        /* wait 10 ms to avoid trying to get another packet */
        /* XXX: horrible */
        SDL_Delay(10);
        continue;
    }
#endif
    ProcessSeekRequest();
    ProcessAttachedPicture();
    if (!isNeedReadMore()) {
      std::unique_lock<std::mutex> lock(read_mutex);
      continue_read_thread_->wait(lock);
      continue;
    }
    if (IsReadComplete()) {
//      // TODO check complete
//      bool loop = configuration.loop != 1 && (!configuration.loop || --configuration.loop);
//      msg_ctx->NotifyMsg(FFP_MSG_COMPLETED, loop);
//      if (loop) {
//        Seek(configuration.start_time != AV_NOPTS_VALUE ? configuration.start_time : 0);
//      } else {
//        msg_ctx->NotifyMsg(FFP_MSG_PLAYBACK_STATE_CHANGED, int(MediaPlayerState::END));
//        change_player_state(player, END);
//        stream_toggle_pause(player->is);
//      }
    }
    {
      auto ret = ProcessReadFrame(pkt, read_mutex);
      if (ret < 0) {
        break;
      } else if (ret > 0) {
        continue;
      }
    }

    ProcessQueuePacket(pkt);
    if (on_new_packet_send_) {
      on_new_packet_send_();
    }
  }
}

void DataSource1::ProcessSeekRequest() {
  if (!seek_req_) {
    return;
  }
  auto seek_target = seek_position;
  auto ret = avformat_seek_file(format_ctx_, -1, INT64_MIN, seek_target, INT64_MAX, 0);
  if (ret < 0) {
    av_log(nullptr, AV_LOG_ERROR, "%s: error while seeking, error: %s\n", filename, av_err_to_str(ret));
  } else {
    if (audio_stream_index >= 0 && audio_queue) {
      audio_queue->Flush();
      audio_queue->Put(PacketQueue::GetFlushPacket());
    }
    if (subtitle_stream_index >= 0 && subtitle_queue) {
      subtitle_queue->Flush();
      subtitle_queue->Put(PacketQueue::GetFlushPacket());
    }
    if (video_stream_index >= 0 && video_queue) {
      video_queue->Flush();
      video_queue->Put(PacketQueue::GetFlushPacket());
    }
    if (ext_clock) {
      ext_clock->SetClock(seek_target / (double) AV_TIME_BASE, 0);
    }
  }
  seek_req_ = false;
  queue_attachments_req_ = true;
  eof = false;

  // TODO notify on seek complete.
}

void DataSource1::ProcessAttachedPicture() {
  if (!queue_attachments_req_) {
    return;
  }
  if (video_stream_ && video_stream_->disposition & AV_DISPOSITION_ATTACHED_PIC) {
    AVPacket copy;
    auto ret = av_packet_ref(&copy, &video_stream_->attached_pic);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR, "%s: error to read attached pic. error: %s", filename, av_err_to_str(ret));
    } else {
      video_queue->Put(&copy);
      video_queue->PutNullPacket(video_stream_index);
    }
  }
  queue_attachments_req_ = false;
}

bool DataSource1::isNeedReadMore() {
  if (infinite_buffer) {
    return true;
  }
  if (audio_queue->size + video_queue->size + subtitle_queue->size > MAX_QUEUE_SIZE) {
    return false;
  }
  if (stream_has_enough_packets(audio_stream_, audio_stream_index, audio_queue)
      && stream_has_enough_packets(video_stream_, video_stream_index, video_queue)
      && stream_has_enough_packets(subtitle_stream_, subtitle_stream_index, subtitle_queue)) {
    return false;
  }
  return true;
}

bool DataSource1::IsReadComplete() const {
  if (paused) {
    return false;
  }
  return eof;
}

int DataSource1::ProcessReadFrame(AVPacket *pkt, std::mutex &read_mutex) {
  auto ret = av_read_frame(format_ctx_, pkt);
  if (ret < 0) {
    if ((ret == AVERROR_EOF || avio_feof(format_ctx_->pb)) && !eof) {
      if (video_stream_index >= 0) {
        video_queue->PutNullPacket(video_stream_index);
      }
      if (audio_stream_index >= 0) {
        video_queue->PutNullPacket(audio_stream_index);
      }
      if (subtitle_stream_index >= 0) {
        video_queue->PutNullPacket(subtitle_stream_index);
      }
      eof = true;
      //                check_buffering(player);
    }
    if (format_ctx_->pb && format_ctx_->pb->error) {
      return -1;
    }
    std::unique_lock<std::mutex> lock(read_mutex);
    continue_read_thread_->wait_for(lock, std::chrono::milliseconds(10));
    return 1;
  } else {
    eof = false;
  }
  return 0;
}

void DataSource1::ProcessQueuePacket(AVPacket *pkt) {
  auto stream_start_time = format_ctx_->streams[pkt->stream_index]->start_time;
  if (stream_start_time == AV_NOPTS_VALUE) {
    stream_start_time = 0;
  }
  auto pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
  auto player_start_time = start_time == AV_NOPTS_VALUE ? start_time : 0;
  auto diff = (double) (pkt_ts - stream_start_time) * av_q2d(format_ctx_->streams[pkt->stream_index]->time_base)
      - player_start_time / (double) AV_TIME_BASE;
  bool pkt_in_play_range = duration == AV_NOPTS_VALUE
      || diff <= duration / (double) AV_TIME_BASE;
  if (pkt->stream_index == audio_stream_index && pkt_in_play_range) {
    audio_queue->Put(pkt);
  } else if (pkt->stream_index == video_stream_index && pkt_in_play_range &&
      !(video_stream_->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
    video_queue->Put(pkt);
  } else if (pkt->stream_index == subtitle_stream_index && pkt_in_play_range) {
    subtitle_queue->Put(pkt);
  } else {
    av_packet_unref(pkt);
  }
}

bool DataSource1::ContainVideoStream() {
  return video_stream_ != nullptr;
}

bool DataSource1::ContainAudioStream() {
  return audio_stream_ != nullptr;
}

bool DataSource1::ContainSubtitleStream() {
  return subtitle_stream_ != nullptr;
}

void DataSource1::Seek(double position) {
  auto target = (int64_t) (position * AV_TIME_BASE);
  if (!format_ctx_) {
    start_time = FFMAX(0, target);
    return;
  }

  if (format_ctx_->start_time != AV_NOPTS_VALUE) {
    position = (double) FFMAX(format_ctx_->start_time, target);
  }
  target = FFMAX(0, target);
  target = FFMIN(target, format_ctx_->duration);
  av_log(nullptr, AV_LOG_INFO, "data source seek to %0.2f \n", position);

  if (!seek_req_) {
    seek_position = target;
    seek_req_ = true;

    // TODO update buffered position
//        player->buffered_position = -1;
//        change_player_state(player, BUFFERING);
    continue_read_thread_->notify_all();
  }
}

double DataSource1::GetSeekPosition() const { return seek_position / (double) (AV_TIME_BASE); }

double DataSource1::GetDuration() {
  CHECK_VALUE_WITH_RETURN(format_ctx_, -1);
  return format_ctx_->duration / (double) AV_TIME_BASE;
}

int DataSource1::GetChapterCount() {
  CHECK_VALUE_WITH_RETURN(format_ctx_, -1);
  return (int) format_ctx_->nb_chapters;
}

int DataSource1::GetChapterByPosition(int64_t position) {
  CHECK_VALUE_WITH_RETURN(format_ctx_, -1);
  CHECK_VALUE_WITH_RETURN(format_ctx_->nb_chapters, -1);
  for (int i = 0; i < format_ctx_->nb_chapters; i++) {
    AVChapter *ch = format_ctx_->chapters[i];
    if (av_compare_ts(position, av_time_base_q_, ch->start, ch->time_base) < 0) {
      i--;
      return i;
    }
  }
  return -1;
}

void DataSource1::SeekToChapter(int chapter) {
  CHECK_VALUE(format_ctx_);
  CHECK_VALUE(format_ctx_->nb_chapters);
  if (chapter < 0 || chapter >= format_ctx_->nb_chapters) {
    av_log(nullptr, AV_LOG_ERROR, "chapter out of range: %d", chapter);
    return;
  }
  AVChapter *ac = format_ctx_->chapters[chapter];
  Seek(av_rescale_q(ac->start, ac->time_base, av_time_base_q_));
}

const char *DataSource1::GetFileName() const { return filename; }

const char *DataSource1::GetMetadataDict(const char *key) {
  CHECK_VALUE_WITH_RETURN(format_ctx_, nullptr);
  auto *entry = av_dict_get(format_ctx_->metadata, key, nullptr, 0);
  if (entry) {
    return entry->value;
  }
  return nullptr;
}

}