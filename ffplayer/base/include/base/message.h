//
// Created by boyan on 2021/3/27.
//

#ifndef MEDIA_BASE_MESSAGE_H_
#define MEDIA_BASE_MESSAGE_H_

#include <queue>
#include <functional>

#include "base/timestamps.h"
#include "base/location.h"

namespace media {
namespace base {

typedef std::function<void(void)> TaskClosure;

struct Message {

  Message(TaskClosure task,
          const tracked_objects::Location &posted_from);

  Message(TaskClosure task,
          const tracked_objects::Location &posted_from,
          TimeDelta when);

  ~Message();

  // Used to support sorting.
  bool operator<(const Message &other) const;

  // The task to run
  TaskClosure task;

  tracked_objects::Location posted_from;

  int sequence_num;

  std::chrono::time_point<std::chrono::system_clock> when;

 private:
  Message *next = nullptr;

  friend class MessageQueue;
  friend class MessageLoop;

};

} // namespace base
} // namespace media

#endif //MEDIA_BASE_MESSAGE_H_
