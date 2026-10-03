#include "orderbook/occupancy_bitmap.hpp"

#include <gtest/gtest.h>

using orderbook::OccupancyBitmap;

TEST(OccupancyBitmap, EmptyFindsNothing) {
    OccupancyBitmap bm(1'000'000);
    EXPECT_FALSE(bm.findFirstSetFrom(0).has_value());
    EXPECT_FALSE(bm.findLastSetUpTo(999'999).has_value());
}

TEST(OccupancyBitmap, SingleBitFoundFromBelowAndAt) {
    OccupancyBitmap bm(1'000'000);
    bm.set(500'000);
    EXPECT_TRUE(bm.test(500'000));
    EXPECT_EQ(bm.findFirstSetFrom(0), 500'000u);
    EXPECT_EQ(bm.findFirstSetFrom(500'000), 500'000u);
    EXPECT_FALSE(bm.findFirstSetFrom(500'001).has_value());
    EXPECT_EQ(bm.findLastSetUpTo(999'999), 500'000u);
    EXPECT_EQ(bm.findLastSetUpTo(500'000), 500'000u);
    EXPECT_FALSE(bm.findLastSetUpTo(499'999).has_value());
}

TEST(OccupancyBitmap, ClearRemovesBit) {
    OccupancyBitmap bm(1'000'000);
    bm.set(42);
    bm.clear(42);
    EXPECT_FALSE(bm.test(42));
    EXPECT_FALSE(bm.findFirstSetFrom(0).has_value());
}

TEST(OccupancyBitmap, ClearOneBitLeavesOthersInSameWord) {
    OccupancyBitmap bm(1'000'000);
    bm.set(10);
    bm.set(11);
    bm.clear(10);
    EXPECT_FALSE(bm.test(10));
    EXPECT_TRUE(bm.test(11));
    EXPECT_EQ(bm.findFirstSetFrom(0), 11u);
}

TEST(OccupancyBitmap, FindsAcrossWordBoundaryWithinSameSummaryWord) {
    OccupancyBitmap bm(1'000'000);
    bm.set(63);
    bm.set(64);  // next word, same level-1 summary word
    EXPECT_EQ(bm.findFirstSetFrom(0), 63u);
    EXPECT_EQ(bm.findFirstSetFrom(64), 64u);
    EXPECT_EQ(bm.findLastSetUpTo(999'999), 64u);
    EXPECT_EQ(bm.findLastSetUpTo(63), 63u);
}

TEST(OccupancyBitmap, FindsAcrossFarApartSparseBits) {
    // Reproduces the pathological case this data structure exists to fix:
    // one bit near 0, one near the top of a million-entry range.
    OccupancyBitmap bm(1'000'000);
    bm.set(10);
    bm.set(900'000);

    EXPECT_EQ(bm.findFirstSetFrom(0), 10u);
    EXPECT_EQ(bm.findFirstSetFrom(11), 900'000u);
    EXPECT_FALSE(bm.findFirstSetFrom(900'001).has_value());

    EXPECT_EQ(bm.findLastSetUpTo(999'999), 900'000u);
    EXPECT_EQ(bm.findLastSetUpTo(899'999), 10u);
    EXPECT_FALSE(bm.findLastSetUpTo(9).has_value());
}

TEST(OccupancyBitmap, FindsAcrossTopLevelSummaryBoundary) {
    // Level boundaries for n=1,000,000: level0 word = 64 bits, level1 word
    // covers 64 level0 words = 4096 bits, level2 word covers 64 level1
    // words = 262144 bits. Set bits straddling a level2 boundary.
    OccupancyBitmap bm(1'000'000);
    bm.set(1);
    bm.set(262'144);  // first bit of the second level2-summary region
    bm.set(999'999);  // last valid bit

    EXPECT_EQ(bm.findFirstSetFrom(0), 1u);
    EXPECT_EQ(bm.findFirstSetFrom(2), 262'144u);
    EXPECT_EQ(bm.findFirstSetFrom(262'145), 999'999u);
    EXPECT_FALSE(bm.findFirstSetFrom(1'000'000).has_value());

    EXPECT_EQ(bm.findLastSetUpTo(999'999), 999'999u);
    EXPECT_EQ(bm.findLastSetUpTo(999'998), 262'144u);
    EXPECT_EQ(bm.findLastSetUpTo(262'143), 1u);
}

TEST(OccupancyBitmap, SetAndClearManyBitsRoundTrip) {
    OccupancyBitmap bm(10'000);
    for (std::size_t i = 0; i < 10'000; i += 7) {
        bm.set(i);
    }
    for (std::size_t i = 0; i < 10'000; i += 7) {
        EXPECT_TRUE(bm.test(i));
    }
    std::size_t count = 0;
    std::optional<std::size_t> cur = bm.findFirstSetFrom(0);
    while (cur.has_value()) {
        EXPECT_EQ(*cur % 7, 0u);
        ++count;
        cur = (*cur + 1 < 10'000) ? bm.findFirstSetFrom(*cur + 1) : std::nullopt;
    }
    EXPECT_EQ(count, (10'000 + 6) / 7);

    for (std::size_t i = 0; i < 10'000; i += 7) {
        bm.clear(i);
    }
    EXPECT_FALSE(bm.findFirstSetFrom(0).has_value());
}
