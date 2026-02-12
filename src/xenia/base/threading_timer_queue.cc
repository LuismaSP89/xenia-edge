/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <forward_list>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
#include <immintrin.h>
#endif

#include "third_party/concurrentqueue/blockingconcurrentqueue.h"
#include "xenia/base/assert.h"
#include "xenia/base/threading.h"
#include "xenia/base/threading_timer_queue.h"

namespace xe {
namespace threading {

using WaitItem = TimerQueueWaitItem;

class TimerQueue {
 public:
  using clock = WaitItem::clock;
  static_assert(clock::is_steady);

 public:
  TimerQueue() : queue_(), shutdown_(false) {
    dispatch_thread_ = std::thread(&TimerQueue::TimerThreadMain, this);
  }

  ~TimerQueue() {
    shutdown_.store(true, std::memory_order_release);

    // Kick dispatch thread to check shutdown flag
    auto wait_item = std::make_shared<WaitItem>(nullptr, nullptr, this,
                                                clock::time_point::min(),
                                                clock::duration::zero());
    wait_item->Disarm();
    QueueTimer(std::move(wait_item));

    dispatch_thread_.join();
  }

  void TimerThreadMain() {
    const auto comp = [](const std::shared_ptr<WaitItem>& left,
                         const std::shared_ptr<WaitItem>& right) {
      return left->due_ < right->due_;
    };

    xe::threading::set_name("xe::threading::TimerQueue");

    while (!shutdown_.load(std::memory_order_relaxed)) {
      {
        // Calculate timeout until next timer is due
        auto now = clock::now();
        auto timeout_duration =
            wait_queue_.empty()
                ? std::chrono::hours(24)
                : std::max(
                      std::chrono::microseconds(0),
                      std::chrono::duration_cast<std::chrono::microseconds>(
                          wait_queue_.front()->due_ - now));

        // Wait for new items with timeout
        std::shared_ptr<WaitItem> item;
        if (queue_.wait_dequeue_timed(item, timeout_duration)) {
          // Got at least one item, collect any others available
          std::forward_list<std::shared_ptr<WaitItem>> wait_items;
          wait_items.push_front(std::move(item));
          while (queue_.try_dequeue(item)) {
            wait_items.push_front(std::move(item));
          }
          wait_items.sort(comp);
          wait_queue_.merge(wait_items, comp);
        }
      }

      {
        // Check wait queue, invoke callbacks and reschedule
        std::forward_list<std::shared_ptr<WaitItem>> wait_items;
        while (!wait_queue_.empty() &&
               wait_queue_.front()->due_ <= clock::now()) {
          auto wait_item = std::move(wait_queue_.front());
          wait_queue_.pop_front();

          // Ensure that it isn't disarmed
          auto state = WaitItem::State::kIdle;
          if (wait_item->state_.compare_exchange_strong(
                  state, WaitItem::State::kInCallback,
                  std::memory_order_acq_rel)) {
            // Possibility to dispatch to a thread pool here
            assert_not_null(wait_item->callback_);
            wait_item->callback_(wait_item->userdata_);

            if (wait_item->interval_ != clock::duration::zero() &&
                wait_item->state_.load(std::memory_order_acquire) !=
                    WaitItem::State::kInCallbackSelfDisarmed) {
              // Item is recurring and didn't self-disarm during callback:
              wait_item->due_ += wait_item->interval_;
              wait_item->state_.store(WaitItem::State::kIdle,
                                      std::memory_order_release);
              wait_items.push_front(std::move(wait_item));
            } else {
              wait_item->state_.store(WaitItem::State::kDisarmed,
                                      std::memory_order_release);
            }
          } else {
            // Specifically, kInCallback is illegal here
            assert_true(WaitItem::State::kDisarmed == state);
          }
        }
        wait_items.sort(comp);
        wait_queue_.merge(wait_items, comp);
      }
    }
  }

  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
    auto wait_item_weak = std::weak_ptr<WaitItem>(wait_item);

    // Mitigate callback flooding
    wait_item->due_ =
        std::max(clock::now() - wait_item->interval_, wait_item->due_);

    queue_.enqueue(std::move(wait_item));

    return wait_item_weak;
  }

  const std::thread& dispatch_thread() const { return dispatch_thread_; }

 private:
  moodycamel::BlockingConcurrentQueue<std::shared_ptr<WaitItem>> queue_;

  // This is a _sorted_ (ascending due_) list of active timers managed by a
  // dedicated thread
  std::forward_list<std::shared_ptr<WaitItem>> wait_queue_;
  std::atomic_bool shutdown_;
  std::thread dispatch_thread_;
};

xe::threading::TimerQueue timer_queue_;

void TimerQueueWaitItem::Disarm() {
  State state;

  // Special case for calling from a callback itself
  if (std::this_thread::get_id() == parent_queue_->dispatch_thread().get_id()) {
    state = State::kInCallback;
    if (state_.compare_exchange_strong(state, State::kInCallbackSelfDisarmed,
                                       std::memory_order_acq_rel)) {
      // If we are self disarming from the callback set this special state and
      // exit
      return;
    }
    // Normal case can handle the rest
  }

  // Adaptive spin-wait implementation
  uint32_t spin_count = 0;
  state = State::kIdle;
  // Classes which hold WaitItems will often call Disarm() to cancel them during
  // destruction. This may lead to race conditions when the dispatch thread
  // executes a callback which accesses memory that is freed simultaneously due
  // to this. Therefore, we need to guarantee that no callbacks will be running
  // once Disarm() has returned.
  while (!state_.compare_exchange_weak(state, State::kDisarmed,
                                       std::memory_order_acq_rel)) {
    if (state == State::kDisarmed) {
      // Do not break for kInCallbackSelfDisarmed and keep spinning in order to
      // meet guarantees
      break;
    }
    state = State::kIdle;

    // Adaptive backoff: busy spin -> yield -> sleep
    if (spin_count < 10) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
      _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
      __asm__ volatile("yield" ::: "memory");
#else
      std::this_thread::yield();
#endif
    } else if ((spin_count - 10) % 20 == 19) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } else {
      std::this_thread::yield();
    }
    ++spin_count;
  }
}
// unused
std::weak_ptr<WaitItem> QueueTimerOnce(std::function<void(void*)> callback,
                                       void* userdata,
                                       WaitItem::clock::time_point due) {
  return timer_queue_.QueueTimer(
      std::make_shared<WaitItem>(std::move(callback), userdata, &timer_queue_,
                                 due, WaitItem::clock::duration::zero()));
}
// only used by HighResolutionTimer
std::weak_ptr<WaitItem> QueueTimerRecurring(
    std::function<void(void*)> callback, void* userdata,
    WaitItem::clock::time_point due, WaitItem::clock::duration interval) {
  return timer_queue_.QueueTimer(std::make_shared<WaitItem>(
      std::move(callback), userdata, &timer_queue_, due, interval));
}

}  // namespace threading
}  // namespace xe
