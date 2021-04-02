//
// Created by boyan on 2021/3/27.
//

#include <thread>

#include "logging.h"
#include "message_loop.h"
#include "utility.h"

namespace media {

namespace base {

thread_local MessageLoop *MessageLoop::thread_local_message_loop_;

MessageLoop::MessageLoop(const char *loop_name) : loop_name_(loop_name) {

}

MessageLoop::~MessageLoop() {
  DCHECK_EQ(this, current());
}

void MessageLoop::PostTask(const tracked_objects::Location &from_here, const TaskClosure &task) {
  Message message(task, from_here);
  message_queue_.EnqueueMessage(message);
}

void MessageLoop::Loop() {
  for (;;) {
    Message *msg = message_queue_.next();
    if (msg == nullptr) {
      DLOG(INFO) << "MessageLoop over.";
      return;
    }

    DCHECK(msg->next == nullptr);

    msg->task();

    delete msg;
  }
}

MessageLoop *MessageLoop::current() {
  return thread_local_message_loop_;
}

void MessageLoop::Quit() {
  message_queue_.Quit();
}

MessageLoop *MessageLoop::prepare_looper(const char *loop_name) {
  auto *looper = new MessageLoop(loop_name);
  std::thread looper_th([looper]() {
    utility::update_thread_name(looper->loop_name_);
    thread_local_message_loop_ = looper;
    looper->Loop();
  });
  looper_th.detach();
  return looper;
}

} // namespace base
} // namespace media