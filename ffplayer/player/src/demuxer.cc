//
// Created by yangbin on 2021/4/5.
//

#include "base/logging.h"

#include "demuxer.h"
#include <ffmpeg/blocking_url_protocol.h>

namespace media {

Demuxer::Demuxer(std::shared_ptr<base::MessageLoop> message_loop,
                 DataSource *data_source,
                 MediaTracksUpdatedCB media_tracks_updated_cb)
    : task_runner_(std::move(message_loop)),
      data_source_(data_source),
      media_tracks_updated_cb_(std::move(media_tracks_updated_cb)),
      host_(nullptr),
      format_context_(nullptr),
      bitrate_(0),
      start_time_(kNoTimestamp()),
      audio_disabled_(false),
      duration_known_(false) {
  DCHECK(task_runner_);
  DCHECK(data_source_);
}

void Demuxer::PostDemuxTask() {
  task_runner_->PostTask(FROM_HERE, [this]() {
    DemuxTask();
  });
}

void Demuxer::DemuxTask() {
  DCHECK(task_runner_->BelongsToCurrentThread());

  // Make sure we have work to do before demuxing.
  if (!StreamsHavePendingReads()) {
    return;
  }

  // Allocate and read an AVPacket from the media.
  unique_ptr_d<AVPacket> packet(new AVPacket(), [](AVPacket *ptr) {
    delete ptr;
  });
  int result = av_read_frame(format_context_, packet.get());
  if (result < 0) {
    // Update the duration based on the audio stream if it was previously unknown.
    // http://crbug.com/86830
    if (!duration_known_) {
      // Search streams for AUDIO one.
      for (auto &stream : streams_) {
        if (stream && stream->type() == DemuxerStream::Audio) {
          auto duration = stream->GetElapsedTime();
          if (duration != kNoTimestamp() && duration > TimeDelta()) {
            host_->SetDuration(duration);
            duration_known_ = true;
          }
          break;
        }
      }
    }

    // If we have reached the end of stream, tell the downstream filters about the event.
    StreamHasEnded();
    return;
  }

  // Queue the packet with the appropriate streams.
  DCHECK_GE(packet->stream_index, 0);
  DCHECK_LT(packet->stream_index, static_cast<int>(streams_.size()));

  if (packet->stream_index >= 0 && packet->stream_index < streams_.size() && streams_[packet->stream_index]
      && (!audio_disabled_ || streams_[packet->stream_index]->type() != DemuxerStream::Audio)) {
    auto demuxer_stream = streams_[packet->stream_index];
    demuxer_stream->EnqueuePacket(std::move(packet));
  }

  // Create a loop by posting another task.  This allows seek and message loop
  // quit tasks to get processed.
  if (StreamsHavePendingReads()) {
    PostDemuxTask();
  }

}

bool Demuxer::StreamsHavePendingReads() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  return std::any_of(streams_.begin(), streams_.end(), [](const std::shared_ptr<DemuxerStream> &stream) -> bool {
    return stream->HasPendingReads();
  });
}

void Demuxer::Initialize(DemuxerHost *host, PipelineStatusCB status_cb) {
  DCHECK(task_runner_->BelongsToCurrentThread());

  host_ = host;

  url_protocol_ = std::make_unique<BlockingUrlProtocol>(
      data_source_,
      [weak_this(std::weak_ptr<Demuxer>(shared_from_this()))]() {
        DLOG(WARNING) << ": data source error";
        if (auto demuxer = weak_this.lock()) {
          demuxer->host_->OnDemuxerError(PIPELINE_ERROR_ABORT);
        }
      });
  glue_ = std::make_unique<FFmpegGlue>(url_protocol_.get());
  AVFormatContext *format_context = glue_->format_context();

  // Disable ID3v1 tag reading to avoid costly seeks to end of file for data we
  // don't use.  FFmpeg will only read ID3v1 tags if no other metadata is
  // available, so add a metadata entry to ensure some is always present.
  av_dict_set(&format_context->metadata, "skip_id3v1_tags", "", 0);

  // Ensure ffmpeg doesn't give up too early while looking for stream params;
  // this does not increase the amount of data downloaded.  The default value
  // is 5 AV_TIME_BASE units (1 second each), which prevents some oddly muxed
  // streams from being detected properly; this value was chosen arbitrarily.
  format_context->max_analyze_duration = 60 * AV_TIME_BASE;

  OnOpenContextDone(glue_->OpenContext(is_local_file_), std::move(status_cb));

}

static TimeDelta ExtractStartTime(AVStream *stream) {
  // The default start time is zero.
  TimeDelta start_time;

  // First try to use  the |start_time| value as is.
  if (stream->start_time != AV_NOPTS_VALUE)
    start_time = ffmpeg::ConvertFromTimeBase(stream->time_base, stream->start_time);

  // Next try to use the first DTS value, for codecs where we know PTS == DTS
  // (excludes all H26x codecs). The start time must be returned in PTS.
  if (stream->first_dts != AV_NOPTS_VALUE &&
      stream->codecpar->codec_id != AV_CODEC_ID_HEVC &&
      stream->codecpar->codec_id != AV_CODEC_ID_H264 &&
      stream->codecpar->codec_id != AV_CODEC_ID_MPEG4) {
    const TimeDelta first_pts =
        ffmpeg::ConvertFromTimeBase(stream->time_base, stream->first_dts);
    if (first_pts < start_time)
      start_time = first_pts;
  }

  return start_time;
}

// Helper for calculating the bitrate of the media based on information stored
// in |format_context| or failing that the size and duration of the media.
//
// Returns 0 if a bitrate could not be determined.
static int CalculateBitrate(
    AVFormatContext *format_context,
    const TimeDelta &duration,
    int64 filesize_in_bytes) {
  // If there is a bitrate set on the container, use it.
  if (format_context->bit_rate > 0)
    return format_context->bit_rate;

  // Then try to sum the bitrates individually per stream.
  int bitrate = 0;
  for (size_t i = 0; i < format_context->nb_streams; ++i) {
    auto *codec_par = format_context->streams[i]->codecpar;
    bitrate += codec_par->bit_rate;
  }
  if (bitrate > 0)
    return bitrate;

  // See if we can approximate the bitrate as long as we have a filesize and
  // valid duration.
  if (duration.count() <= 0 ||
      duration == kInfiniteDuration() ||
      filesize_in_bytes == 0) {
    return 0;
  }

  // Do math in floating point as we'd overflow an int64 if the filesize was
  // larger than ~1073GB.
  double bytes = filesize_in_bytes;
  double duration_us = duration.count();
  return int(bytes * 8000000.0 / duration_us);
}

void Demuxer::OnOpenContextDone(bool open, PipelineStatusCB status_cb) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  if (stopped_) {
    status_cb(PIPELINE_ERROR_ABORT);
    return;
  }
  if (!open) {
    status_cb(PIPELINE_ERROR_ABORT);
    return;
  }

  auto result = avformat_find_stream_info(glue_->format_context(), nullptr);
  if (result < 0) {
    status_cb(PIPELINE_ERROR_ABORT);
    return;
  }

  // Create demuxer stream entries for each possible AVStream. Each stream
  // is examined to determine if it is supported or not (is the codec enabled
  // for it in this release?). Unsupported streams are skipped, allowing for
  // partial playback. At least one audio or video stream must be playable.
  AVFormatContext *format_context = glue_->format_context();
  streams_.resize(format_context->nb_streams);

  std::unique_ptr<MediaTracks> media_tracks = std::make_unique<MediaTracks>();

  DCHECK(track_id_to_demux_stream_map_.empty());

  // If available, |start_time_| will be set to the lowest stream start time.
  start_time_ = kInfiniteDuration();

  TimeDelta max_duration;
  int supported_audio_track_count = 0;
  int supported_video_track_count = 0;
  bool has_opus_or_vorbis_audio = false;
  bool needs_negative_timestamp_fixup = false;
  for (size_t i = 0; i < format_context->nb_streams; ++i) {
    AVStream *stream = format_context->streams[i];
    const AVCodecParameters *codec_parameters = stream->codecpar;
    const AVMediaType codec_type = codec_parameters->codec_type;
    const AVCodecID codec_id = codec_parameters->codec_id;
    // Skip streams which are not properly detected.
    if (codec_id == AV_CODEC_ID_NONE) {
      stream->discard = AVDISCARD_ALL;
      continue;
    }

    if (codec_type == AVMEDIA_TYPE_AUDIO) {
      DLOG(INFO) << "Media.DetectedAudioCodec" << codec_id;
    } else if (codec_type == AVMEDIA_TYPE_VIDEO) {
      DLOG(INFO) << "Media.DetectedVideoCodec" << codec_id;
    } else if (codec_type == AVMEDIA_TYPE_SUBTITLE) {
      stream->discard = AVDISCARD_ALL;
      continue;
    } else {
      stream->discard = AVDISCARD_ALL;
      continue;
    }

    // Attempt to create a FFmpegDemuxerStream from the AVStream. This will
    // return nullptr if the AVStream is invalid. Validity checks will verify
    // things like: codec, channel layout, sample/pixel format, etc...
    auto demuxer_stream = DemuxerStream::Create(this, stream);
    if (demuxer_stream) {
      streams_[i] = std::move(demuxer_stream);
    } else {
      if (codec_type == AVMEDIA_TYPE_AUDIO) {
        DLOG(INFO) << GetDisplayName() << ": skipping invalid or unsupported audio track";
      } else if (codec_type == AVMEDIA_TYPE_VIDEO) {
        DLOG(INFO) << GetDisplayName() << ": skipping invalid or unsupported video track";
      }
      // This AVStream does not successfully convert.
      continue;
    }

    auto track_id = static_cast<MediaTrack::TrackId>(media_tracks->tracks().size() + 1);
    auto track_label = MediaTrack::Label(streams_[i]->GetMetadata("handler_name"));
    auto track_language = MediaTrack::Language(streams_[i]->GetMetadata("language"));

    // Some metadata is named differently in FFmpeg for webm files.
//    if (glue_->container() == container_names::CONTAINER_WEBM)
//      track_label = MediaTrack::Label(streams_[i]->GetMetadata("title"));

//    if (codec_type == AVMEDIA_TYPE_AUDIO) {
//      ++supported_audio_track_count;
//      streams_[i]->SetEnabled(supported_audio_track_count == 1,
//                              base::TimeDelta());
//    } else if (codec_type == AVMEDIA_TYPE_VIDEO) {
//      ++supported_video_track_count;
//      streams_[i]->SetEnabled(supported_video_track_count == 1,
//                              base::TimeDelta());
//    }

    // TODO(chcunningham): Remove the IsValidConfig() checks below. If the
    // config isn't valid we shouldn't have created a demuxer stream nor
    // an entry in |media_tracks|, so the check should always be true.
    if ((codec_type == AVMEDIA_TYPE_AUDIO &&
        media_tracks->getAudioConfig(track_id).IsValidConfig()) ||
        (codec_type == AVMEDIA_TYPE_VIDEO &&
            media_tracks->getVideoConfig(track_id).IsValidConfig())) {
      DLOG(INFO) << GetDisplayName() << ": skipping duplicate media stream id=" << track_id;
      continue;
    }

    // Note when we find our audio/video stream (we only want one of each) and
    // record src= playback UMA stats for the stream's decoder config.
    MediaTrack *media_track = nullptr;
    if (codec_type == AVMEDIA_TYPE_AUDIO) {
      AudioDecoderConfig audio_config = streams_[i]->audio_decoder_config();

      media_track = media_tracks->AddAudioTrack(audio_config, track_id,
                                                MediaTrack::Kind("main"),
                                                track_label, track_language);
      media_track->set_id(MediaTrack::Id(std::to_string(track_id)));
      DCHECK(track_id_to_demux_stream_map_.find(media_track->id()) ==
          track_id_to_demux_stream_map_.end());
      track_id_to_demux_stream_map_[media_track->id()] = streams_[i].get();
    } else if (codec_type == AVMEDIA_TYPE_VIDEO) {
      VideoDecoderConfig video_config = streams_[i]->video_decoder_config();

      media_track = media_tracks->AddVideoTrack(video_config, track_id,
                                                MediaTrack::Kind("main"),
                                                track_label, track_language);
      media_track->set_id(MediaTrack::Id(std::to_string(track_id)));
      DCHECK(track_id_to_demux_stream_map_.find(media_track->id()) ==
          track_id_to_demux_stream_map_.end());
      track_id_to_demux_stream_map_[media_track->id()] = streams_[i].get();
    }

    max_duration = std::max(max_duration, streams_[i]->duration());

    TimeDelta start_time = ExtractStartTime(stream);

    // Note: This value is used for seeking, so we must take the true value and
    // not the one possibly clamped to zero below.
    if (start_time != kNoTimestamp() && start_time < start_time_)
      start_time_ = start_time;

    const bool is_opus_or_vorbis = codec_id == AV_CODEC_ID_OPUS || codec_id == AV_CODEC_ID_VORBIS;
    if (!has_opus_or_vorbis_audio)
      has_opus_or_vorbis_audio = is_opus_or_vorbis;

    if (codec_type == AVMEDIA_TYPE_AUDIO && start_time < TimeDelta() &&
        is_opus_or_vorbis) {
      needs_negative_timestamp_fixup = true;

      // Fixup the seeking information to avoid selecting the audio stream
      // simply because it has a lower starting time.
      start_time = TimeDelta();
    }

//    streams_[i]->set_start_time(start_time);
  }

  if (media_tracks->tracks().empty()) {
    LOG(ERROR) << GetDisplayName() << ": no supported streams";
    status_cb(PIPELINE_ERROR_ABORT);
    return;
  }

  if (format_context->duration != AV_NOPTS_VALUE) {
    // If there is a duration value in the container use that to find the
    // maximum between it and the duration from A/V streams.
    const AVRational av_time_base = {1, AV_TIME_BASE};
    max_duration = std::max(max_duration, ffmpeg::ConvertFromTimeBase(av_time_base, format_context->duration));
  } else {
    // The duration is unknown, in which case this is likely a live stream.
    max_duration = kInfiniteDuration();
  }

  // If no start time could be determined, default to zero.
  if (start_time_ == kInfiniteDuration())
    start_time_ = TimeDelta();

  // MPEG-4 B-frames cause grief for a simple container like AVI. Enable PTS
  // generation so we always get timestamps, see http://crbug.com/169570
//  if (glue_->container() == container_names::CONTAINER_AVI)
//    format_context->flags |= AVFMT_FLAG_GENPTS;

  // FFmpeg will incorrectly adjust the start time of MP3 files into the future
  // based on discard samples. We were unable to fix this upstream without
  // breaking ffmpeg functionality. https://crbug.com/1062037
//  if (glue_->container() == container_names::CONTAINER_MP3)
//    start_time_ = base::TimeDelta();

//  // For testing purposes, don't overwrite the timeline offset if set already.
//  if (timeline_offset_.is_null()) {
//    timeline_offset_ =
//        ExtractTimelineOffset(glue_->container(), format_context);
//  }

  // Since we're shifting the externally visible start time to zero, we need to
  // adjust the timeline offset to compensate.
//  if (!timeline_offset_.is_null() && start_time_ < base::TimeDelta())
//    timeline_offset_ += start_time_;

//  if (max_duration == kInfiniteDuration && !timeline_offset_.is_null()) {
//    SetLiveness(DemuxerStream::LIVENESS_LIVE);
//  } else if (max_duration != kInfiniteDuration) {
//    SetLiveness(DemuxerStream::LIVENESS_RECORDED);
//  } else {
//    SetLiveness(DemuxerStream::LIVENESS_UNKNOWN);
//  }

  // Good to go: set the duration and bitrate and notify we're done
  // initializing.
  host_->SetDuration(max_duration);
  duration_ = max_duration;
  duration_known_ = (max_duration != kInfiniteDuration());

  int64_t filesize_in_bytes = 0;
  url_protocol_->GetSize(&filesize_in_bytes);
  bitrate_ = CalculateBitrate(format_context, max_duration, filesize_in_bytes);
  if (bitrate_ > 0)
    data_source_->SetBitrate(bitrate_);

  media_tracks_updated_cb_(std::move(media_tracks));

  status_cb(PIPELINE_OK);

}

void Demuxer::StreamHasEnded() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  for (auto &stream: streams_) {
    if (stream) {
      stream->SetEndOfStream();
    }
  }
}

void Demuxer::NotifyBufferingChanged() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  Ranges<TimeDelta> buffered;
  std::shared_ptr<DemuxerStream> audio = audio_disabled_ ? nullptr : GetFFmpegStream(DemuxerStream::AUDIO);
  std::shared_ptr<DemuxerStream> video = GetFFmpegStream(DemuxerStream::VIDEO);
  if (audio && video) {
    buffered = audio->GetBufferedRanges().IntersectionWith(video->GetBufferedRanges());
  } else if (audio_disabled_) {
    buffered = audio->GetBufferedRanges();
  } else if (video) {
    buffered = video->GetBufferedRanges();
  }
  for (size_t i = 0; i < buffered.size(); ++i) {
//    host_->AddBufferedTimeRange(buffered.start(i), buffered.end(i));
  }
}

std::shared_ptr<DemuxerStream> Demuxer::GetFFmpegStream(DemuxerStream::Type type) const {
  for (auto &stream : streams_) {
    if (stream->type() == type) {
      return stream;
    }
  }
  return nullptr;
}

void Demuxer::Stop(const std::function<void(void)> &callback) {
  task_runner_->PostTask(FROM_HERE, [&]() {
    StopTask(callback);
  });

  // Then wakes up the thread from reading.
  SignalReadCompleted(DataSource::kReadError);
}

void Demuxer::StopTask(const std::function<void(void)> &callback) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  for (auto &stream: streams_) {
    if (stream) {
      stream->Stop();
    }
  }
  if (data_source_) {
    data_source_->Stop();
  } else {
    callback();
  }
}

void Demuxer::SignalReadCompleted(int size) {
  last_read_bytes_ = size;
//  read_event_.Signal();
}

std::string Demuxer::GetDisplayName() const {
  return "Demuxer";
}

DemuxerStream *Demuxer::GetFirstStream(DemuxerStream::Type type) {
  auto streams = GetAllStreams();
  for (auto &stream: streams) {
    if (stream->type() == type) {
      return stream;
    }
  }
  return nullptr;
}

std::vector<DemuxerStream *> Demuxer::GetAllStreams() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  std::vector<DemuxerStream *> result;
  // Put enabled streams at the beginning of the list so that
  // MediaResource::GetFirstStream returns the enabled stream if there is one.
  // TODO(servolk): Revisit this after media track switching is supported.
  for (const auto &stream : streams_) {
    if (stream)
      result.push_back(stream.get());
  }
  // And include disabled streams at the end of the list.
//  for (const auto &stream : streams_) {
//    if (stream && !stream->IsEnabled())
//      result.push_back(stream.get());
//  }
  return result;
}

}