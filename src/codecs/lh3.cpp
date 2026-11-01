#include "codec_common.h"
#include "codec_modules.h"
#include "bit_io.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <queue>
#include <utility>

// LH3 codec
// Similar to LH2 but with static huffman

// thanks to Tsubusu05 for help with most of this

namespace lha::detail {

namespace {
constexpr std::size_t windowSize = 8192;
constexpr std::size_t minMatch = 3;
constexpr std::size_t maxMatch = 256;
constexpr std::size_t characterSymbols = 510;
constexpr std::size_t positionSymbols = 14;
constexpr std::size_t maxCodeBits = 16;

struct HuffmanNode {
    std::uint64_t frequency;
    int left;
    int right;
    std::size_t symbol;
};

struct QueueEntry {
    std::uint64_t frequency;
    std::size_t order;
    int node;
};

struct QueueEntryGreater {
    bool operator()(const QueueEntry& left, const QueueEntry& right) const {
        if (left.frequency != right.frequency) {
            return left.frequency > right.frequency;
        }

        return left.order > right.order;
    }
};

class HuffmanTree {
public:
    bool build(std::span<const std::uint8_t> lengths, unsigned int maxBits = maxCodeBits) {
        lengths_.assign(lengths.begin(), lengths.end());
        codes_.assign(lengths.size(), 0);
        counts_.assign(maxBits + 1, 0);
        firstCodes_.assign(maxBits + 1, 0);
        firstSymbols_.assign(maxBits + 1, 0);
        orderedSymbols_.clear();

        std::size_t active = 0;
        for (std::uint8_t length : lengths_) {
            if (length > maxBits) {
                return false;
            }

            if (length != 0) {
                ++counts_[length];
                ++active;
            }
        }

        if (active == 0) {
            return false;
        }

        std::uint32_t code = 0;
        std::size_t symbolOffset = 0;
        for (unsigned int bits = 1; bits <= maxBits; ++bits) {
            code = (code + static_cast<std::uint32_t>(counts_[bits - 1])) << 1;
            if (code + counts_[bits] > (std::uint32_t{1} << bits)) {
                return false;
            }

            firstCodes_[bits] = code;
            firstSymbols_[bits] = symbolOffset;
            symbolOffset += counts_[bits];
        }

        std::vector<std::size_t> ranks(maxBits + 1, 0);
        for (unsigned int bits = 1; bits <= maxBits; ++bits) {
            for (std::size_t symbol = 0; symbol < lengths_.size(); ++symbol) {
                if (lengths_[symbol] == bits) {
                    // probably should use the rank 'tank' function from the other one
                    codes_[symbol] = firstCodes_[bits] + static_cast<std::uint32_t>(ranks[bits]++);
                    orderedSymbols_.push_back(symbol);
                }
            }
        }

        return true;
    }

    bool encode(BitWriter& writer, std::size_t symbol) const {
        if (symbol >= lengths_.size() || lengths_[symbol] == 0) {
            return false;
        }

        writer.write(lengths_[symbol], codes_[symbol]);
        return true;
    }

    bool decode(BitReader& reader, std::size_t& symbol) const {
        std::uint32_t code = 0;
        for (std::size_t bits = 1; bits < counts_.size(); ++bits) {
            std::uint32_t bit = 0;
            if (!reader.read(1, bit)) {
                return false;
            }

            code = (code << 1) | bit;
            if (code >= firstCodes_[bits] && code - firstCodes_[bits] < counts_[bits]) {
                symbol = orderedSymbols_[firstSymbols_[bits] + code - firstCodes_[bits]];
                return true;
            }
        }

        return false;
    }

private:
    std::vector<std::uint8_t> lengths_;
    std::vector<std::uint32_t> codes_;
    std::vector<std::size_t> counts_;
    std::vector<std::uint32_t> firstCodes_;
    std::vector<std::size_t> firstSymbols_;
    std::vector<std::size_t> orderedSymbols_;
    
};

std::vector<std::uint8_t> makeCodeLengths(std::span<const std::uint64_t> frequencies, unsigned int limit) {
    std::vector<HuffmanNode> nodes;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, QueueEntryGreater> queue;
    for (std::size_t symbol = 0; symbol < frequencies.size(); ++symbol) {
        if (frequencies[symbol] != 0) {
            nodes.push_back({frequencies[symbol], -1, -1, symbol});
            queue.push({frequencies[symbol], symbol, static_cast<int>(nodes.size() - 1)});
        }
    }

    std::vector<std::uint8_t> lengths(frequencies.size(), 0);
    if (queue.empty()) {
        return lengths;
    }

    if (queue.size() == 1) {
        lengths[nodes[static_cast<std::size_t>(queue.top().node)].symbol] = 1;
        return lengths;
    }

    std::size_t order = frequencies.size();
    while (queue.size() > 1) {
        const QueueEntry left = queue.top();
        queue.pop();
        const QueueEntry right = queue.top();
        queue.pop();
        nodes.push_back({left.frequency + right.frequency, left.node, right.node, 0});
        queue.push({left.frequency + right.frequency, order++, static_cast<int>(nodes.size() - 1)});
    }

    std::vector<std::pair<int, unsigned int>> stack = {{queue.top().node, 0}};
    while (!stack.empty()) {
        const auto [nodeIndex, depth] = stack.back();
        stack.pop_back();
        const HuffmanNode& node = nodes[static_cast<std::size_t>(nodeIndex)];
        if (node.left < 0) {
            if (depth > limit) {
                return {};
            }

            lengths[node.symbol] = static_cast<std::uint8_t>(std::max(depth, 1u));
        } else {
            stack.push_back({node.left, depth + 1});
            stack.push_back({node.right, depth + 1});
        }
    }

    return lengths;
}

std::uint16_t hashAt(std::span<const Byte> input, std::size_t position) {
    const std::uint32_t byte0 = input[position];
    const std::uint32_t byte1 = input[position + 1];
    const std::uint32_t byte2 = input[position + 2];

    const std::uint32_t hash = (byte0 * 251u + byte1) * 251u + byte2;

    return static_cast<std::uint16_t>(hash);
}

struct Token {
    std::uint16_t symbol;
    std::uint16_t position;
};

bool readLengths(BitReader& reader, std::size_t count, std::vector<std::uint8_t>& lengths) {
    lengths.resize(count);
    for (std::uint8_t& length : lengths) {
        std::uint32_t value = 0;
        if (!reader.read(5, value) || value > maxCodeBits) {
            return false;
        }

        length = static_cast<std::uint8_t>(value);
    }

    return true;
}

struct Match {
    std::size_t length = 0;
    std::size_t distance = 0;
};

Match findBestMatch(
    std::span<const std::uint8_t> input,
    std::size_t offset,
    std::size_t head,
    std::span<const std::size_t> previous,
    std::size_t missing,
    std::size_t windowSize) 
{
    Match bestMatch;
    std::size_t candidate = head;
    unsigned int attempts = 0;

    while (candidate != missing && (offset - candidate) <= windowSize && attempts++ < 64) {
        std::size_t length = 0;
        
        while (length < maxMatch && 
               (offset + length) < input.size() && 
               input[candidate + length] == input[offset + length]
        ) {
            ++length;
        }

        if (length > bestMatch.length && length >= minMatch) {
            bestMatch.length = length;
            bestMatch.distance = offset - candidate;
            
            if (length == maxMatch) {
                break;
            }
        }

        candidate = previous[candidate];
    }

    return bestMatch;
}
} // namespace

Result compressLh3(std::span<const Byte> input, const Options& options) {
    if (!outputSizeAllowed(input.size(), options)) {
        return codecFailure(
            Error::OutputLimitExceeded,
            "Input exceeds configured size limit"
        );
    }

    if (input.size() > std::numeric_limits<std::uint32_t>::max()) {
        return codecFailure(
            Error::OutputLimitExceeded,
            "Input exceeds LH3 token count limit"
        );
    }

    Result result;
    if (input.empty()) {
        return result;
    }

    const std::size_t missing = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> heads(65536, missing);
    std::vector<std::size_t> previous(input.size(), missing);

    std::vector<Token> tokens;
    tokens.reserve(input.size());

    std::vector<std::uint64_t> characterFrequencies(characterSymbols, 0);
    std::vector<std::uint64_t> positionFrequencies(positionSymbols, 0);

    std::size_t offset = 0;
    while (offset < input.size()) {
        Match match;

        if (offset + minMatch <= input.size()) {
            const std::uint16_t currentHash = hashAt(input, offset);
            match = findBestMatch(input, offset, heads[currentHash], previous, missing, windowSize);
        }

        const std::size_t consumed = (match.length >= minMatch) ? match.length : 1;

        if (consumed == 1) {
            const std::size_t symbol = input[offset];
            tokens.push_back({static_cast<std::uint16_t>(symbol), 0});
            ++characterFrequencies[symbol];
        } else {
            const std::size_t symbol = consumed + 253;
            const std::size_t position = match.distance - 1;
            const std::size_t positionCode = (position == 0) ? 0 : std::bit_width(position);

            tokens.push_back({static_cast<std::uint16_t>(symbol), static_cast<std::uint16_t>(position)});
            ++characterFrequencies[symbol];
            ++positionFrequencies[positionCode];
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

    std::vector<std::uint8_t> characterLengths = makeCodeLengths(characterFrequencies, maxCodeBits);
    std::vector<std::uint8_t> positionLengths = makeCodeLengths(positionFrequencies, maxCodeBits);
    if (characterLengths.empty() 
    ||  positionLengths.empty()
    ) {
        return codecFailure(
            Error::InvalidInput,
            "Unable to create static LH3 Huffman tables"
        );
    }

    HuffmanTree characterTree;
    HuffmanTree positionTree;
    if (!characterTree.build(characterLengths) 
    ||  !positionTree.build(positionLengths)
    ) {
        return codecFailure(
            Error::InvalidInput,
            "Invalid static LH3 Huffman tables"
        );
    }

    BitWriter writer;
    writer.write(32, static_cast<std::uint32_t>(tokens.size()));
    for (std::uint8_t length : characterLengths) {
        writer.write(5, length);
    }

    for (std::uint8_t length : positionLengths) {
        writer.write(5, length);
    }

    for (const Token& token : tokens) {
        if (!characterTree.encode(writer, token.symbol)) {
            return codecFailure(
                Error::InvalidInput,
                "Unable to encode LH3 token"
            );
        }

        if (token.symbol >= 256) {
            const std::size_t positionCode = token.position == 0 ? 0 : std::bit_width(token.position);
            if (!positionTree.encode(writer, positionCode)) {
                return codecFailure(
                    Error::InvalidInput, 
                    "Unable to encode LH3 match position"
                );
            }
            
            if (positionCode != 0) {
                writer.write(
                    static_cast<unsigned int>(positionCode - 1), 
                    static_cast<std::uint32_t>(token.position - (std::size_t{1} << (positionCode - 1)))
                );
            }
        }
    }

    result.data = writer.finish();
    return result;
}

// ふしだら！

Result decompressLh3(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    if (!outputSizeAllowed(expectedSize, options)) {
        return codecFailure(
            Error::OutputLimitExceeded,
            "Expected output exceeds configured or platform size limit"
        );
    }

    if (expectedSize == 0) {
        if (input.empty()) {
            return {};
        }
        return codecFailure(
            Error::InvalidInput,
            "Unexpected data in empty LH3 stream"
        );
    }

    BitReader reader(input);
    std::uint32_t tokenCount = 0;
    if (!reader.read(32, tokenCount) || tokenCount == 0 || tokenCount > expectedSize) {
        return codecFailure(
            Error::InvalidInput,
            "Invalid LH3 token count"
        );
    }

    std::vector<std::uint8_t> characterLengths;
    std::vector<std::uint8_t> positionLengths;
    if (!readLengths(reader, characterSymbols, characterLengths) 
    ||  !readLengths(reader, positionSymbols, positionLengths)
    ) {
        return codecFailure(
            Error::InvalidInput,
            "Truncated LH3 Huffman tables"
        );
    }

    HuffmanTree characterTree;
    HuffmanTree positionTree;
    if (!characterTree.build(characterLengths) 
    ||  !positionTree.build(positionLengths)
    ) {
        return codecFailure(
            Error::InvalidInput,
            "Invalid LH3 Huffman tables"
        );
    }

    Bytes output;
    output.reserve(static_cast<std::size_t>(expectedSize));
    Bytes window(windowSize, 0x20);
    std::size_t windowPosition = windowSize - maxMatch;
    for (std::uint32_t token = 0; token < tokenCount; ++token) {
        std::size_t symbol = 0;
        if (!characterTree.decode(reader, symbol)) {
            return codecFailure(
                Error::InvalidInput,
                "Truncated LH3 token stream"
            );
        }

        if (symbol < 256) {
            if (output.size() >= expectedSize) {
                return codecFailure(
                    Error::SizeMismatch,
                    "LH3 stream expands beyond expected output size"
                );
            }

            const Byte value = static_cast<Byte>(symbol);
            output.push_back(value);
            window[windowPosition] = value;
            windowPosition = (windowPosition + 1) % windowSize;
        } else {
            const std::size_t length = symbol - 253;
            std::size_t positionCode = 0;
            if (!positionTree.decode(reader, positionCode)
            ||  positionCode >= positionSymbols
            ) {
                // FIX: this sometimes happens in scenarios where it shouldn't
                return codecFailure(
                    Error::InvalidInput,
                    "Invalid LH3 match position"
                );
            }

            std::uint32_t extra = 0;
            if (positionCode > 1 && !reader.read(static_cast<unsigned int>(positionCode - 1), extra)) {
                return codecFailure(
                    Error::InvalidInput,
                    "Truncated LH3 match distance"
                );
            }

            const std::size_t distance = positionCode == 0 ? 1 : (std::size_t{1} << (positionCode - 1)) + extra + 1;
            if (distance > windowSize
            || length > expectedSize - output.size()
            ) {
                return codecFailure(
                    Error::InvalidInput,
                    "LH3 match exceeds dictionary or expected output size"
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

    if (output.size() != expectedSize) {
        return codecFailure(
            Error::SizeMismatch,
            "LH3 output size does not match expected size"
        );
    }

    if (!reader.consumedAllBytes()) {
        return codecFailure(
            Error::InvalidInput,
            "Trailing bytes follow LH3 stream"
        );
    }

    Result result;
    result.data = std::move(output);
    return result;
}

} // namespace lha::detail