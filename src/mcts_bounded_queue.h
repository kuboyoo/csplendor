#ifndef CSPLENDOR_MCTS_BOUNDED_QUEUE_H
#define CSPLENDOR_MCTS_BOUNDED_QUEUE_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mcts_parallel {

// A closeable bounded MPMC queue. close() is idempotent and wakes both blocked
// producers and consumers. Items already queued remain drainable after close.
template <typename T> class BoundedQueue {
public:
  explicit BoundedQueue(size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0)
      throw std::invalid_argument("bounded queue capacity must be positive");
  }

  BoundedQueue(const BoundedQueue &) = delete;
  BoundedQueue &operator=(const BoundedQueue &) = delete;

  bool push(T value) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_full_.wait(lock, [&] { return closed_ || queue_.size() < capacity_; });
    if (closed_)
      return false;
    queue_.push_back(std::move(value));
    lock.unlock();
    not_empty_.notify_one();
    return true;
  }

  bool pop(T &value) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [&] { return closed_ || !queue_.empty(); });
    if (queue_.empty())
      return false;
    value = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();
    not_full_.notify_one();
    return true;
  }

  template <typename Rep, typename Period>
  bool pop_for(T &value, const std::chrono::duration<Rep, Period> &timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!not_empty_.wait_for(lock, timeout,
                             [&] { return closed_ || !queue_.empty(); }))
      return false;
    if (queue_.empty())
      return false;
    value = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();
    not_full_.notify_one();
    return true;
  }

  bool try_pop(T &value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty())
      return false;
    value = std::move(queue_.front());
    queue_.pop_front();
    not_full_.notify_one();
    return true;
  }

  void close() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_)
        return;
      closed_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  bool closed() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  size_t size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  size_t capacity() const noexcept { return capacity_; }

private:
  const size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<T> queue_;
  bool closed_ = false;
};

} // namespace mcts_parallel

#endif // CSPLENDOR_MCTS_BOUNDED_QUEUE_H
