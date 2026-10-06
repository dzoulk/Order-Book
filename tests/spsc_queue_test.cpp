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

TEST(SpscQueue, BatchPushPopBasic) {
    SpscQueue<int> q(16);
    int in[5] = {10, 20, 30, 40, 50};
    EXPECT_EQ(q.tryPushBatch(in, 5), 5u);

    int out[5] = {};
    EXPECT_EQ(q.tryPopBatch(out, 5), 5u);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(out[i], in[i]);
    EXPECT_EQ(q.tryPopBatch(out, 5), 0u);
}

TEST(SpscQueue, BatchPushRespectsCapacity) {
    SpscQueue<int> q(4);
    int in[6] = {0, 1, 2, 3, 4, 5};
    EXPECT_EQ(q.tryPushBatch(in, 6), 4u);  // only 4 slots available

    int out[4] = {};
    EXPECT_EQ(q.tryPopBatch(out, 10), 4u);  // only 4 were ever pushed
    for (int i = 0; i < 4; ++i) EXPECT_EQ(out[i], i);
}

TEST(SpscQueue, BatchWrapsAroundCorrectly) {
    SpscQueue<int> q(4);
    for (int round = 0; round < 10; ++round) {
        int in[3] = {round, round + 1, round + 2};
        std::size_t written = 0;
        while (written < 3) written += q.tryPushBatch(in + written, 3 - written);

        int out[3] = {};
        std::size_t read = 0;
        while (read < 3) read += q.tryPopBatch(out + read, 3 - read);
        for (int i = 0; i < 3; ++i) EXPECT_EQ(out[i], in[i]);
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

// Same stress test as above, but through the batch API: producer
// accumulates a local batch before publishing it with one tryPushBatch
// call, consumer drains a local batch per tryPopBatch call. Verifies
// the batched path delivers everything exactly once, in order, same as
// the single-item path.
TEST(SpscQueue, ConcurrentBatchProducerConsumerDeliversAllInOrder) {
    constexpr int kCount = 1'000'000;
    constexpr std::size_t kBatchSize = 64;
    SpscQueue<int> q(1024);

    std::thread producer([&] {
        int batch[kBatchSize];
        std::size_t batchLen = 0;
        for (int i = 0; i < kCount; ++i) {
            batch[batchLen++] = i;
            if (batchLen == kBatchSize) {
                std::size_t written = 0;
                while (written < batchLen) {
                    written += q.tryPushBatch(batch + written, batchLen - written);
                }
                batchLen = 0;
            }
        }
        std::size_t written = 0;
        while (written < batchLen) {
            written += q.tryPushBatch(batch + written, batchLen - written);
        }
    });

    std::vector<int> received;
    received.reserve(kCount);
    std::thread consumer([&] {
        int batch[kBatchSize];
        while (static_cast<int>(received.size()) < kCount) {
            std::size_t n = q.tryPopBatch(batch, kBatchSize);
            for (std::size_t i = 0; i < n; ++i) received.push_back(batch[i]);
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        ASSERT_EQ(received[static_cast<std::size_t>(i)], i) << "mismatch at index " << i;
    }
}
