#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nexus::core {

/**
 * @brief High-efficiency contiguous network/protocol byte buffer with read/write cursors
 *        and Big-Endian (network byte order) read/write helpers for RTP/RTCP/RTSP/FLV/MP4.
 */
class ByteBuffer {
public:
    static constexpr size_t kCheapPrepend = 8;
    static constexpr size_t kInitialSize = 4096;

    explicit ByteBuffer(size_t initial_size = kInitialSize)
        : buffer_(kCheapPrepend + initial_size),
          reader_index_(kCheapPrepend),
          writer_index_(kCheapPrepend) {}

    size_t ReadableBytes() const noexcept { return writer_index_ - reader_index_; }
    size_t WritableBytes() const noexcept { return buffer_.size() - writer_index_; }
    size_t PrependableBytes() const noexcept { return reader_index_; }

    const uint8_t* Peek() const noexcept { return buffer_.data() + reader_index_; }
    uint8_t* BeginWrite() noexcept { return buffer_.data() + writer_index_; }
    const uint8_t* BeginWrite() const noexcept { return buffer_.data() + writer_index_; }

    std::span<const uint8_t> ReadableSpan() const noexcept {
        return {Peek(), ReadableBytes()};
    }

    std::string_view ReadableStringView() const noexcept {
        return {reinterpret_cast<const char*>(Peek()), ReadableBytes()};
    }

    void Retrieve(size_t len) noexcept {
        if (len < ReadableBytes()) {
            reader_index_ += len;
        } else {
            RetrieveAll();
        }
    }

    void RetrieveAll() noexcept {
        reader_index_ = kCheapPrepend;
        writer_index_ = kCheapPrepend;
    }

    std::string RetrieveAsString(size_t len) {
        len = std::min(len, ReadableBytes());
        std::string result(reinterpret_cast<const char*>(Peek()), len);
        Retrieve(len);
        return result;
    }

    std::string RetrieveAllAsString() { return RetrieveAsString(ReadableBytes()); }

    void EnsureWritableBytes(size_t len) {
        if (WritableBytes() < len) {
            MakeSpace(len);
        }
    }

    void HasWritten(size_t len) noexcept { writer_index_ += len; }

    void Append(const void* data, size_t len) {
        if (len == 0) return;
        EnsureWritableBytes(len);
        std::memcpy(BeginWrite(), data, len);
        HasWritten(len);
    }

    void Append(std::span<const uint8_t> data) { Append(data.data(), data.size()); }
    void Append(std::string_view str) { Append(str.data(), str.size()); }

    // Big-endian (Network Byte Order) writers
    void WriteU8(uint8_t val) { Append(&val, 1); }

    void WriteU16BE(uint16_t val) {
        uint8_t buf[2] = {
            static_cast<uint8_t>((val >> 8) & 0xFF),
            static_cast<uint8_t>(val & 0xFF)};
        Append(buf, 2);
    }

    void WriteU24BE(uint32_t val) {
        uint8_t buf[3] = {
            static_cast<uint8_t>((val >> 16) & 0xFF),
            static_cast<uint8_t>((val >> 8) & 0xFF),
            static_cast<uint8_t>(val & 0xFF)};
        Append(buf, 3);
    }

    void WriteU32BE(uint32_t val) {
        uint8_t buf[4] = {
            static_cast<uint8_t>((val >> 24) & 0xFF),
            static_cast<uint8_t>((val >> 16) & 0xFF),
            static_cast<uint8_t>((val >> 8) & 0xFF),
            static_cast<uint8_t>(val & 0xFF)};
        Append(buf, 4);
    }

    // Big-endian readers
    uint8_t ReadU8() {
        if (ReadableBytes() < 1) throw std::out_of_range("ByteBuffer underflow (U8)");
        uint8_t val = *Peek();
        Retrieve(1);
        return val;
    }

    uint16_t ReadU16BE() {
        if (ReadableBytes() < 2) throw std::out_of_range("ByteBuffer underflow (U16BE)");
        const uint8_t* p = Peek();
        uint16_t val = (static_cast<uint16_t>(p[0]) << 8) | p[1];
        Retrieve(2);
        return val;
    }

    uint32_t ReadU24BE() {
        if (ReadableBytes() < 3) throw std::out_of_range("ByteBuffer underflow (U24BE)");
        const uint8_t* p = Peek();
        uint32_t val = (static_cast<uint32_t>(p[0]) << 16) |
                       (static_cast<uint32_t>(p[1]) << 8) | p[2];
        Retrieve(3);
        return val;
    }

    uint32_t ReadU32BE() {
        if (ReadableBytes() < 4) throw std::out_of_range("ByteBuffer underflow (U32BE)");
        const uint8_t* p = Peek();
        uint32_t val = (static_cast<uint32_t>(p[0]) << 24) |
                       (static_cast<uint32_t>(p[1]) << 16) |
                       (static_cast<uint32_t>(p[2]) << 8) | p[3];
        Retrieve(4);
        return val;
    }

    std::vector<uint8_t> ToVector() const {
        return std::vector<uint8_t>(Peek(), Peek() + ReadableBytes());
    }

private:
    void MakeSpace(size_t len) {
        if (WritableBytes() + PrependableBytes() < len + kCheapPrepend) {
            buffer_.resize(writer_index_ + len);
        } else {
            size_t readable = ReadableBytes();
            std::memmove(buffer_.data() + kCheapPrepend, Peek(), readable);
            reader_index_ = kCheapPrepend;
            writer_index_ = reader_index_ + readable;
        }
    }

    std::vector<uint8_t> buffer_;
    size_t reader_index_;
    size_t writer_index_;
};

}  // namespace nexus::core
