#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace orderbook {

// Hierarchical bitmap: one bit per index, plus a chain of summary levels
// where level L+1 has one bit per 64-bit word of level L (set iff that
// word is non-zero). Finding the next/previous set bit is then at most
// one O(1) word operation per level, not a linear scan, which is what
// FastOrderBook needs findNextOccupied to actually be O(1) on a sparse
// book. See docs/HISTORY.md ("Bugs found and fixed") for the bug this
// fixes and the before/after numbers.
//
// For n = 1,000,000 (FastOrderBook::kMaxPrice), this is 4 levels
// (15625, 245, 4, 1 words), so any find is at most 4 word-level checks,
// independent of how far apart occupied bits are.
class OccupancyBitmap {
public:
    explicit OccupancyBitmap(std::size_t n);

    void set(std::size_t i);
    void clear(std::size_t i);
    bool test(std::size_t i) const;

    // Smallest set bit in [start, n), or nullopt if none.
    std::optional<std::size_t> findFirstSetFrom(std::size_t start) const;
    // Largest set bit in [0, bound], or nullopt if none. bound >= n is
    // clamped to n - 1.
    std::optional<std::size_t> findLastSetUpTo(std::size_t bound) const;

private:
    std::size_t levelBitSpace(std::size_t level) const;
    std::optional<std::size_t> findFirstSetAtLevel(std::size_t level, std::size_t start) const;
    std::optional<std::size_t> findLastSetAtLevel(std::size_t level, std::size_t bound) const;

    std::size_t n_;
    std::vector<std::vector<std::uint64_t>> levels_;  // levels_[0] is the finest
};

} // namespace orderbook
