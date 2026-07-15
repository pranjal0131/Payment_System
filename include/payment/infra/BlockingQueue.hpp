#pragma once
//
// BlockingQueue.hpp — a thread-safe, optionally-bounded blocking queue.
//
// LLD lesson: the Producer–Consumer pattern. Producers push work; consumers pop
// and process it. The queue is the synchronization boundary between them. Three
// classic concurrency tools cooperate here:
//
//   * std::mutex             — guards the underlying std::queue (mutual exclusion).
//   * std::condition_variable — lets a thread SLEEP until state changes, instead
//                               of busy-waiting (spinning burns 100% CPU).
//   * a "closed" flag        — clean shutdown so consumers don't block forever.
//
// Bounded capacity gives us BACKPRESSURE: if consumers fall behind, producers
// block on push() rather than letting the queue grow without limit and OOM the
// process. This is a real payment-system concern — an unbounded in-memory queue
// under a traffic spike is an outage waiting to happen.
//
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace pay::infra {

template <typename T>
class BlockingQueue {
public:
    // capacity == 0 means unbounded. Prefer a real bound in production.
    explicit BlockingQueue(std::size_t capacity = 0) : capacity_(capacity) {}

    // Non-copyable, non-movable: it owns synchronization primitives.
    BlockingQueue(const BlockingQueue&)            = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    // Blocking push. If the queue is full (bounded), waits until space frees up.
    // Returns false if the queue was closed while waiting (push rejected).
    bool push(T item) {
        std::unique_lock<std::mutex> lk(mtx_);
        notFull_.wait(lk, [&] { return closed_ || !isFullLocked(); });
        if (closed_) return false;
        queue_.push(std::move(item));
        lk.unlock();
        notEmpty_.notify_one();   // wake exactly one waiting consumer
        return true;
    }

    // Blocking pop. Sleeps until an item is available.
    // Returns std::nullopt only when the queue is closed AND drained — the
    // signal for a consumer loop to exit cleanly.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lk(mtx_);
        notEmpty_.wait(lk, [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return std::nullopt;   // closed and drained
        T item = std::move(queue_.front());
        queue_.pop();
        lk.unlock();
        notFull_.notify_one();    // wake a producer that may be blocked on push
        return item;
    }

    // Signal shutdown: no more pushes accepted, and every blocked thread wakes
    // so it can observe the closed state and return. Idempotent.
    void close() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            closed_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return queue_.size();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return closed_;
    }

private:
    bool isFullLocked() const {
        return capacity_ != 0 && queue_.size() >= capacity_;
    }

    const std::size_t       capacity_;
    mutable std::mutex      mtx_;
    std::condition_variable notEmpty_;   // consumers wait here
    std::condition_variable notFull_;    // producers wait here
    std::queue<T>           queue_;
    bool                    closed_ = false;
};

} // namespace pay::infra
