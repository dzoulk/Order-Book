#pragma once

// Minimal reader for NASDAQ ITCH 5.0 research-distribution files: a flat
// stream of [2-byte big-endian length][message body of that length]
// records, body[0] being the 1-byte message type. This is the format
// NASDAQ's downloadable historical sample files use (not the raw
// MoldUDP64 multicast framing, which carries sequence numbers too).
//
// Only reads; doesn't interpret message bodies beyond the type byte and
// the common header (stock locate) every message type shares. Field
// layouts for the message types this project actually acts on live in
// itch_replay_main.cpp, next to where they're used, rather than as a
// a full protocol library, this project only needs a slice of ITCH 5.0.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace itch {

struct RawMessage {
    char type;
    std::uint16_t stockLocate;
    const std::uint8_t* body;  // full message body, body[0] == type; valid until the next next() call
    std::size_t length;        // body length, including the type byte
};

// Reads fixed-framing ITCH records from a plain (already decompressed)
// file. Tolerates a truncated trailing record at EOF (expected when
// working from a partial/range-downloaded sample) by just stopping
// there instead of erroring.
class Reader {
public:
    explicit Reader(const std::string& path);
    ~Reader();

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // Returns the next message, or nullopt once there's no complete
    // message left to read (true EOF, or a truncated trailing one).
    std::optional<RawMessage> next();

private:
    std::FILE* file_;
    std::vector<std::uint8_t> buf_;
};

inline std::uint16_t readU16BE(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

inline std::uint32_t readU32BE(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

inline std::uint64_t readU64BE(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

} // namespace itch
