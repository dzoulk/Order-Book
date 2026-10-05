#include "orderbook/spsc_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using orderbook::SpscQueue;

TEST(SpscQueue, EmptyPopReturnsNullopt) {
    SpscQueue<int> q(8);
    EXPECT_FALSE(q.tryPop().has_value());
}

TEST(SpscQueue, PushThenPopPreservesOrder) {
    SpscQueue<int> q(8);
    EXPECT_TRUE(q.tryPush(1));
    EXPECT_TRUE(q.tryPush(2));
    EXPECT_TRUE(q.tryPush(3));

    EXPECT_EQ(q.tryPop(), 1);
    EXPECT_EQ(q.tryPop(), 2);
    EXPECT_EQ(q.tryPop(), 3);
    EXPECT_FALSE(q.tryPop().has_value());
}

TEST(SpscQueue, FullQueueRejectsPush) {
    SpscQueue<int> q(4);  // rounds up to 4, already a power of two
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.tryPush(i));
    EXPECT_FALSE(q.tryPush(99));

    EXPECT_EQ(q.tryPop(), 0);
    EXPECT_TRUE(q.tryPush(99));  // freed one slot
}

TEST(SpscQueue, CapacityRoundsUpToPowerOfTwo) {
    SpscQueue<int> q(5);  // rounds up to 8
    for (int i = 0; i < 8; ++i) EXPECT_TRUE(q.tryPush(i));
    EXPECT_FALSE(q.tryPush(99));
}

TEST(SpscQueue, WrapsAroundCorrectly) {
    SpscQueue<int> q(4);
    for (int round = 0; round < 10; ++round) {
        EXPECT_TRUE(q.tryPush(round));
        EXPECT_EQ(q.tryPop(), round);
    }
}

// Real producer/consumer threads pushing/popping concurrently, spinning
// on tryPush/tryPop rather than treating a full/empty result as an
// error, the way an actual gateway/matching pipeline would. Verifies
// every value arrives exactly once, in order, with no real OS thread
// synchronization beyond the queue itself.
TEST(SpscQueue, ConcurrentProducerConsumerDeliversAllInOrder) {
    constexpr int kCount = 1'000'000;
    SpscQueue<int> q(1024);

    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            while (!q.tryPush(i)) {
                // full, spin
            }
        }
    });

    std::vector<int> received;
    received.reserve(kCount);
    std::thread consumer([&] {
        while (static_cast<int>(received.size()) < kCount) {
            if (auto v = q.tryPop()) {
                received.push_back(*v);
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        ASSERT_EQ(received[static_cast<std::size_t>(i)], i) << "mismatch at index " << i;
    }
}
