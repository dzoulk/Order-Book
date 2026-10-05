#pragma once

// Single-producer, single-consumer lock-free ring buffer. Fixed
// capacity (rounded up to a power of two), bounded: tryPush fails
// rather than blocking or growing when full. No locks or CAS loops,
// since there's only ever one thread on each end, head_ is written
// only by the producer and tail_ only by the consumer, so plain
// atomics with acquire/release are enough to publish an element
// safely across threads.
//
// Each side keeps a plain (non-atomic) cached copy of the other side's
// index, refreshed only when the cache says the queue might be
// full/empty, to avoid an atomic load (and the cross-core traffic that
// comes with it) on every single push/pop. The four fields below are
// each padded to their own cache line: head_ and tail_ are genuinely
// shared between threads, but tailCache_/headCache_ are each touched by
// only one thread, putting a cache-only field on the same line as an
// atomic the other thread polls would dirty that line on every update
// and force an unnecessary reload on the other side.

#include <atomic>
#include <cstddef>
#include <optional>
#include <vector>

namespace orderbook {

template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(std::size_t capacity)
        : capacity_(nextPowerOfTwo(capacity)), mask_(capacity_ - 1), buffer_(capacity_) {}

    // Producer side only. Returns false if the queue is full.
    bool tryPush(T value) {
        std::size_t head = head_.load(std::memory_order_relaxed);
        if (head - tailCache_ == capacity_) {
            tailCache_ = tail_.load(std::memory_order_acquire);
            if (head - tailCache_ == capacity_) return false;
        }
        buffer_[head & mask_] = std::move(value);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side only. Returns nullopt if the queue is empty.
    std::optional<T> tryPop() {
        std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == headCache_) {
            headCache_ = head_.load(std::memory_order_acquire);
            if (tail == headCache_) return std::nullopt;
        }
        T value = std::move(buffer_[tail & mask_]);
        tail_.store(tail + 1, std::memory_order_release);
        return value;
    }

private:
    static std::size_t nextPowerOfTwo(std::size_t n) {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    static constexpr std::size_t kCacheLineSize = 64;

    std::size_t capacity_;
    std::size_t mask_;
    std::vector<T> buffer_;

    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
    alignas(kCacheLineSize) std::size_t tailCache_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
    alignas(kCacheLineSize) std::size_t headCache_{0};
};

} // namespace orderbook
