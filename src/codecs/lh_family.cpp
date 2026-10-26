#include "codec_common.h"
#include "codec_modules.h"
#include "bit_io.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <queue>
#include <utility>

// common compression implementations for LH

namespace lha {

using detail::codecFailure;
using detail::BitReader;
using detail::BitWriter;

namespace {

constexpr std::size_t lh5WindowSize = 8192;
constexpr std::size_t lh5MaxMatch = 256;
constexpr std::size_t lh5MinMatch = 3;
constexpr std::size_t lh5MaxBlockTokens = 65535;
constexpr std::size_t lh5CharacterSymbols = 510;
constexpr std::size_t lharkCharacterSymbols = 289;
constexpr std::size_t lh5CodeLengthSymbols = 19;
constexpr std::size_t lharkMaxMatch = 514;

struct HuffmanTree {
    std::vector<std::uint8_t> lengths;
    std::vector<std::uint32_t> firstCode;
    std::vector<std::size_t> firstSymbol;
    std::vector<std::size_t> count;
    std::vector<std::size_t> orderedSymbols;
    std::vector<std::uint32_t> codes;

    int constantSymbol = -1;

    bool build(std::span<const std::uint8_t> codeLengths, unsigned int maxBits, bool constantIfSingle = false) {
        lengths.assign(codeLengths.begin(), codeLengths.end());
        firstCode.assign(maxBits + 1, 0);
        firstSymbol.assign(maxBits + 1, 0);

        count.assign(maxBits + 1, 0);
        orderedSymbols.clear();

        codes.assign(lengths.size(), 0);
        constantSymbol = -1;

        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            if (lengths[symbol] > maxBits) {
                return false;
            }

            if (lengths[symbol] != 0) {
                ++count[lengths[symbol]];
            }
        }

        std::size_t symbols = 0;
        for (unsigned int bits = 1; bits <= maxBits; ++bits) {
            symbols += count[bits];
        }

        if (symbols == 0) return false;
        
        if (symbols == 1 && constantIfSingle) {
            for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
                if (lengths[symbol] != 0) {
                    constantSymbol = static_cast<int>(symbol);

                    return true;
                }
            }
        }

        std::uint32_t code = 0;
        std::size_t offset = 0;
        for (unsigned int bits = 1; bits <= maxBits; ++bits) {
            code = (code + static_cast<std::uint32_t>(count[bits - 1])) << 1;
            if (code + count[bits] > (std::uint32_t{1} << bits)) {
                return false;
            }

            firstCode[bits] = code;
            firstSymbol[bits] = offset;
            offset += count[bits];
        }

        std::vector<std::size_t> nextRank(maxBits + 1, 0);
        for (unsigned int bits = 1; bits <= maxBits; ++bits) {
            for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
                if (lengths[symbol] == bits) {
                    orderedSymbols.push_back(symbol);
                    codes[symbol] = firstCode[bits] + static_cast<std::uint32_t>(nextRank[bits]++);
                }
            }
        }

        return true;
    }

    bool decode(BitReader& reader, std::size_t& symbol) const {
        if (constantSymbol >= 0) {
            symbol = static_cast<std::size_t>(constantSymbol);

            return true;
        }

        std::uint32_t code = 0;
        for (unsigned int bits = 1; bits < count.size(); ++bits) {
            std::uint32_t bit = 0;

            if (!reader.read(1, bit)) {

                return false;
            }

            code = (code << 1) | bit;
            if (code >= firstCode[bits] && code - firstCode[bits] < count[bits]) {
                symbol = orderedSymbols[firstSymbol[bits] + code - firstCode[bits]];

                return true;
            }
        }

        return false;
    }

    bool encode(BitWriter& writer, std::size_t symbol) const {
        if (symbol >= lengths.size()) {
            return false;
        }

        if (constantSymbol >= 0) {
            return symbol == static_cast<std::size_t>(constantSymbol);
        }

        const unsigned int bits = lengths[symbol];
        if (bits == 0) {
            return false;
        }
        writer.write(bits, codes[symbol]);
        
        return true;
    }
};

bool readTreeLengths(
    BitReader& reader, 
    std::size_t symbolCount, 
    unsigned int countBits, 
    int specialIndex, 
    std::vector<std::uint8_t>& lengths, 
    bool* isConstant = nullptr
) {
    std::uint32_t count = 0;
    if (!reader.read(countBits, count) || count > symbolCount) {
        return false;
    }

    lengths.assign(symbolCount, 0);
    if (isConstant != nullptr) {
        *isConstant = count == 0;
    }
    
    if (count == 0) {
        std::uint32_t symbol = 0;
        if (!reader.read(countBits, symbol) || symbol >= symbolCount) {
            return false;
        }
        lengths[symbol] = 1;
        return true;
    }

    std::size_t index = 0;
    while (index < count) {
        if (index >= symbolCount) {
            return false;
        }

        std::uint32_t length = 0;
        if (!reader.read(3, length)) {
            return false;
        }

        if (length == 7) {
            std::uint32_t continuation = 0;
            do {
                if (!reader.read(1, continuation)) {
                    return false;
                }
                if (continuation != 0) {
                    ++length;
                }
            } while (continuation != 0 && length <= 16);
        }

        if (length > 16) {
            return false;
        }

        lengths[index++] = static_cast<std::uint8_t>(length);
        if (specialIndex >= 0 && index == static_cast<std::size_t>(specialIndex + 1)) {
            std::uint32_t zeros = 0;
            if (!reader.read(2, zeros) || zeros > count - index) {
                return false;
            }

            index += zeros;
        }
    }

    return true;
}

struct Lh5Token {
    std::uint16_t symbol;
    std::uint32_t position;
    std::uint16_t length = 0;
};

struct HuffmanNode {
    std::uint64_t frequency;
    int left;
    int right;
    std::size_t symbol;
};

std::vector<std::uint8_t> makeCodeLengths(const std::vector<std::uint64_t>& frequencies, unsigned int maxBits) {
    // TODO: reimplement STAMs

    struct QueueEntry {
        std::uint64_t frequency;
        std::size_t order;
        int node;
    };

    struct Greater {
        bool operator()(const QueueEntry& left, const QueueEntry& right) const {
            if (left.frequency != right.frequency) {
                return left.frequency > right.frequency;
            }

            return left.order > right.order;
        }
    };

    std::vector<HuffmanNode> nodes;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, Greater> queue;
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
        lengths[nodes[queue.top().node].symbol] = 1;
        return lengths;
    }

    std::size_t order = frequencies.size();
    while (queue.size() > 1) {
        const QueueEntry left = queue.top();
        queue.pop();

        const QueueEntry right = queue.top();
        queue.pop();

        nodes.push_back({left.frequency + right.frequency, left.node, right.node, 0});
        queue.push({
            left.frequency + right.frequency, 
            order++, 
            static_cast<int>(nodes.size() - 1)
        });
    }

    std::vector<std::pair<int, unsigned int>> stack = {{queue.top().node, 0}};
    std::array<std::size_t, 17> bitCounts = {};
    while (!stack.empty()) {
        const auto [nodeIndex, depth] = stack.back();
        stack.pop_back();

        const HuffmanNode& node = nodes[static_cast<std::size_t>(nodeIndex)];
        if (node.left < 0) {
            const unsigned int bits = std::max(1u, depth);
            lengths[node.symbol] = static_cast<std::uint8_t>(std::min(bits, maxBits));
            ++bitCounts[std::min(bits, maxBits)];

        } else {
            stack.push_back({node.left, depth + 1});
            stack.push_back({node.right, depth + 1});

            // stack.push_back({node.left, coda_item.dval}) // STAM implementation
            // stack.push_back({node.right, coda_item.dval})
        }
    }

    std::size_t overflow = 0;
    std::vector<unsigned int> depths(frequencies.size(), 0);
    stack = {{queue.top().node, 0}};
    while (!stack.empty()) {
        const auto [nodeIndex, depth] = stack.back();
        stack.pop_back();
        const HuffmanNode& node = nodes[static_cast<std::size_t>(nodeIndex)];

        if (node.left < 0) {
            depths[node.symbol] = depth;

            if (depth > maxBits) {
                ++overflow;
            }
            
        } else {
            stack.push_back({node.left, depth + 1});
            stack.push_back({node.right, depth + 1});

            // stack.push_back({node.left, coda_item.dval}) // STAM implementation
            // stack.push_back({node.right, coda_item.dval})
        }

    }

    if (overflow != 0) {
        bitCounts.fill(0);

        for (unsigned int depth : depths) {
            if (depth > maxBits) {
                ++bitCounts[maxBits];
            } else if (depth != 0) {
                ++bitCounts[depth];
            }
        }

        while (overflow > 0) {
            int bits = static_cast<int>(maxBits) - 1;
            while (bits > 0 && bitCounts[static_cast<std::size_t>(bits)] == 0) {
                --bits;
            }

            if (bits == 0 || bitCounts[maxBits] == 0) {
                return {};
            }

            --bitCounts[static_cast<std::size_t>(bits)];
            bitCounts[static_cast<std::size_t>(bits + 1)] += 2;
            --bitCounts[maxBits];
            overflow = overflow > 1 ? overflow - 2 : 0;
        }

        std::vector<std::size_t> symbols;
        for (std::size_t symbol = 0; symbol < frequencies.size(); ++symbol) {
            if (frequencies[symbol] != 0) {
                symbols.push_back(symbol);
            }
        }

        std::sort(symbols.begin(), symbols.end(), [&](std::size_t left, std::size_t right) {
            if (frequencies[left] != frequencies[right]) {
                return frequencies[left] < frequencies[right];
            }
            return left < right;
        });

        std::fill(lengths.begin(), lengths.end(), 0);
        std::size_t symbolIndex = 0;
        for (int bits = static_cast<int>(maxBits); bits >= 1; --bits) {
            for (std::size_t i = 0; i < bitCounts[static_cast<std::size_t>(bits)]; ++i) {
                if (symbolIndex >= symbols.size()) {
                    return {};
                }

                lengths[symbols[symbolIndex++]] = static_cast<std::uint8_t>(bits);
            }
        }

        if (symbolIndex != symbols.size()) {
            return {};
        }
    }
    return lengths;
}

void writeFixedPtTree(BitWriter& writer) {
    writer.write(5, lh5CodeLengthSymbols);

    for (std::size_t i = 0; i < lh5CodeLengthSymbols; ++i) {
        writer.write(3, 5);

        if (i == 3) {
            writer.write(2, 0);
        }
    }
}

bool makeFixedPtTree(HuffmanTree& tree) {
    std::vector<std::uint8_t> lengths(lh5CodeLengthSymbols, 5);
    return tree.build(lengths, 16);
}

bool writeCharacterTree(BitWriter& writer, const std::vector<std::uint8_t>& lengths, const HuffmanTree& ptTree) {
    const auto first = std::find_if(
        lengths.begin(), lengths.end(), 
        [](std::uint8_t length) { 
            return length != 0; 
        }
    );

    if (first == lengths.end()) 
        return false;

    writeFixedPtTree(writer);
    const std::size_t nonzeroCount = static_cast<std::size_t>(
        std::count_if(
            lengths.begin(), lengths.end(), 
            [](std::uint8_t length) { 
                return length != 0; 
            }
        )
    );

    if (nonzeroCount == 1) {
        writer.write(9, 0);
        writer.write(9, static_cast<std::uint32_t>(std::distance(lengths.begin(), first)));
        return true;
    }

    writer.write(9, static_cast<std::uint32_t>(lengths.size()));
    std::size_t index = 0;
    while (index < lengths.size()) {
        if (lengths[index] != 0) {
            const std::size_t value = lengths[index] + 2;
            if (!ptTree.encode(writer, value)) {
                return false;
            }

            ++index;
            continue;
        }

        std::size_t run = 1;
        while (index + run < lengths.size() && lengths[index + run] == 0) {
            ++run;
        }

        while (run >= 20) {
            const std::size_t chunk = std::min<std::size_t>(run, 531);
            if (!ptTree.encode(writer, 2)) {
                return false;
            }

            writer.write(9, static_cast<std::uint32_t>(chunk - 20));
            index += chunk;
            run -= chunk;
        }

        if (run >= 3) {
            const std::size_t chunk = std::min<std::size_t>(run, 18);
            if (!ptTree.encode(writer, 1)) {
                return false;
            }

            writer.write(4, static_cast<std::uint32_t>(chunk - 3));
            index += chunk;
            run -= chunk;
        }

        while (run != 0) {
            if (!ptTree.encode(writer, 0)) {
                return false;
            }

            ++index;
            --run;
        }
    }

    return true;
}

std::uint16_t lh5Hash(std::span<const Byte> input, std::size_t index) {
    return  static_cast<std::uint16_t>((
            static_cast<std::uint32_t>(input[index]) 
        *   251u 
        +   static_cast<std::uint32_t>(input[index + 1])
        )
        *   251u 
        +   static_cast<std::uint32_t>(input[index + 2])
    );
}

Result compressLhFamilyImpl(
    std::span<const Byte> input,
    const Options& options,
    std::size_t windowSize,
    std::size_t positionSymbols,
    unsigned int positionCountBits,
    bool lhark
) {
    if (!detail::outputSizeAllowed(input.size(), options)) {
        return codecFailure(Error::OutputLimitExceeded, "Input exceeds configured size limit");
    }

    Result result;
    if (input.empty()) {
        return result;
    }

    const std::size_t missing = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> heads(65536, missing);
    std::vector<std::size_t> previous(input.size(), missing);
    std::size_t inputPosition = 0;
    HuffmanTree ptTree;
    if (!makeFixedPtTree(ptTree)) {
        return codecFailure(Error::InvalidInput, "Unable to create LH code-length tree");
    }

    BitWriter writer;
    const std::size_t characterSymbols = lhark ? lharkCharacterSymbols : lh5CharacterSymbols;
    const std::size_t maxMatch = lhark ? lharkMaxMatch : lh5MaxMatch;

    // TODO: reduce this insane nesting...
    while (inputPosition < input.size()) {
        std::vector<Lh5Token> tokens;
        tokens.reserve(std::min(lh5MaxBlockTokens, input.size() - inputPosition));
        std::vector<std::uint64_t> characterFrequencies(characterSymbols, 0);

        while (inputPosition < input.size() && tokens.size() < lh5MaxBlockTokens) {
            std::size_t bestLength = 0;
            std::size_t bestDistance = 0;

            if (inputPosition + lh5MinMatch <= input.size()) {
                const std::uint16_t hash = lh5Hash(input, inputPosition);
                std::size_t candidate = heads[hash];
                unsigned int chain = 0;

                while (candidate != missing && inputPosition - candidate <= windowSize && chain++ < 64) {
                    std::size_t length = 0;
                    while   (length < maxMatch 
                        &&  inputPosition + length < input.size() 
                        &&  input[candidate + length] == input[inputPosition + length]
                    ) {
                        ++length;
                    }

                    if (length > bestLength && length >= lh5MinMatch) {
                        bestLength = length;
                        bestDistance = inputPosition - candidate;

                        if (length == maxMatch) {
                            break;
                        }
                    }

                    candidate = previous[candidate];
                }
            }

            const std::size_t consumed = bestLength >= lh5MinMatch ? bestLength : 1;
            if (consumed == 1) {
                const std::size_t symbol = input[inputPosition];
                tokens.push_back({static_cast<std::uint16_t>(symbol), 0});
                ++characterFrequencies[symbol];
            } else {
                std::size_t symbol = consumed + 253;
                if (lhark) {
                    if (consumed <= 10) {
                        symbol = 253 + consumed;

                    } else if (consumed == lharkMaxMatch) {
                        symbol = 288;

                    } else {
                        const std::size_t adjustedLength = consumed - lh5MinMatch;
                        const unsigned int lowBits = std::bit_width(adjustedLength) - 3;
                        const std::size_t group = (adjustedLength >> lowBits) - 4;

                        symbol = 260 + 4 * lowBits + group;
                    }
                }

                const std::size_t position = bestDistance - 1;
                tokens.push_back({static_cast<std::uint16_t>(symbol), static_cast<std::uint32_t>(position), static_cast<std::uint16_t>(consumed)});
                ++characterFrequencies[symbol];
            }

            for (std::size_t offset = 0; offset < consumed; ++offset) {
                const std::size_t position = inputPosition + offset;

                if (position + 2 < input.size()) {
                    const std::uint16_t hash = lh5Hash(input, position);

                    previous[position] = heads[hash];
                    heads[hash] = position;
                }
            }

            inputPosition += consumed;
        }

        writer.write(16, static_cast<std::uint32_t>(tokens.size()));
        std::vector<std::uint8_t> characterLengths = makeCodeLengths(characterFrequencies, 16);
        if (characterLengths.empty()) {
            return codecFailure(Error::InvalidInput, "Unable to construct LH character Huffman tree");
        }

        if (!writeCharacterTree(writer, characterLengths, ptTree)) {
            return codecFailure(Error::InvalidInput, "Unable to serialize LH character Huffman tree");
        }

        writer.write(positionCountBits, static_cast<std::uint32_t>(positionSymbols));
        for (std::size_t i = 0; i < positionSymbols; ++i) {
            writer.write(3, lhark ? 6 : 5);
        }

        HuffmanTree characterTree;
        if (!characterTree.build(
            characterLengths, 16,
            std::count_if(
                characterLengths.begin(),
                characterLengths.end(),
                [](std::uint8_t length) {
                    return length != 0;
                }
            ) == 1
        )) {
            return codecFailure(Error::InvalidInput, "Unable to construct LH character encoder");
        }

        for (const Lh5Token& token : tokens) {
            if (!characterTree.encode(writer, token.symbol)) {
                return codecFailure(Error::InvalidInput, "Unable to encode LH character token");
            }

            if (token.symbol >= 256) {
                if (lhark && token.symbol >= 264 && token.symbol < 288) {
                    const unsigned int lengthBits = static_cast<unsigned int>((token.symbol - 260) / 4);
                    const std::size_t base = (4 + token.symbol % 4) << lengthBits;

                    writer.write(lengthBits, static_cast<std::uint32_t>(token.length - lh5MinMatch - base));
                }

                const std::size_t position = token.position;
                if (lhark) {
                    std::size_t offsetCode = position;
                    std::size_t extra = 0;

                    unsigned int extraBits = 0;

                    if (position >= 4) {
                        extraBits = std::bit_width(position) - 2;
                        const std::size_t base = std::size_t{1} << extraBits;
                        const std::size_t group = position >= 3 * base ? 1 : 0;
                        offsetCode = 2 + 2 * extraBits + group;
                        extra = position - (2 + group) * base;
                    }

                    writer.write(positionCountBits, static_cast<std::uint32_t>(offsetCode));
                    if (extraBits != 0) {
                        writer.write(extraBits, static_cast<std::uint32_t>(extra));
                    }

                } else {
                    const unsigned int positionCode = position == 0 ? 0 : std::bit_width(position);

                    writer.write(5, static_cast<std::uint32_t>(positionCode));
                    if (positionCode != 0) {
                        writer.write(
                            positionCode - 1, 
                            static_cast<std::uint32_t>(
                                position - (std::size_t{1} << (positionCode - 1))
                            )
                        );
                    }

                }
            }
        }
    }

    result.data = writer.finish();
    return result;
}

Result decompressLhFamilyImpl(
    std::span<const Byte> input, 
    std::uint64_t expectedSize, 
    const Options& options, 
    std::size_t windowSize, 
    std::size_t positionSymbols, 
    unsigned int positionCountBits, 
    bool lhark
) {
    if (!detail::outputSizeAllowed(expectedSize, options)) {
        return codecFailure(Error::OutputLimitExceeded, "Expected output exceeds configured or platform size limit");
    }
    
    if (input.empty()) {
        if (expectedSize == 0) {
            return {};
        }
        return codecFailure(Error::SizeMismatch, "Compressed input is empty");
    }

    BitReader reader(input);

    Bytes output;
    output.reserve(static_cast<std::size_t>(expectedSize));

    Bytes window(windowSize, 0x20);
    std::size_t windowPosition = lhark ? 0 : windowSize - lh5MaxMatch;

    const std::size_t characterSymbols = lhark ? lharkCharacterSymbols : lh5CharacterSymbols;

    while (output.size() < expectedSize) {
        std::uint32_t blockTokens = 0;
        if (!reader.read(16, blockTokens) || blockTokens == 0) {
            return codecFailure(Error::InvalidInput, "Invalid or truncated LH block header");
        }

        std::vector<std::uint8_t> ptLengths;
        bool ptConstant = false;
        if (!readTreeLengths(reader, lh5CodeLengthSymbols, 5, 3, ptLengths, &ptConstant)) {
            return codecFailure(Error::InvalidInput, "Invalid or truncated LH code-length tree");
        }

        HuffmanTree ptTree;
        if (!ptTree.build(ptLengths, 16, ptConstant)) {
            return codecFailure(Error::InvalidInput, "Invalid LH code-length Huffman tree");
        }

        std::vector<std::uint8_t> characterLengths(characterSymbols, 0);
        std::uint32_t characterCount = 0;
        if (!reader.read(9, characterCount) || characterCount > characterSymbols) {
            return codecFailure(Error::InvalidInput, "Invalid LH character-tree header");
        }

        if (characterCount == 0) {
            std::uint32_t symbol = 0;
            if (!reader.read(9, symbol) || symbol >= characterSymbols) {
                return codecFailure(Error::InvalidInput, "Invalid constant LH character symbol");
            }

            characterLengths[symbol] = 1;
        }

        const bool characterConstant = characterCount == 0;
        if (characterCount != 0) {
            std::size_t index = 0;
            while (index < characterCount) {
                std::size_t value = 0;
                if (!ptTree.decode(reader, value)) {
                    return codecFailure(Error::InvalidInput, "Truncated LH character-tree data");
                }

                if (value == 0) {
                    ++index;
                } else if (value == 1 || value == 2) {
                    std::uint32_t run = 0;
                    const unsigned int extraBits = value == 1 ? 4 : 9;
                    if (!reader.read(extraBits, run)) {
                        return codecFailure(Error::InvalidInput, "Truncated LH zero-run code");
                    }

                    run += value == 1 ? 3 : 20;
                    if (run > characterCount - index) {
                        return codecFailure(Error::InvalidInput, "LH zero run exceeds character tree");
                    }

                    index += run;
                } else if (value < 19) {
                    characterLengths[index++] = static_cast<std::uint8_t>(value - 2);
                } else {
                    return codecFailure(Error::InvalidInput, "Invalid LH character code length");
                }
            }
        }

        HuffmanTree characterTree;
        if (!characterTree.build(characterLengths, 16, characterConstant)) {
            return codecFailure(Error::InvalidInput, "Invalid LH character Huffman tree");
        }

        std::vector<std::uint8_t> positionLengths;
        bool positionConstant = false;
        if (!readTreeLengths(reader, positionSymbols, positionCountBits, -1, positionLengths, &positionConstant)) {
            return codecFailure(Error::InvalidInput, "Invalid or truncated LH position tree");
        }

        HuffmanTree positionTree;
        if (!positionTree.build(positionLengths, 16, positionConstant)) {
            return codecFailure(Error::InvalidInput, "Invalid LH position Huffman tree");
        }

        for (std::uint32_t token = 0; token < blockTokens; ++token) {
            std::size_t symbol = 0;
            if (!characterTree.decode(reader, symbol)) {
                return codecFailure(Error::InvalidInput, "Truncated LH token stream");
            }

            if (symbol < 256) {
                if (output.size() >= expectedSize) {
                    return codecFailure(Error::SizeMismatch, "LH stream expands beyond expected output size");
                }

                const Byte value = static_cast<Byte>(symbol);
                output.push_back(value);
                window[windowPosition] = value;
                windowPosition = (windowPosition + 1) % windowSize;
            } else {
                std::uint32_t extra = 0;
                std::size_t length = 0;

                if (lhark) {
                    if (symbol <= 263) {
                        length = symbol - 253;
                    } else if (symbol < 288) {
                        const unsigned int extraBits = static_cast<unsigned int>((symbol - 260) / 4);
                        if (!reader.read(extraBits, extra)) {
                            return codecFailure(Error::InvalidInput, "Truncated LHARK match length");
                        }

                        length = ((4 + symbol % 4) << extraBits) + extra + lh5MinMatch;
                    } else {
                        length = lharkMaxMatch;
                    }
                } else {
                    length = symbol - 253;
                }

                std::size_t positionCode = 0;
                if (!positionTree.decode(reader, positionCode) || positionCode >= positionSymbols) {
                    return codecFailure(Error::InvalidInput, "Invalid LH match position");
                }

                extra = 0;
                std::size_t distance = 0;

                if (lhark) {
                    const unsigned int extraBits = positionCode < 4 ? 0 : static_cast<unsigned int>((positionCode - 2) / 2);
                    if (extraBits != 0 && !reader.read(extraBits, extra)) {
                        return codecFailure(Error::InvalidInput, "Truncated LHARK match distance");
                    }

                    distance = positionCode < 4
                    ?   positionCode + 1
                    :   ((2 + positionCode % 2) << extraBits) + extra + 1;
                } else {
                    if (positionCode > 1 
                    && !reader.read(static_cast<unsigned int>(positionCode - 1), extra)
                    ) {
                        return codecFailure(Error::InvalidInput, "Truncated LH match distance");
                    }
                    distance = positionCode == 0 ? 1 : (std::size_t{1} << (positionCode - 1)) + extra + 1;
                }

                if (distance > windowSize || length > expectedSize - output.size()) {
                    return codecFailure(Error::InvalidInput, "LH match exceeds the dictionary or expected output size");
                }

                for (std::size_t i = 0; i < length; ++i) {
                    const Byte value = window[(windowPosition + windowSize - distance) % windowSize];
                    output.push_back(value);
                    window[windowPosition] = value;
                    windowPosition = (windowPosition + 1) % windowSize;
                }
            }
        }
    }

    if (output.size() != expectedSize) {
        return codecFailure(Error::SizeMismatch, "LH output size does not match expected size");
    }

    if (!reader.consumedAllBytes()) {
        return codecFailure(Error::InvalidInput, "Trailing bytes follow the LH stream");
    }

    Result result;
    result.data = std::move(output);

    return result;
}

} // namespace

namespace detail {

Result compressLhCompressed(
    std::span<const Byte> input,
    const Options& options,
    std::size_t windowSize,
    std::size_t positionSymbols,
    unsigned int positionCountBits,
    bool lhark
) {
    return compressLhFamilyImpl(input, options, windowSize, positionSymbols, positionCountBits, lhark);
}

Result decompressLhCompressed(
    std::span<const Byte> input,
    std::uint64_t expectedSize,
    const Options& options,
    std::size_t windowSize,
    std::size_t positionSymbols,
    unsigned int positionCountBits,
    bool lhark
) {
    return decompressLhFamilyImpl(input, expectedSize, options, windowSize, positionSymbols, positionCountBits, lhark);
}

} // namepsace detail
} // namespace lha