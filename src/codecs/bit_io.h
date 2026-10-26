#pragma once

#include <liblha/lha.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace lha::detail {

class BitReader {
public:
    explicit BitReader(std::span<const Byte> data) : data_(data) {}

    bool read(unsigned int count, std::uint32_t& value) {
        if (count > 32 
        ||  bitPosition_ > data_.size() * 8 
        ||  count > data_.size() * 8 - bitPosition_
        ) return false;
        
        value = 0;
        for (unsigned int i = 0; i < count; ++i) {
            value = (value << 1) | ((data_[bitPosition_ / 8] >> (7 - bitPosition_ % 8)) & 1u);
            ++bitPosition_;
        }

        return true;
    }

    bool consumedAllBytes() const {
        return (bitPosition_ + 7) / 8 == data_.size();
    }

    bool hasValidPadding() const {
        if (data_.size() * 8 - bitPosition_ > 7) {
            return false;
        }

        for (std::size_t bit = bitPosition_; bit < data_.size() * 8; ++bit) {
            const std::size_t byteIndex = bit / 8;
            const unsigned int bitIndex = static_cast<unsigned int>(7 - bit % 8);
            if (((data_[byteIndex] >> bitIndex) & 1u) != 0) {
                return false;
            }
        }

        return true;
    }

private:
    std::span<const Byte> data_;
    std::size_t bitPosition_ = 0;
    
};

class BitWriter {
public:
    void write(unsigned int count, std::uint32_t value) {
        for (unsigned int bit = count; bit > 0; --bit) {
            current_ = static_cast<Byte>((current_ << 1) | ((value >> (bit - 1)) & 1u));

            if (++used_ == 8) {
                data_.push_back(current_);
                current_ = 0;
                used_ = 0;
            }
        }
    }

    Bytes finish() {
        if (used_ != 0) {
            data_.push_back(static_cast<Byte>(current_ << (8 - used_)));
        }

        return std::move(data_);
    }

private:
    Bytes data_;
    Byte current_ = 0;
    unsigned int used_ = 0;
};

} // namespace lha::detail