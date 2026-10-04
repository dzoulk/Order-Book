#include "orderbook/occupancy_bitmap.hpp"

#include <bit>

namespace orderbook {

namespace {
std::size_t wordCount(std::size_t bits) {
    return (bits + 63) / 64;
}
} // namespace

OccupancyBitmap::OccupancyBitmap(std::size_t n) : n_(n) {
    std::size_t bits = n;
    while (true) {
        std::size_t words = wordCount(bits);
        levels_.emplace_back(words, std::uint64_t{0});
        if (words <= 1) break;
        bits = words;
    }
}

void OccupancyBitmap::set(std::size_t i) {
    std::size_t idx = i;
    for (auto& level : levels_) {
        std::size_t w = idx / 64;
        std::size_t b = idx % 64;
        bool wordWasNonZero = level[w] != 0;
        level[w] |= (std::uint64_t{1} << b);
        if (wordWasNonZero) return;  // summary above already reflects this word's occupancy
        idx = w;
    }
}

void OccupancyBitmap::clear(std::size_t i) {
    std::size_t idx = i;
    for (auto& level : levels_) {
        std::size_t w = idx / 64;
        std::size_t b = idx % 64;
        level[w] &= ~(std::uint64_t{1} << b);
        if (level[w] != 0) return;  // still occupied; nothing to propagate upward
        idx = w;
    }
}

bool OccupancyBitmap::test(std::size_t i) const {
    return (levels_[0][i / 64] >> (i % 64)) & 1u;
}

std::size_t OccupancyBitmap::levelBitSpace(std::size_t level) const {
    return level == 0 ? n_ : levels_[level - 1].size();
}

std::optional<std::size_t> OccupancyBitmap::findFirstSetFrom(std::size_t start) const {
    if (start >= n_) return std::nullopt;
    return findFirstSetAtLevel(0, start);
}

std::optional<std::size_t> OccupancyBitmap::findLastSetUpTo(std::size_t bound) const {
    if (n_ == 0) return std::nullopt;
    if (bound >= n_) bound = n_ - 1;
    return findLastSetAtLevel(0, bound);
}

std::optional<std::size_t> OccupancyBitmap::findFirstSetAtLevel(std::size_t level, std::size_t start) const {
    const auto& words = levels_[level];
    std::size_t wordIdx = start / 64;
    std::size_t bitOff = start % 64;

    std::uint64_t masked = words[wordIdx] & (~std::uint64_t{0} << bitOff);
    if (masked != 0) {
        return wordIdx * 64 + static_cast<std::size_t>(std::countr_zero(masked));
    }

    if (level + 1 >= levels_.size()) {
        for (std::size_t w = wordIdx + 1; w < words.size(); ++w) {
            if (words[w] != 0) return w * 64 + static_cast<std::size_t>(std::countr_zero(words[w]));
        }
        return std::nullopt;
    }

    auto nextWord = findFirstSetAtLevel(level + 1, wordIdx + 1);
    if (!nextWord.has_value()) return std::nullopt;
    std::size_t w = *nextWord;  // summary bit set guarantees words[w] != 0
    return w * 64 + static_cast<std::size_t>(std::countr_zero(words[w]));
}

std::optional<std::size_t> OccupancyBitmap::findLastSetAtLevel(std::size_t level, std::size_t bound) const {
    const auto& words = levels_[level];
    std::size_t wordIdx = bound / 64;
    std::size_t bitOff = bound % 64;

    std::uint64_t mask = (bitOff == 63) ? ~std::uint64_t{0} : ((std::uint64_t{1} << (bitOff + 1)) - 1);
    std::uint64_t masked = words[wordIdx] & mask;
    if (masked != 0) {
        return wordIdx * 64 + (63 - static_cast<std::size_t>(std::countl_zero(masked)));
    }

    if (wordIdx == 0) return std::nullopt;

    if (level + 1 >= levels_.size()) {
        for (std::size_t w = wordIdx; w-- > 0;) {
            if (words[w] != 0) return w * 64 + (63 - static_cast<std::size_t>(std::countl_zero(words[w])));
        }
        return std::nullopt;
    }

    auto prevWord = findLastSetAtLevel(level + 1, wordIdx - 1);
    if (!prevWord.has_value()) return std::nullopt;
    std::size_t w = *prevWord;  // summary bit set guarantees words[w] != 0
    return w * 64 + (63 - static_cast<std::size_t>(std::countl_zero(words[w])));
}

} // namespace orderbook
