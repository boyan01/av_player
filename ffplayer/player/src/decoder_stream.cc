//
// Created by yangbin on 2021/5/2.
//

#include "decoder_stream.h"

#include <utility>

#include "base/bind_to_current_loop.h"
#include "base/lambda.h"

namespace media {

template<DemuxerStream::Type StreamType>
DecoderStream<StreamType>::DecoderStream(std::unique_ptr<DecoderStreamTraits<StreamType>> traits,
                                         TaskRunner *task_runner)
    : traits_(std::move(traits)), task_runner_(task_runner), outputs_(300000) {}

template<DemuxerStream::Type StreamType>
void DecoderStream<StreamType>::Initialize(DemuxerStream *stream, DecoderStream::InitCallback init_callback) {

  decoder_ = std::make_unique<Decoder>();
  demuxer_stream_ = stream;

  traits_->InitializeDecoder(
      decoder_.get(), stream,
      bind_weak(&DecoderStream<StreamType>::OnFrameAvailable, this->shared_from_this()));
  auto init_callback_bound = BindToLoop(task_runner_, std::move(init_callback));
  init_callback_bound(true);
}

template<DemuxerStream::Type StreamType>
void DecoderStream<StreamType>::Read(DecoderStream::ReadCallback read_callback) {
  auto read_callback_bound = BindToCurrentLoop(read_callback);
  if (outputs_.IsEmpty()) {
    read_callback_ = std::move(read_callback);
  } else {
    read_callback_bound(std::move(outputs_.PopFront()));
  }
  ReadFromDemuxerStream();
}

template<DemuxerStream::Type StreamType>
void DecoderStream<StreamType>::ReadFromDemuxerStream() {
  if (outputs_.GetSize() > 3) {
    return;
  }
  task_runner_->PostTask(FROM_HERE, bind_weak(&DecoderStream<StreamType>::DecodeTask, this->shared_from_this()));

}

template<DemuxerStream::Type StreamType>
void DecoderStream<StreamType>::DecodeTask() {
  DCHECK(demuxer_stream_);
  DCHECK(decoder_);

  AVPacket packet;
  av_init_packet(&packet);
  if (!demuxer_stream_->ReadPacket(&packet) || packet.data == PacketQueue::GetFlushPacket()->data) {
    // TODO
    return;
  }

  decoder_->Decode(&packet);

}

template<DemuxerStream::Type StreamType>
void DecoderStream<StreamType>::OnFrameAvailable(std::shared_ptr<Output> output) {
  DLOG_IF(WARNING, outputs_.IsFull()) << "OnFrameAvailable but outputs pool is full";

  outputs_.InsertLast(output);
  if (read_callback_) {
    std::move(read_callback_)(std::move(outputs_.PopFront()));
    read_callback_ = nullptr;
  }
  ReadFromDemuxerStream();
}

template
class DecoderStream<DemuxerStream::Video>;
template
class DecoderStream<DemuxerStream::Audio>;

}