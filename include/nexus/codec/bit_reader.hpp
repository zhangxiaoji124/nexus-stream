#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace nexus::codec {

/**
 * @brief Bit-level bitstream reader supporting ITU-T H.264 / H.265 Exponential-Golomb
 *        coding: u(n), ue(v), and se(v).
 */
class BitReader {
public:
    explicit BitReader(std::span<const uint8_t> data)
        : data_(data), byte_offset_(0), bit_offset_(0) {}

    bool HasBits(size_t count = 1) const noexcept {
        return BitsRemaining() >= count;
    }

    size_t BitsRemaining() const noexcept {
        if (byte_offset_ >= data_.size()) return 0;
        return (data_.size() - byte_offset_) * 8 - bit_offset_;
    }

    uint32_t ReadBit() {
        if (byte_offset_ >= data_.size()) {
            throw std::out_of_range("BitReader: out of bits");
        }
        uint32_t bit = (data_[byte_offset_] >> (7 - bit_offset_)) & 0x01;
        if (++bit_offset_ == 8) {
            bit_offset_ = 0;
            ++byte_offset_;
        }
        return bit;
    }

    uint32_t ReadBits(size_t n) {
        if (n > 32) {
            throw std::invalid_argument("BitReader::ReadBits supports up to 32 bits");
        }
        uint32_t val = 0;
        for (size_t i = 0; i < n; ++i) {
            val = (val << 1) | ReadBit();
        }
        return val;
    }

    /**
     * @brief Unsigned Exponential-Golomb code ue(v) per ITU-T H.264 Section 9.1:
     *        code_num = 2^leadingZeroBits - 1 + read_bits(leadingZeroBits)
     */
    uint32_t ReadUE() {
        size_t leading_zero_bits = 0;
        while (HasBits(1) && ReadBit() == 0) {
            ++leading_zero_bits;
            if (leading_zero_bits > 31) {
                throw std::runtime_error("BitReader::ReadUE: corrupted Exp-Golomb code");
            }
        }
        if (leading_zero_bits == 0) {
            return 0;
        }
        uint32_t suffix = ReadBits(leading_zero_bits);
        return ((1u << leading_zero_bits) - 1u) + suffix;
    }

    /**
     * @brief Signed Exponential-Golomb code se(v) per ITU-T H.264 Section 9.1.1:
     *        (-1)^(k+1) * Ceil(k / 2)
     */
    int32_t ReadSE() {
        uint32_t code_num = ReadUE();
        int32_t val = static_cast<int32_t>((code_num + 1) >> 1);
        return (code_num & 1) ? val : -val;
    }

private:
    std::span<const uint8_t> data_;
    size_t byte_offset_;
    size_t bit_offset_;
};

/**
 * @brief Bit-level writer for constructing H.264 RBSP (SPS/PPS/Slice headers) and AAC headers.
 */
class BitWriter {
public:
    void WriteBit(uint32_t bit) {
        if (bit_offset_ == 0) {
            buffer_.push_back(0);
        }
        if (bit & 1) {
            buffer_.back() |= static_cast<uint8_t>(1u << (7 - bit_offset_));
        }
        bit_offset_ = (bit_offset_ + 1) & 7;
    }

    void WriteBits(uint32_t val, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            uint32_t bit = (val >> (n - 1 - i)) & 1u;
            WriteBit(bit);
        }
    }

    void WriteUE(uint32_t val) {
        uint32_t code_num = val + 1;
        size_t bits = 0;
        uint32_t tmp = code_num;
        while (tmp > 0) {
            ++bits;
            tmp >>= 1;
        }
        size_t leading_zeros = bits - 1;
        for (size_t i = 0; i < leading_zeros; ++i) {
            WriteBit(0);
        }
        WriteBits(code_num, bits);
    }

    void WriteSE(int32_t val) {
        uint32_t code_num = (val <= 0)
                                ? static_cast<uint32_t>(-2 * val)
                                : static_cast<uint32_t>(2 * val - 1);
        WriteUE(code_num);
    }

    void WriteRbspTrailingBits() {
        WriteBit(1);
        while (bit_offset_ != 0) {
            WriteBit(0);
        }
    }

    const std::vector<uint8_t>& Data() const noexcept { return buffer_; }

private:
    std::vector<uint8_t> buffer_;
    size_t bit_offset_ = 0;
};

}  // namespace nexus::codec
