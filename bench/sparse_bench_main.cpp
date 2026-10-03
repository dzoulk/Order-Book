// Regression benchmark for the sparse-book pathology found in milestone
// 5 follow-up review: one resting order far from where the action is,
// repeatedly adding and cancelling an order at another far-away price.
// findNextOccupied used to be a linear scan over the price array, so this
// pattern made it O(price range): v2 measured 388,668 ns per add+cancel
// here (v1: 46 ns, about 8450x faster) before the fix to a hierarchical
// bitmap in OccupancyBitmap. Keeping this as a permanent benchmark so
// that pathology can never silently come back.
//
// Configuration via environment variables:
//   BENCH_SPARSE_ITERS   add+cancel iterations to time (default: 2000)

#include "orderbook/fast_order_book.hpp"
#include "orderbook/order_book.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace orderbook;
using Clock = std::chrono::steady_clock;

namespace {

std::size_t itersFromEnvOrDefault() {
    if (const char* s = std::getenv("BENCH_SPARSE_ITERS")) {
        return static_cast<std::size_t>(std::strtoull(s, nullptr, 10));
    }
    return 2000;
}

template <typename Book>
void run(const char* label, std::size_t iters) {
    Book book;
    book.addLimit(1, Side::Buy, 10, 100);  // one resting bid, far from the action

    Clock::time_point start = Clock::now();
    for (std::size_t i = 0; i < iters; ++i) {
        OrderId id = 1000 + i;
        book.addLimit(id, Side::Buy, 900'000, 10);
        book.cancel(id);
    }
    Clock::time_point end = Clock::now();

    double nsPerOp = std::chrono::duration<double, std::nano>(end - start).count() / static_cast<double>(iters);
    std::printf("sparse_bench [%s]: %.0f ns per add+cancel (%zu iters)\n", label, nsPerOp, iters);
}

} // namespace

int main() {
    std::size_t iters = itersFromEnvOrDefault();
    run<OrderBook>("v1", iters);
    run<FastOrderBook>("v2", iters);
    return 0;
}
