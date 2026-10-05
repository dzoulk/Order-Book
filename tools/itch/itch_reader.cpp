#include "itch_reader.hpp"

#include <stdexcept>

namespace itch {

Reader::Reader(const std::string& path) {
    file_ = std::fopen(path.c_str(), "rb");
    if (!file_) throw std::runtime_error("itch::Reader: could not open " + path);
}

Reader::~Reader() {
    if (file_) std::fclose(file_);
}

std::optional<RawMessage> Reader::next() {
    std::uint8_t lenBytes[2];
    if (std::fread(lenBytes, 1, 2, file_) != 2) return std::nullopt;  // EOF, cleanly

    std::uint16_t length = readU16BE(lenBytes);
    buf_.resize(length);
    if (std::fread(buf_.data(), 1, length, file_) != length) {
        return std::nullopt;  // truncated trailing record; stop here, not an error
    }

    RawMessage msg;
    msg.type = static_cast<char>(buf_[0]);
    msg.stockLocate = readU16BE(&buf_[1]);
    msg.body = buf_.data();
    msg.length = length;
    return msg;
}

} // namespace itch
