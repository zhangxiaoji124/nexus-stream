#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace nexus::core {

/**
 * @brief Thread-safe bounded blocking queue with graceful shutdown support.
 *        Widely used in audio/video pipelines (Demuxer -> Decoder -> Muxer/Pusher)
 *        to provide backpressure and prevent unbounded memory growth.
 */
template <typename T>
class BoundedBlockingQueue {
public:
    explicit BoundedBlockingQueue(size_t capacity = 256)
        : capacity_(capacity > 0 ? capacity : 1) {}

    BoundedBlockingQueue(const BoundedBlockingQueue&) = delete;
    BoundedBlockingQueue& operator=(const BoundedBlockingQueue&) = delete;

    /**
     * @brief Blocks until space is available or the queue is closed.
     * @return true if item was pushed, false if queue was closed.
     */
    bool Push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_cv_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
        if (closed_) {
            return false;
        }
        queue_.push_back(std::move(item));
        not_empty_cv_.notify_one();
        return true;
    }

    /**
     * @brief Non-blocking push. If full, optionally drops the oldest element (for real-time live streaming).
     */
    bool TryPush(T item, bool drop_oldest_if_full = false) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_) {
            return false;
        }
        if (queue_.size() >= capacity_) {
            if (!drop_oldest_if_full) {
                return false;
            }
            queue_.pop_front();
            ++dropped_count_;
        }
        queue_.push_back(std::move(item));
        not_empty_cv_.notify_one();
        return true;
    }

    /**
     * @brief Blocks until an item is available or the queue is closed and empty.
     */
    std::optional<T> Pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_cv_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) {
            return std::nullopt;
        }
        T item = std::move(queue_.front());
        queue_.pop_front();
        not_full_cv_.notify_one();
        return item;
    }

    /**
     * @brief Blocks up to timeout_ms waiting for an item.
     */
    std::optional<T> PopFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!not_empty_cv_.wait_for(lock, timeout, [this] { return closed_ || !queue_.empty(); })) {
            return std::nullopt;
        }
        if (queue_.empty()) {
            return std::nullopt;
        }
        T item = std::move(queue_.front());
        queue_.pop_front();
        not_full_cv_.notify_one();
        return item;
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        not_empty_cv_.notify_all();
        not_full_cv_.notify_all();
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        not_full_cv_.notify_all();
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    size_t Capacity() const noexcept { return capacity_; }

    size_t DroppedCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_count_;
    }

    bool IsClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_cv_;
    std::condition_variable not_full_cv_;
    std::deque<T> queue_;
    size_t dropped_count_ = 0;
    bool closed_ = false;
};

}  // namespace nexus::core
