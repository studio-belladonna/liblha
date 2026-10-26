#include "codec_common.h"
#include "codec_modules.h"
#include "bit_io.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <vector>

// LH1 codec
// This supports 4 KiB sliding window uding dynamic Huffman encoding is
// Support a maximum of 60 bytes of matching length

namespace lha::detail {

namespace {
constexpr std::size_t minMatch = 3;
constexpr std::uint32_t maxFrequency = 0x8000;

class AdaptiveHuffman {
public:
    explicit AdaptiveHuffman(std::size_t maxMatch)
    :   characterSymbols_(254 + maxMatch), 
        treeSize_(characterSymbols_ * 2 - 1), 
        rootNode_(treeSize_ - 1), 
        frequency_(treeSize_ + 1), 
        child_(treeSize_ + characterSymbols_ + 1), 
        parent_(treeSize_ + characterSymbols_ + 1)
    {
        for (std::size_t i = 0; i < characterSymbols_; ++i) {
            frequency_[i] = 1;
            child_[i] = static_cast<int>(i + treeSize_);
            parent_[i + treeSize_] = static_cast<int>(i);
        }

        std::size_t i = 0;
        std::size_t j = characterSymbols_;

        while (j <= rootNode_) {
            frequency_[j] = frequency_[i] + frequency_[i + 1];
            child_[j] = static_cast<int>(i);
            parent_[i] = static_cast<int>(j);
            parent_[i + 1] = static_cast<int>(j);

            i += 2;
            ++j;
        }

        frequency_[treeSize_] = maxFrequency;
        parent_[rootNode_] = 0;
    }

    bool encode(BitWriter& writer, std::size_t symbol) {
        if (symbol >= characterSymbols_) {
            return false;
        }

        std::vector<Byte> path(treeSize_);
        std::size_t length = 0;
        int node = parent_[symbol + treeSize_];
        while (node != static_cast<int>(rootNode_)) {
            if (node < 0 || length >= path.size()) {
                return false;
            }
            path[length++] = static_cast<Byte>(symbolOrNodeIsRight(node));
            node = parent_[static_cast<std::size_t>(node)];
        }

        while (length != 0) {
            writer.write(1, path[--length]);
        }

        update(symbol);
        return true;
    }

    bool decode(BitReader& reader, std::size_t& symbol) {
        int node = static_cast<int>(rootNode_);
        while (node >= 0
            && static_cast<std::size_t>(node) < treeSize_ 
            && child_[static_cast<std::size_t>(node)] < static_cast<int>(treeSize_)
        ) {
            std::uint32_t bit = 0;
            if (!reader.read(1, bit)) {
                return false;
            }

            const std::size_t internal = static_cast<std::size_t>(node);
            node = child_[internal] + static_cast<int>(bit);
        }

        if (node < 0 || static_cast<std::size_t>(node) >= treeSize_) {
            return false;
        }

        const int decoded = child_[static_cast<std::size_t>(node)] - static_cast<int>(treeSize_);
        if (decoded < 0 || static_cast<std::size_t>(decoded) >= characterSymbols_) {
            return false;
        }


        symbol = static_cast<std::size_t>(decoded);
        update(symbol);

        return true;
    }

private:
    bool symbolOrNodeIsRight(int node) const {
        const int parent = parent_[static_cast<std::size_t>(node)];
        return child_[static_cast<std::size_t>(parent)] != node;
    }

    void swapNodes(std::size_t left, std::size_t right) {
        const int leftChild = child_[left];
        const int rightChild = child_[right];

        child_[left] = rightChild;
        child_[right] = leftChild;

        parent_[static_cast<std::size_t>(leftChild)] = static_cast<int>(right);
        parent_[static_cast<std::size_t>(rightChild)] = static_cast<int>(left);

        if (leftChild < static_cast<int>(treeSize_)) {
            parent_[static_cast<std::size_t>(leftChild + 1)] = static_cast<int>(right);
        }

        if (rightChild < static_cast<int>(treeSize_)) {
            parent_[static_cast<std::size_t>(rightChild + 1)] = static_cast<int>(left);
        }
    }

    void reconstruct() {
        std::size_t leafCount = 0;
        for (std::size_t i = 0; i < treeSize_; ++i) {
            if (child_[i] >= static_cast<int>(treeSize_)) {
                frequency_[leafCount] = (frequency_[i] + 1) / 2;
                child_[leafCount] = child_[i];
                ++leafCount;
            }
        }

        std::size_t i = 0;
        std::size_t j = characterSymbols_;
        while (j < treeSize_) {
            const std::uint32_t combined = frequency_[i] + frequency_[i + 1];
            std::size_t insert = j;
            while (insert > 0 && combined < frequency_[insert - 1]) {
                --insert;
            }

            if (insert != j) {
                for (std::size_t move = j; move > insert; --move) {
                    frequency_[move] = frequency_[move - 1];
                    child_[move] = child_[move - 1];
                }

                frequency_[insert] = combined;
                child_[insert] = static_cast<int>(i);
            } else {
                frequency_[j] = combined;
                child_[j] = static_cast<int>(i);
            }

            i += 2;
            ++j;
        }

        for (std::size_t node = 0; node < treeSize_; ++node) {
            const int child = child_[node];
            if (child >= static_cast<int>(treeSize_)) {
                parent_[static_cast<std::size_t>(child)] = static_cast<int>(node);

            } else {
                parent_[static_cast<std::size_t>(child)] = static_cast<int>(node);
                parent_[static_cast<std::size_t>(child + 1)] = static_cast<int>(node);
            }
        }

        frequency_[treeSize_] = maxFrequency;
        parent_[rootNode_] = 0;
    }

    void update(std::size_t symbol) {
        if (frequency_[rootNode_] == maxFrequency) {
            reconstruct();
        }

        int node = parent_[symbol + treeSize_];
        while (node != 0) {
            const std::size_t index = static_cast<std::size_t>(node);
            const std::uint32_t value = ++frequency_[index];
            std::size_t leader = index + 1;

            while (value > frequency_[leader]) {
                ++leader;
            }

            --leader;
            if (leader != index) {
                std::swap(frequency_[index], frequency_[leader]);
                swapNodes(index, leader);
                node = static_cast<int>(leader);
            }

            node = parent_[static_cast<std::size_t>(node)];
        }
    }

    std::size_t characterSymbols_;
    std::size_t treeSize_;
    std::size_t rootNode_;
    std::vector<std::uint32_t> frequency_;
    std::vector<int> child_;
    std::vector<int> parent_;
};

class PositionCodes {
public:
    PositionCodes(std::size_t prefixes, unsigned int lowBits)
    :   lowBits_(lowBits),
        maxCodeBits_(prefixes == 64 ? 8 : std::bit_width(prefixes - 1)),
        lengths_(prefixes), 
        codes_(prefixes), 
        counts_(maxCodeBits_ + 1)
    {
        for (std::size_t symbol = 0; symbol < prefixes; ++symbol) {
            if (prefixes != 64) {
                lengths_[symbol] = static_cast<std::uint8_t>(maxCodeBits_);

                continue;
            }

            lengths_[symbol] 
            =   symbol == 0 ? 3 
            :   symbol < 4 ? 4 
            :   symbol < 12 ? 5 
            :   symbol < 24 ? 6 
            :   symbol < 48 ? 7 
            :   8;
        }

        std::vector<std::uint32_t> nextCode(maxCodeBits_ + 1);
        for (std::size_t bits = 1; bits <= maxCodeBits_; ++bits) {
            nextCode[bits] = (nextCode[bits - 1] + counts_[bits - 1]) << 1;

            for (std::uint8_t length : lengths_) {
                if (length == bits) {
                    ++counts_[bits];
                }
            }
        }

        for (std::size_t symbol = 0; symbol < lengths_.size(); ++symbol) {
            codes_[symbol] = nextCode[lengths_[symbol]]++;
        }
    }

    void encode(BitWriter& writer, std::size_t position) const {
        const std::size_t prefix = position >> lowBits_;
        writer.write(lengths_[prefix], codes_[prefix]);
        writer.write(lowBits_, static_cast<std::uint32_t>(position & ((std::size_t{1} << lowBits_) - 1)));
    }

    bool decode(BitReader& reader, std::size_t& position) const {
        std::uint32_t code = 0;
        std::size_t prefix = lengths_.size();
        for (unsigned int bits = 1; bits <= maxCodeBits_; ++bits) {
            std::uint32_t bit = 0;
            if (!reader.read(1, bit)) {
                return false;
            }

            code = (code << 1) | bit;
            for (std::size_t symbol = 0; symbol < lengths_.size(); ++symbol) {
                if (lengths_[symbol] == bits && codes_[symbol] == code) {
                    prefix = symbol;

                    break;
                }
            }

            if (prefix != lengths_.size()) {
                break;
            }
        }

        if (prefix == lengths_.size()) {
            return false;
        }

        std::uint32_t low = 0;
        if (!reader.read(lowBits_, low)) {
            return false;
        }

        position = (prefix << lowBits_) | low;
        return true;
    }

private:
    unsigned int lowBits_;
    unsigned int maxCodeBits_;
    std::vector<std::uint8_t> lengths_;
    std::vector<std::uint32_t> codes_;
    std::vector<std::uint32_t> counts_;
};

std::uint16_t hashAt(std::span<const Byte> input, std::size_t position) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(input[position]) * 251u + input[position + 1]) * 251u + input[position + 2]
    );
}

std::size_t findMatch(
    std::span<const Byte> input, 
    std::size_t position, 
    std::size_t& distance, 
    const std::vector<std::size_t>& heads, 
    const std::vector<std::size_t>& previous, 
    std::size_t windowSize, 
    std::size_t maxMatch
) {
    std::size_t bestLength = 0;
    distance = 0;
    if (position + minMatch > input.size()) {
        return bestLength;
    }

    std::size_t candidate = heads[hashAt(input, position)];
    unsigned int attempts = 0;
    while (candidate != std::numeric_limits<std::size_t>::max() 
        && position - candidate <= windowSize 
        && attempts++ < 64
    ) {
        std::size_t length = 0;
        while (length < maxMatch 
            && position + length < input.size() 
            && input[candidate + length] == input[position + length]
        ) {
            ++length;
        }

        if (length > bestLength) {
            bestLength = length;
            distance = position - candidate;

            if (bestLength == maxMatch) {
                break;
            }
        }

        candidate = previous[candidate];
    }

    return bestLength;
}
} // namespace

Result compressLhAdaptive(
    std::span<const Byte> input, 
    const Options& options, 
    std::size_t windowSize, 
    std::size_t maxMatch, 
    std::size_t positionPrefixes, 
    unsigned int lowPositionBits
) {
    if (!outputSizeAllowed(input.size(), options)) {
        return codecFailure(
            Error::OutputLimitExceeded,
            "Input exceeds configured size limit"
        );
    }

    Result result;
    if (input.empty()) {
        return result;
    }

    AdaptiveHuffman huffman(maxMatch);
    PositionCodes positions(positionPrefixes, lowPositionBits);
    BitWriter writer;

    const std::size_t missing = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> heads(65536, missing);
    std::vector<std::size_t> previous(input.size(), missing);

    std::size_t offset = 0;
    while (offset < input.size()) {
        std::size_t distance = 0;
        const std::size_t matchLength = findMatch(input, offset, distance, heads, previous, windowSize, maxMatch);
        const std::size_t consumed = matchLength >= minMatch ? std::min(matchLength, maxMatch) : 1;

        if (matchLength >= minMatch) {
            if (!huffman.encode(writer, 255 - 2 + consumed)) {
                return codecFailure(
                    Error::InvalidInput,
                    "Unable to encode adaptive LH match length"
                );
            }

            positions.encode(writer, distance - 1);
        } else {
            if (!huffman.encode(writer, input[offset])) {
                return codecFailure(
                    Error::InvalidInput,
                    "Unable to encode adaptive LH literal"
                );
            }
        }

        for (std::size_t i = 0; i < consumed; ++i) {
            const std::size_t current = offset + i;
            if (current + 2 < input.size()) {
                const std::uint16_t hash = hashAt(input, current);

                previous[current] = heads[hash];
                heads[hash] = current;
            }
        }

        offset += consumed;
    }

    result.data = writer.finish();
    return result;
}

Result decompressLhAdaptive(
    std::span<const Byte> input,
    std::uint64_t expectedSize,
    const Options& options,
    std::size_t windowSize,
    std::size_t maxMatch,
    std::size_t positionPrefixes,
    unsigned int lowPositionBits
) {
    if (!outputSizeAllowed(expectedSize, options)) {
        return codecFailure(
            Error::OutputLimitExceeded,
            "Expected output exceeds configured or platform size limit"
        );
    }

    if (expectedSize == 0) {
        if (!input.empty()) {
            return codecFailure(
                Error::InvalidInput,
                "Unexpected data in empty LH1 stream"
            );
        }
        return {};
    }

    if (input.empty()) {
        return codecFailure(
            Error::InvalidInput,
            "LH1 stream is empty"
        );
    }

    BitReader reader(input);
    AdaptiveHuffman huffman(maxMatch);
    PositionCodes positions(positionPrefixes, lowPositionBits);

    Bytes window(windowSize, 0x20);
    std::size_t windowPosition = windowSize - maxMatch;

    Bytes output;
    output.reserve(static_cast<std::size_t>(expectedSize));

    while (output.size() < expectedSize) {
        std::size_t symbol = 0;
        if (!huffman.decode(reader, symbol)) {
            return codecFailure(
                Error::InvalidInput,
                "Truncated or invalid adaptive LH Huffman stream"
            );
        }

        if (symbol < 256) {
            const Byte value = static_cast<Byte>(symbol);
            output.push_back(value);

            window[windowPosition] = value;
            windowPosition = (windowPosition + 1) % windowSize;
        } else {
            const std::size_t length = symbol - 255 + 2;
            std::size_t position = 0;
            if (!positions.decode(reader, position)) {
                return codecFailure(
                    Error::InvalidInput,
                    "Truncated or invalid adaptive LH match position"
                );
            }

            const std::size_t distance = position + 1;
            if (length > expectedSize - output.size()) {
                return codecFailure(
                    Error::SizeMismatch,
                    "LH1 match exceeds expected output size"
                );
            }

            for (std::size_t i = 0; i < length; ++i) {
                const Byte value = window[(windowPosition + windowSize - distance) % windowSize];
                output.push_back(value);
                window[windowPosition] = value;
                windowPosition = (windowPosition + 1) % windowSize;
            }
        }
    }

    if (!reader.consumedAllBytes()) {
        return codecFailure(
            Error::InvalidInput,
            "Trailing bytes follow the LH1 stream"
        );
    }

    Result result;
    result.data = std::move(output);
    return result;
}

Result compressLh1(std::span<const Byte> input, const Options& options) {
    return compressLhAdaptive(input, options, 4096, 60, 64, 6);
}

Result decompressLh1(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    return decompressLhAdaptive(input, expectedSize, options, 4096, 60, 64, 6);
}

} // namespace lha::detail