//
// Created by boyan on 2021/3/27.
//

#include <thread>

#include "base/logging.h"
#include "base/message_loop.h"
#include "base/utility.h"

namespace media {

namespace base {

thread_local MessageLooper *MessageLooper::thread_local_message_loop_;

MessageLooper::MessageLooper(const char *loop_name)
    : loop_name_(loop_name),
      message_queue_(std::make_unique<MessageQueue>()) {
}

MessageLooper::~MessageLooper() {
  DCHECK_EQ(this, current());
}

void MessageLooper::Prepare() {
  DCHECK(!prepared_);
  prepared_ = true;
  utility::update_thread_name(loop_name_);
  thread_local_message_loop_ = this;
}

void MessageLooper::Loop() {
  for (;;) {
    Message *msg = message_queue_->next();
    if (msg == nullptr) {
      DLOG(INFO) << "MessageLoop " << loop_name_ << " over.";
      return;
    }

    DCHECK(msg->next == nullptr);

    TRACE_METHOD_DURATION_WITH_LOCATION(16, msg->posted_from);
    msg->task();
    delete msg;
  }
}

void MessageLooper::PostTask(const tracked_objects::Location &from_here, const TaskClosure &task) {
  static const TimeDelta delay(0);
  PostDelayedTask(from_here, delay, task);
}
void MessageLooper::PostDelayedTask(const tracked_objects::Location &from_here,
                                    TimeDelta delay,
                                    const TaskClosure &task_closure) {
  Message message(task_closure, from_here, delay, nullptr, 0);
  message_queue_->EnqueueMessage(message);
}

MessageLooper *MessageLooper::current() {
  return thread_local_message_loop_;
}

void MessageLooper::Quit() {
  message_queue_->Quit();
}

MessageLooper *MessageLooper::prepare_looper(const char *loop_name) {
  auto *looper = new MessageLooper(loop_name);
  std::thread looper_th([looper]() {
    looper->Prepare();
    looper->Loop();
    delete looper;
  });
  looper_th.detach();
  return looper;
}

} // namespace base
} // namespace media