#include "codec_common.h"
#include "codec_modules.h"
#include "bit_io.h"

#include <algorithm>
#include <array>
#include <utility>

// PMarc implementations

// PM0:
// Noncompressed data storage

// PM1
// 8 KB sliding window with static huffman

// PM2
// Similar to LH5 with a 4 KiB sliding window

// thanks to Tsubusu05 for help

namespace lha::detail {

Result compressLh0(std::span<const Byte> input, const Options& options);
Result decompressLh0(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options);

namespace {

struct Range {
    unsigned int offset;
    unsigned int bits;
};

bool readRange(BitReader& reader, const Range& range, unsigned int& value) {
    std::uint32_t extra = 0;
    if (!reader.read(range.bits, extra)) {
        return false;
    }
    value = range.offset + extra;
    return true;
}

class ByteHistory {
public:
    ByteHistory() {
        for (unsigned int i = 0; i < 256; ++i) {
            previous_[i] = static_cast<Byte>(i + 1);
            next_[i] = static_cast<Byte>(i - 1);
        }

        head_ = 0x20;
        previous_[0x7f] = 0x00;
        next_[0x00] = 0x7f;
        previous_[0x1f] = 0xa0;
        next_[0xa0] = 0x1f;
        previous_[0xdf] = 0x80;
        next_[0x80] = 0xdf;
        previous_[0x9f] = 0xe0;
        next_[0xe0] = 0x9f;
        previous_[0xff] = 0x20;
        next_[0x20] = 0xff;
    }

    Byte at(unsigned int depth) const {
        Byte value = head_;
        if (depth < 128) {
            for (unsigned int i = 0; i < depth; ++i) value = previous_[value];
        } else {
            for (unsigned int i = 0; i < 256 - depth; ++i) value = next_[value];
        }

        return value;
    }

    unsigned int depthOf(Byte value) const {
        Byte current = head_;
        for (unsigned int depth = 0; depth < 128; ++depth) {
            if (current == value) return depth;
            current = previous_[current];
        }

        current = head_;
        for (unsigned int distance = 1; distance <= 128; ++distance) {
            current = next_[current];
            if (current == value) return 256 - distance;
        }

        return 0;
    }

    void touch(Byte value) {
        if (value == head_) return;
        previous_[next_[value]] = previous_[value];
        next_[previous_[value]] = next_[value];
        const Byte oldNext = next_[head_];
        next_[head_] = value;
        previous_[value] = head_;
        next_[value] = oldNext;
        previous_[oldNext] = value;
        head_ = value;
    }

private:
    std::array<Byte, 256> previous_{};
    std::array<Byte, 256> next_{};
    Byte head_ = 0;
};

class PmTree {
public:
    bool build(std::span<const Byte> lengths) {
        lengths_.assign(lengths.begin(), lengths.end());
        firstCode_.fill(0);
        firstSymbol_.fill(0);
        counts_.fill(0);
        symbols_.clear();

        for (Byte length : lengths_) {
            if (length > 15) return false;
            if (length != 0) ++counts_[length];
        }

        std::size_t total = 0;
        for (unsigned int length = 1; length <= 15; ++length) total += counts_[length];
        if (total == 0) return false;

        std::uint32_t code = 0;
        std::size_t index = 0;
        for (unsigned int length = 1; length <= 15; ++length) {
            code = (code + counts_[length - 1]) << 1;
            if (code + counts_[length] > (std::uint32_t{1} << length)) return false;
            firstCode_[length] = code;
            firstSymbol_[length] = index;
            index += counts_[length];
        }

        for (unsigned int length = 1; length <= 15; ++length) {
            for (std::size_t symbol = 0; symbol < lengths_.size(); ++symbol) {
                if (lengths_[symbol] == length) symbols_.push_back(static_cast<Byte>(symbol));
            }
        }

        return true;
    }

    bool setSingle(Byte symbol, std::size_t symbolCount) {
        std::vector<Byte> lengths(symbolCount, 0);
        if (symbol >= symbolCount) return false;
        lengths[symbol] = 1;
        return build(lengths);
    }

    bool decode(BitReader& reader, unsigned int& symbol) const {
        std::uint32_t code = 0;
        for (unsigned int length = 1; length <= 15; ++length) {
            std::uint32_t bit = 0;
            if (!reader.read(1, bit)) return false;
            code = (code << 1) | bit;

            if (code >= firstCode_[length] && code - firstCode_[length] < counts_[length]) {
                symbol = symbols_[firstSymbol_[length] + code - firstCode_[length]];
                return true;
            }
        }

        return false;
    }

private:
    std::vector<Byte> lengths_;
    std::array<std::uint32_t, 16> firstCode_{};
    std::array<std::size_t, 16> firstSymbol_{};
    std::array<std::size_t, 16> counts_{};
    std::vector<Byte> symbols_;
};

bool readPm2CodeTree(BitReader& reader, PmTree& tree, bool& needOffsetTree) {
    std::uint32_t codeCount = 0;
    std::uint32_t minimumLength = 0;

    if (!reader.read(5, codeCount) 
    ||  !reader.read(3, minimumLength) 
    ||  codeCount == 0 || codeCount > 29
    ) return false;

    needOffsetTree = codeCount >= 10 && !(codeCount == 29 && minimumLength == 0);
    if (minimumLength == 0) return tree.setSingle(static_cast<Byte>(codeCount - 1), 31);
    std::uint32_t lengthBits = 0;

    if (!reader.read(3, lengthBits) || lengthBits == 0) return false;
    std::array<Byte, 31> lengths{};

    for (std::size_t i = 0; i < codeCount; ++i) {
        std::uint32_t value = 0;
        if (!reader.read(lengthBits, value)) return false;

        lengths[i] = value == 0 ? 0 : static_cast<Byte>(minimumLength + value - 1);
    }

    return tree.build(std::span<const Byte>(lengths).first(codeCount));
}

bool readPm2OffsetTree(BitReader& reader, unsigned int numOffsets, bool needed, PmTree& tree) {
    if (!needed) return true;
    std::array<Byte, 8> lengths{};
    unsigned int nonzero = 0;
    unsigned int only = 0;

    for (unsigned int i = 0; i < numOffsets; ++i) {
        std::uint32_t value = 0;
        if (!reader.read(3, value)) return false;
        lengths[i] = static_cast<Byte>(value);
        if (value != 0) {
            ++nonzero;
            only = i;
        }
    }

    if (nonzero == 1) return tree.setSingle(static_cast<Byte>(only), numOffsets);
    return tree.build(std::span<const Byte>(lengths).first(numOffsets));
}

enum class Pm2TreeStage { Initial, After1K, After2K, After4K, Continuing };

struct Pm2Decoder {
    BitReader& reader;
    std::array<Byte, 8192> window{};
    std::size_t windowPosition = 0;
    ByteHistory history;
    PmTree codes;
    PmTree offsets;
    bool needOffsets = false;
    Pm2TreeStage stage = Pm2TreeStage::Initial;
    std::size_t rebuildRemaining = 0;

    bool initialize() {
        std::uint32_t ignored = 0;
        if (!reader.read(1, ignored)) return false;
        window.fill(' ');

        if (!readPm2CodeTree(reader, codes, needOffsets) 
        ||  !readPm2OffsetTree(reader, 5, needOffsets, offsets)
        ) return false;

        stage = Pm2TreeStage::After1K;
        rebuildRemaining = 1024;

        return true;
    }

    bool rebuild() {
        std::uint32_t bit = 0;
        switch (stage) {
            case Pm2TreeStage::Initial:
                return initialize();

            case Pm2TreeStage::After1K:
                if (!readPm2OffsetTree(reader, 6, needOffsets, offsets)) return false;
                stage = Pm2TreeStage::After2K;
                rebuildRemaining = 1024;
                return true;

            case Pm2TreeStage::After2K:
                if (!readPm2OffsetTree(reader, 7, needOffsets, offsets)) return false;
                stage = Pm2TreeStage::After4K;
                rebuildRemaining = 2048;
                return true;

            case Pm2TreeStage::After4K:
                if (!reader.read(1, bit)) return false;
                if (bit != 0 && !readPm2CodeTree(reader, codes, needOffsets)) return false;
                if (!readPm2OffsetTree(reader, 8, needOffsets, offsets)) return false;
                stage = Pm2TreeStage::Continuing;
                rebuildRemaining = 4096;
                return true;

            case Pm2TreeStage::Continuing:
                if (!reader.read(1, bit)) return false;
                if (bit != 0 && (!readPm2CodeTree(reader, codes, needOffsets) 
                ||  !readPm2OffsetTree(reader, 8, needOffsets, offsets))
                ) {
                    return false;
                }
                rebuildRemaining = 4096;
                return true;
        }

        return false;
    }

    bool output(Byte value, Bytes& output) {
        window[windowPosition] = value;
        windowPosition = (windowPosition + 1) % window.size();
        output.push_back(value);
        history.touch(value);
        if (--rebuildRemaining == 0) return rebuild();

        return true;
    }
};

constexpr std::array<Range, 6> byteRanges = {{
    {0, 4}, {16, 4}, {32, 5}, {64, 6}, {128, 6}, {192, 6}
}};

constexpr std::array<Range, 15> copyRanges = {{
    {0, 6},     {64, 8},    {0, 6},     {64, 9}, 
    {576, 11},  {2624, 13}, {64, 8},    {576, 8}, 
    {576, 9},   {576, 10},  {2624, 8},  {2624, 9}, 
    {2624, 10}, {2624, 11}, {2624, 12}
}};

constexpr std::array<Range, 8> pm2HistoryRanges = {{
    {0, 3}, {8, 3}, {16, 4}, {32, 5}, 
    {64, 5}, {96, 5}, {128, 6}, {192, 6}
}};

constexpr std::array<Range, 6> pm2CopyRanges = {{
    {17, 3}, {25, 3}, {33, 5}, {65, 6}, {129, 7}, {256, 0}
}};

// 大きい
constexpr std::array<std::array<Byte, 5>, 33> byteTrees = {{
    {{0x12,0x2d,0xef,0x1c,0xab}}, {{0x12,0x23,0xde,0xab,0xcf}}, {{0x12,0x2c,0xd2,0xab,0xef}}, {{0x12,0xa2,0xd2,0xbc,0xef}},
    {{0x12,0xa2,0xc2,0xbd,0xef}}, {{0x12,0xa2,0xcd,0xb1,0xef}}, {{0x12,0xab,0x12,0xcd,0xef}}, {{0x12,0xab,0x1d,0xc1,0xef}},
    {{0x12,0xab,0xc1,0xd1,0xef}}, {{0xa1,0x12,0x2c,0xde,0xbf}}, {{0xa1,0x1d,0x1c,0xb1,0xef}}, {{0xa1,0x12,0x2d,0xef,0xbc}},
    {{0xa1,0x12,0xb2,0xde,0xcf}}, {{0xa1,0x12,0xbc,0xd1,0xef}}, {{0xa1,0x1c,0xb1,0xd1,0xef}}, {{0xa1,0xb1,0x12,0xcd,0xef}},
    {{0xa1,0xb1,0xc1,0xd1,0xef}}, {{0x12,0x1c,0xde,0xab,0x00}}, {{0x12,0xa2,0xcd,0xbe,0x00}}, {{0x12,0xab,0xc1,0xde,0x00}},
    {{0xa1,0x1d,0x1c,0xbe,0x00}}, {{0xa1,0x12,0xbc,0xde,0x00}}, {{0xa1,0x1c,0xb1,0xde,0x00}}, {{0xa1,0xb1,0xc1,0xde,0x00}},
    {{0x1d,0x1c,0xab,0x00,0x00}}, {{0x1c,0xa1,0xbd,0x00,0x00}}, {{0x12,0xab,0xcd,0x00,0x00}}, {{0xa1,0x1c,0xbd,0x00,0x00}},
    {{0xa1,0xb1,0xcd,0x00,0x00}}, {{0xa1,0xbc,0x00,0x00,0x00}}, {{0xab,0x00,0x00,0x00,0x00}}, {{0x00,0x00,0x00,0x00,0x00}}
}};

bool readByteIndex(BitReader& reader, const std::array<Byte, 5>& tree, unsigned int& index) {
    if (tree[0] == 0) {
        index = 0;
        return true;
    }

    const Byte* node = tree.data();
    for (unsigned int depth = 0; depth < 16; ++depth) {
        std::uint32_t bit = 0;
        if (!reader.read(1, bit)) return false;
        const unsigned int child = bit == 0 ? node[0] >> 4 : node[0] & 0x0f;
        if (child >= 10) {
            index = child - 10;
            return index < byteRanges.size();
        }

        if (child == 0 || node + child >= tree.data() + tree.size()) {
            return false;
        }
        node += child;
    }

    return false;
}

bool writeByteIndex(BitWriter& writer, const std::array<Byte, 5>& tree, unsigned int wanted) {
    const auto search = [&](
        const auto& self, std::size_t nodeOffset, unsigned int depth, std::uint32_t code
    ) -> bool {
        if (depth >= 8 || nodeOffset >= tree.size()) return false;
        const Byte node = tree[nodeOffset];
        for (unsigned int bit = 0; bit < 2; ++bit) {
            const unsigned int child = bit == 0 ? node >> 4 : node & 0x0f;
            const std::uint32_t nextCode = (code << 1) | bit;
            if (child >= 10) {
                if (child - 10 == wanted) {
                    writer.write(depth + 1, nextCode);
                    return true;
                }

            } else if (child != 0 && self(self, nodeOffset + child, depth + 1, nextCode)) {
                return true;
            }
        }

        return false;
    };

    return search(search, 0, 0, 0);
}

bool writeRange(BitWriter& writer, std::span<const Range> ranges, unsigned int index, unsigned int value) {
    if (index >= ranges.size()) return false;
    const Range& range = ranges[index];
    const unsigned int count = 1u << range.bits;
    if (value < range.offset || value - range.offset >= count) return false;
    writer.write(range.bits, value - range.offset);
    return true;
}

bool writePm1Byte(BitWriter& writer, const std::array<Byte, 5>& tree, ByteHistory& history, Byte value) {
    const unsigned int depth = history.depthOf(value);
    if (history.at(depth) != value) return false;
    std::size_t rangeIndex = 0;

    //罪wwwww
    while (rangeIndex + 1 < byteRanges.size() && depth >= byteRanges[rangeIndex].offset + (1u << byteRanges[rangeIndex].bits)) {
        ++rangeIndex;
    }

    if (!writeByteIndex(writer, tree, static_cast<unsigned int>(rangeIndex))
    ||  !writeRange(writer, byteRanges, static_cast<unsigned int>(rangeIndex), depth)
    ) {
        return false;
    }

    history.touch(value);

    return true;
}

void writePm1ByteBlockCount(BitWriter & writer, unsigned int count) {
    if (count <= 3) writer.write(2, count - 1);
    else if (count <= 10) {
        writer.write(2, 3);
        writer.write(3, count - 4);
    } else if (count <= 24) {
        writer.write(2, 3);
        writer.write(3, 7);
        writer.write(4, count - 11);
    } else if (count <= 88) {
        writer.write(2, 3);
        writer.write(3, 7);
        writer.write(4, 14);
        writer.write(6, count - 25);
    } else {
        writer.write(2, 3);
        writer.write(3, 7);
        writer.write(4, 15);
        writer.write(7, count - 89);
    }
}

void writePm1FinalCopy(BitWriter & writer, std::size_t outputPosition) {
    writer.write(1, 0);
    if (outputPosition >= 576) writer.write(1, 0);
    if (outputPosition >= 64) writer.write(1, 0);
    writer.write(6, 0);
}

bool readPm1Byte(BitReader & reader,
    const std::array < Byte, 5 > & tree, ByteHistory & history, Byte & value) {
    unsigned int index = 0;
    unsigned int depth = 0;
    if (!readByteIndex(reader, tree, index) || !readRange(reader, byteRanges[index], depth)) return false;
    value = history.at(depth);
    return true;
}

bool readPm1ByteCount(BitReader & reader, unsigned int & count) {
    std::uint32_t value = 0;
    if (!reader.read(2, value)) return false;
    if (value < 3) {
        count = value + 1;
        return true;
    }
    if (!reader.read(3, value)) return false;
    if (value < 7) {
        count = value + 4;
        return true;
    }
    if (!reader.read(4, value)) return false;
    if (value < 14) {
        count = value + 11;
        return true;
    }
    if (value == 14) {
        if (!reader.read(6, value)) return false;
        count = value + 25;
        return true;
    }
    if (!reader.read(7, value)) return false;
    count = value + 89;

    return true;
}

bool readPm1CopyCount(BitReader& reader, unsigned int& count) {
    std::uint32_t value = 0;
    if (!reader.read(2, value)) return false;
    if (value < 3) { count = value + 3; return true; }
    if (!reader.read(3, value)) return false;
    if (value < 5) { count = value + 6; return true; }
    if (value == 5) {
        if (!reader.read(2, value)) return false;
        count = value + 11;
        return true;
    }
    if (value == 6) {
        if (!reader.read(3, value)) return false;
        count = value + 15;
        return true;
    }
    if (!reader.read(6, value)) return false;
    if (value < 62) { count = value + 23; return true; }
    if (value == 62) {
        if (!reader.read(5, value)) return false;
        count = value + 85;
        return true;
    }
    if (!reader.read(7, value)) return false;
    count = value + 117;

    return true;
}

bool readPm1Copy(
    BitReader& reader, 
    std::size_t outputPosition, 
    std::size_t expectedSize, 
    ByteHistory& history, 
    std::array<Byte, 16384>& window, 
    std::size_t& windowPosition, 
    Bytes& output
) {
    std::uint32_t bit = 0;
    if (!reader.read(1, bit)) return false;
    unsigned int range = 0;
    
    if (bit == 0) {
        if (outputPosition >= 576) {
            if (!reader.read(1, bit)) return false;
        } else {
            bit = 0;
        }

        if (bit != 0) {
            range = 4;
        } else {
            if (outputPosition >= 64) {
                if (!reader.read(1, bit)) return false;
            } else {
                bit = 0;
            }
            range = bit != 0 ? 1 : 0;
        }
    } else {
        if (outputPosition >= 64) {
            if (!reader.read(1, bit)) return false;
        } else {
            bit = 1;
        }

        if (bit == 0) {
            range = 3;
        } else {
            if (outputPosition >= 2624) {
                if (!reader.read(1, bit)) return false;
            } else {
                bit = 1;
            }
            range = bit != 0 ? 2 : 5;
        }
    }

    unsigned int count = 2;
    if (range >= 2 && !readPm1CopyCount(reader, count)) return false;
    if (range == 3 && outputPosition < 320) range = 6;
    if (range == 4) range = outputPosition < 832 ? 7 
    :   outputPosition < 1088 ? 8 
    :   outputPosition < 1600 ? 9 
    :   4;

    if (range == 5) range = outputPosition < 2880 ? 10 
    :   outputPosition < 3136 ? 11 
    :   outputPosition < 3648 ? 12 
    :   outputPosition < 4672 ? 13 
    :   outputPosition < 6720 ? 14 
    :   5;
    
    unsigned int distance = 0;

    if (!readRange(reader, copyRanges[range], distance) 
    || distance >= outputPosition 
    || count > 244 
    || count > expectedSize - output.size()
    ) return false;

    std::size_t source = (windowPosition + window.size() - distance - 1) % window.size();
    const unsigned int outputCount = static_cast<unsigned int>(
        std::min<std::size_t>(count, expectedSize - output.size())
    );

    for (unsigned int i = 0; i < outputCount; ++i) {
        const Byte value = window[source];
        output.push_back(value);
        window[windowPosition] = value;
        windowPosition = (windowPosition + 1) % window.size();

        source = (source + 1) % window.size();

        history.touch(value);
    }

    return true;
}

Result decodePm1(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    if (!outputSizeAllowed(expectedSize, options)) {
        return codecFailure(
            Error::OutputLimitExceeded,
            "Expected output exceeds configured size limit"
        );
    }

    if (expectedSize == 0) {
        return input.empty() ? Result{} : codecFailure(Error::InvalidInput, "Unexpected PM1 input for empty output");
    }
    
    BitReader reader(input);
    std::uint32_t treeIndex = 0;

    if (!reader.read(5, treeIndex) || treeIndex >= byteTrees.size()) {
        return codecFailure(
            Error::InvalidInput,
            "Invalid PM1 byte tree selector"
        );
    }
    
    const auto& tree = byteTrees[treeIndex];
    ByteHistory history;

    std::array<Byte, 16384> window{};
    std::size_t windowPosition = 0;

    Bytes output;
    output.reserve(static_cast<std::size_t>(expectedSize));
    while (output.size() < expectedSize) {
        std::uint32_t command = 0;
        if (!reader.read(1, command)) {
            return codecFailure(
                Error::InvalidInput,
                "Truncated PM1 command"
            );
        }
        
        if (command == 0) {
            const std::size_t before = output.size();
            if (!readPm1Copy(
                reader, before, 
                static_cast<std::size_t>(expectedSize), 
                history, window, windowPosition, output)
            ) return codecFailure(Error::InvalidInput, "Invalid PM1 copy command");

        } else {
            unsigned int count = 0;
            if (!readPm1ByteCount(reader, count) 
            ||  count > expectedSize - output.size()
            ) return codecFailure(Error::InvalidInput, "Invalid PM1 literal block");

            for (unsigned int i = 0; i < count; ++i) {
                Byte value = 0;
                if (!readPm1Byte(reader, tree, history, value)) {
                    return codecFailure(
                        Error::InvalidInput,
                        "Truncated PM1 literal block"
                    );
                }
                
                output.push_back(value);
                window[windowPosition] = value;
                windowPosition = (windowPosition + 1) % window.size();
                history.touch(value);
            }

            if (count < 216 && output.size() < expectedSize 
            &&  !readPm1Copy(
                reader, 
                output.size(), 
                static_cast<std::size_t>(expectedSize), 
                history, window, windowPosition, output
            )) return codecFailure(Error::InvalidInput, "Invalid PM1 block-following copy");
        }
    }

    Result result;
    result.data = std::move(output);
    return result;
}

Result decodePm2(std::span<const Byte> input, std::uint64_t expectedSize, const Options &options) {
    if (!outputSizeAllowed(expectedSize, options)) {
        return codecFailure(
            Error::OutputLimitExceeded,
            "Expected output exceeds configured size limit"
        );
    }

    if (expectedSize == 0) {
        return input.empty() ? Result{} : codecFailure(
            Error::InvalidInput,
            "Unexpected PM2 input for empty output"
        );
    }

    BitReader reader(input);
    Pm2Decoder decoder{reader};
    if (!decoder.initialize()) {
        return codecFailure(
            Error::InvalidInput,
            "Invalid PM2 initial Huffman tables"
        );
    }

    Bytes output;
    output.reserve(static_cast<std::size_t>(expectedSize));
    while (output.size() < expectedSize) {
        unsigned int code = 0;
        if (!decoder.codes.decode(reader, code)) {
            return codecFailure(
                Error::InvalidInput,
                "Truncated PM2 command"
            );
        }

        if (code < 8) {
            unsigned int historyIndex = 0;
            if (!readRange(reader, pm2HistoryRanges[code], historyIndex)) {
                return codecFailure(
                    Error::InvalidInput,
                    "Truncated PM2 history byte"
                );
            }

            if (!decoder.output(decoder.history.at(historyIndex), output)) {
                return codecFailure(
                    Error::InvalidInput,
                    "Invalid PM2 tree rebuild"
                );
            }
        }
        else {
            const unsigned int copyCode = code - 8;
            unsigned int count = 0;

            if (copyCode < 15) {
                count = copyCode + 2;
            } else if (copyCode - 15 < pm2CopyRanges.size() 
                   && readRange(reader, pm2CopyRanges[copyCode - 15], count)
            ) {
                // prop it
            } else {
                return codecFailure(
                    Error::InvalidInput,
                    "Invalid PM2 copy length"
                );
            }

            unsigned int offset = 0;
            if (copyCode == 0) {
                std::uint32_t value = 0;
                if (!reader.read(6, value)) {
                    return codecFailure(
                        Error::InvalidInput,
                        "Truncated PM2 copy offset"
                    );
                }

                offset = value;
            } else if (copyCode < 20) {
                unsigned int offsetCode = 0;
                if (!decoder.offsets.decode(reader, offsetCode)) {
                    return codecFailure(
                        Error::InvalidInput,
                        "Invalid PM2 copy offset code"
                    );
                }

                const unsigned int bits = offsetCode == 0 ? 6 : offsetCode + 5;
                std::uint32_t value = 0;
                if (!reader.read(bits, value)) {
                    return codecFailure(
                        Error::InvalidInput,
                        "Truncated PM2 copy offset"
                    );
                }

                offset = offsetCode == 0 ? value : (1u << bits) + value;
            }

            if (count == 0 || count > 256 || count > expectedSize - output.size()) {
                return codecFailure(
                    Error::SizeMismatch,
                    "Invalid PM2 copy length"
                );
            }
            
            // 修正ガ必要
            std::size_t source = (decoder.windowPosition + decoder.window.size() - 1 - offset) % decoder.window.size();
            for (unsigned int i = 0; i < count; ++i) {
                const Byte value = decoder.window[source];
                source = (source + 1) % decoder.window.size();
                if (!decoder.output(value, output)) {
                    return codecFailure(
                        Error::InvalidInput,
                        "Invalid PM2 tree rebuild"
                    );
                }
            }
        }
    }

    Result result;
    result.data = std::move(output);
    return result;
}

} // namespace

Result compressPm0(std::span<const Byte> input, const Options& options) {
    return compressLh0(input, options);
}

Result decompressPm0(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    return decompressLh0(input, expectedSize, options);
}

Result compressPm1(std::span<const Byte> input, const Options &options) {
    if (!outputSizeAllowed(input.size(), options))
        return codecFailure(
            Error::OutputLimitExceeded,
            "Input exceeds configured size limit"
        );

    if (input.empty())
        return {};

    constexpr std::size_t maxLiteralBlock = 216;
    constexpr std::array<Byte, 5> tree = {{0x12, 0x2d, 0xef, 0x1c, 0xab}};

    BitWriter writer;
    writer.write(5, 0);
    ByteHistory history;

    std::size_t position = 0;
    while (position < input.size()) {
        const unsigned int count = static_cast<unsigned int>(std::min(maxLiteralBlock, input.size() - position));
        writer.write(1, 1);
        writePm1ByteBlockCount(writer, count);
        for (unsigned int i = 0; i < count; ++i) {
            if (!writePm1Byte(writer, tree, history, input[position++])) {
                return codecFailure(
                    Error::InvalidInput,
                    "Unable to encode PM1 literal"
                );
            }
        }
    }

    if (input.size() % maxLiteralBlock != 0) {
        writePm1FinalCopy(writer, input.size());
    }

    Result result;
    result.data = writer.finish();
    return result;
}

Result decompressPm1(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    return decodePm1(input, expectedSize, options);
}

Result compressPm2(std::span<const Byte> input, const Options &options) {
    if (!outputSizeAllowed(input.size(), options))
        return codecFailure(
            Error::OutputLimitExceeded,
            "Input exceeds configured size limit"
        );

    if (input.empty())
        return {};

    BitWriter writer;
    writer.write(1, 0);
    writer.write(5, 8);
    writer.write(3, 3);
    writer.write(3, 1);
    for (unsigned int i = 0; i < 8; ++i)
        writer.write(1, 1);

    ByteHistory history;
    for (std::size_t position = 0; position < input.size(); ++position) {
        const Byte value = input[position];
        const unsigned int depth = history.depthOf(value);

        std::size_t code = 0;
        while (code + 1 < pm2HistoryRanges.size() 
        &&  depth >= pm2HistoryRanges[code].offset + (1u << pm2HistoryRanges[code].bits)
        ) ++code;

        writer.write(3, static_cast<std::uint32_t>(code));
        if (!writeRange(writer, pm2HistoryRanges, static_cast<unsigned int>(code), depth))
            return codecFailure(
                Error::InvalidInput,
                "Unable to encode PM2 history byte"
            );

        history.touch(value);
        if ((position + 1) % 4096 == 0)
            writer.write(1, 0);
    }

    Result result;
    result.data = writer.finish();
    return result;
}

Result decompressPm2(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    return decodePm2(input, expectedSize, options);
}

} // namespace lha::detail