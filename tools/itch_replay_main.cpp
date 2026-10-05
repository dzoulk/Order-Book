// Replays real NASDAQ ITCH 5.0 order flow (for one chosen symbol)
// through OrderBook and FastOrderBook, instead of synthetic generated
// flow. See tools/README.md for how to get sample data; see
// docs/HISTORY.md for why this exists (a synthetic-only benchmark
// "invites skepticism", per an external code review) and what doesn't
// map cleanly between ITCH's protocol and this engine's API (documented
// below, not glossed over).
//
// ITCH message to engine operation mapping:
//   'A'/'F' Add Order            -> addLimit
//   'D'     Order Delete         -> cancel
//   'X'     Order Cancel (part.) -> reduceQty (or cancel if it zeroes out)
//   'E'/'C' Order Executed       -> reduceQty (or cancel if it zeroes out)
//   'U'     Order Replace        -> cancel(old) + addLimit(new, ...)
//
// Deliberately NOT a 1:1 protocol-to-engine mapping, because ITCH's
// TotalView-style feed only publishes the *outcome* of matching (an
// Executed message says a resting order's quantity shrank by N shares),
// never a replayable incoming aggressive order for the other side of
// that trade. Replaying 'E'/'C' as reduceQty/cancel is the closest
// honest translation: it reproduces the real quantity-decrease and
// arrival pattern on the resting side, same realistic price/size/timing
// distributions as the real market, without pretending our engine
// "discovered" a trade it has no data to reconstruct. 'U' Replace maps
// to cancel+addLimit rather than this project's own replacePrice,
// because ITCH's Replace can change both price AND quantity and always
// assigns a new order reference number, which doesn't fit
// replacePrice's same-id, price-only contract.
//
// Usage: ./orderbook_itch_replay <path-to-decompressed-ITCH-file> [SYMBOL]
// SYMBOL defaults to AAPL. See tools/README.md for how to get a file.

#include "itch/itch_reader.hpp"
#include "orderbook/fast_order_book.hpp"
#include "orderbook/order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace orderbook;
using namespace itch;
using Clock = std::chrono::steady_clock;

namespace {

enum class OpKind { AddLimit, Cancel, ReduceQty };

struct ItchOp {
    OpKind kind;
    OrderId id;
    Side side;    // AddLimit only
    Price price;  // AddLimit only
    Qty qty;      // AddLimit (initial) or ReduceQty (new remaining)
};

std::string trimmed(const std::uint8_t* p, std::size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// Real feeds include a small fraction of orders at deliberately extreme
// prices (effectively marketable limits, e.g. a buy at $199,999.99 that
// will match almost anything resting). Real exchanges reject these at a
// price-collar gateway check before they ever reach matching; filtering
// them here is the same idea, not a cover-up. This band ($50-$500)
// covers 99.71% of real AAPL orders in the sample used to pick it.
constexpr Price kPriceBandMin = 5000;   // $50.00, in cents
constexpr Price kPriceBandMax = 50000;  // $500.00, in cents

// Pass 1: scan the whole file for Stock Directory ('R') records, which
// NASDAQ emits once per listed symbol, near the start of the day. Builds
// the locate-code -> ticker map needed to find which locate code our
// target symbol is (ITCH filters by locate code, not by ticker text, on
// every other message type).
std::unordered_map<std::uint16_t, std::string> readStockDirectory(const std::string& path) {
    std::unordered_map<std::uint16_t, std::string> locateToSymbol;
    Reader reader(path);
    while (auto msg = reader.next()) {
        if (msg->type == 'R' && msg->length >= 19) {
            locateToSymbol[msg->stockLocate] = trimmed(msg->body + 11, 8);
        }
    }
    return locateToSymbol;
}

// Pass 2: scan the whole file again, translating every message for the
// target locate code into an ItchOp, in file order. orderSide_/
// orderRemaining_ are a minimal per-order ledger, just enough to resolve
// each translation (which side an order is on, how much of it is left),
// not a reimplementation of the matching engine itself.
class Translator {
public:
    std::vector<ItchOp> run(const std::string& path, std::uint16_t targetLocate) {
        Reader reader(path);
        std::vector<ItchOp> ops;
        std::size_t skippedUnknownRef = 0;
        std::size_t skippedPriceOutOfBand = 0;

        while (auto msg = reader.next()) {
            if (msg->stockLocate != targetLocate) continue;
            const std::uint8_t* b = msg->body;

            switch (msg->type) {
                case 'A':
                case 'F': {  // Add Order [with MPID]: first 36 bytes identical either way
                    if (msg->length < 36) break;
                    OrderId ref = readU64BE(b + 11);
                    Side side = (b[19] == 'B') ? Side::Buy : Side::Sell;
                    Qty shares = readU32BE(b + 20);
                    // ITCH prices are 1/10000 dollar; /100 to our own
                    // cent ticks, matching this project's documented
                    // tick philosophy (and keeping prices for any
                    // normally-priced stock well under kMaxPrice).
                    Price price = static_cast<Price>(readU32BE(b + 32) / 100);
                    if (price < kPriceBandMin || price > kPriceBandMax) {
                        ++skippedPriceOutOfBand;
                        break;
                    }
                    orderSide_[ref] = side;
                    orderRemaining_[ref] = shares;
                    ops.push_back(ItchOp{OpKind::AddLimit, ref, side, price, shares});
                    break;
                }
                case 'D': {  // Order Delete
                    if (msg->length < 19) break;
                    OrderId ref = readU64BE(b + 11);
                    if (!orderRemaining_.contains(ref)) {
                        ++skippedUnknownRef;
                        break;
                    }
                    ops.push_back(ItchOp{OpKind::Cancel, ref, Side::Buy, 0, 0});
                    orderSide_.erase(ref);
                    orderRemaining_.erase(ref);
                    break;
                }
                case 'X': {  // Order Cancel (partial)
                    if (msg->length < 23) break;
                    OrderId ref = readU64BE(b + 11);
                    Qty cancelled = readU32BE(b + 19);
                    applyReduction(ops, ref, cancelled, skippedUnknownRef);
                    break;
                }
                case 'E': {  // Order Executed
                    if (msg->length < 31) break;
                    OrderId ref = readU64BE(b + 11);
                    Qty executed = readU32BE(b + 19);
                    applyReduction(ops, ref, executed, skippedUnknownRef);
                    break;
                }
                case 'C': {  // Order Executed With Price (first 31 bytes same shape as 'E')
                    if (msg->length < 31) break;
                    OrderId ref = readU64BE(b + 11);
                    Qty executed = readU32BE(b + 19);
                    applyReduction(ops, ref, executed, skippedUnknownRef);
                    break;
                }
                case 'U': {  // Order Replace: new reference number, can change price AND qty
                    if (msg->length < 35) break;
                    OrderId oldRef = readU64BE(b + 11);
                    OrderId newRef = readU64BE(b + 19);
                    Qty newShares = readU32BE(b + 27);
                    Price newPrice = static_cast<Price>(readU32BE(b + 31) / 100);  // see the /100 note above
                    auto it = orderSide_.find(oldRef);
                    if (it == orderSide_.end()) {
                        ++skippedUnknownRef;
                        break;
                    }
                    Side side = it->second;
                    ops.push_back(ItchOp{OpKind::Cancel, oldRef, Side::Buy, 0, 0});
                    orderSide_.erase(it);
                    orderRemaining_.erase(oldRef);
                    // The old order is gone either way (it's replaced);
                    // only track and replay the new one if its price
                    // falls inside the same band the 'A'/'F' case uses.
                    if (newPrice < kPriceBandMin || newPrice > kPriceBandMax) {
                        ++skippedPriceOutOfBand;
                        break;
                    }
                    orderSide_[newRef] = side;
                    orderRemaining_[newRef] = newShares;
                    ops.push_back(ItchOp{OpKind::AddLimit, newRef, side, newPrice, newShares});
                    break;
                }
                default:
                    break;  // not a message type this replay acts on
            }
        }

        std::fprintf(stderr,
                      "itch replay: %zu ops translated, %zu referenced an order outside our window (skipped), "
                      "%zu skipped for being outside the price band\n",
                      ops.size(), skippedUnknownRef, skippedPriceOutOfBand);
        return ops;
    }

private:
    void applyReduction(std::vector<ItchOp>& ops, OrderId ref, Qty amount, std::size_t& skippedUnknownRef) {
        auto it = orderRemaining_.find(ref);
        if (it == orderRemaining_.end()) {
            ++skippedUnknownRef;
            return;
        }
        if (amount >= it->second) {
            ops.push_back(ItchOp{OpKind::Cancel, ref, Side::Buy, 0, 0});
            orderSide_.erase(ref);
            orderRemaining_.erase(it);
        } else {
            it->second -= amount;
            ops.push_back(ItchOp{OpKind::ReduceQty, ref, Side::Buy, 0, it->second});
        }
    }

    std::unordered_map<OrderId, Side> orderSide_;
    std::unordered_map<OrderId, Qty> orderRemaining_;
};

template <typename Book>
void apply(Book& book, const ItchOp& op) {
    switch (op.kind) {
        case OpKind::AddLimit:
            book.addLimit(op.id, op.side, op.price, op.qty);
            break;
        case OpKind::Cancel:
            book.cancel(op.id);
            break;
        case OpKind::ReduceQty:
            book.reduceQty(op.id, op.qty);
            break;
    }
}

template <typename Book>
void benchmark(const char* label, const std::vector<ItchOp>& ops) {
    Book book;
    Clock::time_point start = Clock::now();
    for (const ItchOp& op : ops) apply(book, op);
    Clock::time_point end = Clock::now();

    double seconds = std::chrono::duration<double>(end - start).count();
    std::printf("  [%s] %zu ops in %.4fs, %.0f ops/sec\n", label, ops.size(), seconds,
                static_cast<double>(ops.size()) / seconds);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-decompressed-ITCH-file> [SYMBOL]\n", argv[0]);
        return 1;
    }
    std::string path = argv[1];
    std::string symbol = (argc >= 3) ? argv[2] : "AAPL";

    std::fprintf(stderr, "itch replay: scanning stock directory in %s...\n", path.c_str());
    std::unordered_map<std::uint16_t, std::string> directory = readStockDirectory(path);
    std::fprintf(stderr, "itch replay: %zu symbols in directory\n", directory.size());

    std::uint16_t targetLocate = 0;
    bool found = false;
    for (const auto& [locate, sym] : directory) {
        if (sym == symbol) {
            targetLocate = locate;
            found = true;
            break;
        }
    }
    if (!found) {
        std::fprintf(stderr, "itch replay: symbol %s not found in this file's stock directory\n", symbol.c_str());
        return 1;
    }
    std::fprintf(stderr, "itch replay: %s -> locate %u\n", symbol.c_str(), targetLocate);

    Translator translator;
    std::vector<ItchOp> ops = translator.run(path, targetLocate);
    if (ops.empty()) {
        std::fprintf(stderr, "itch replay: no ops translated for %s in this file\n", symbol.c_str());
        return 1;
    }

    std::size_t addCount = 0, cancelCount = 0, reduceCount = 0;
    for (const ItchOp& op : ops) {
        if (op.kind == OpKind::AddLimit) ++addCount;
        else if (op.kind == OpKind::Cancel) ++cancelCount;
        else ++reduceCount;
    }
    std::printf("orderbook_itch_replay: %s, %zu real ops (%zu add, %zu cancel, %zu reduceQty)\n", symbol.c_str(),
                ops.size(), addCount, cancelCount, reduceCount);

    benchmark<OrderBook>("v1", ops);
    benchmark<FastOrderBook>("v2", ops);

    return 0;
}
