#include "codec_common.h"
#include "codec_modules.h"

// LK7 codec

namespace lha::detail {

namespace {
constexpr std::size_t windowSize = 1u << 16;
constexpr std::size_t positionSymbols = (1u << 6) - 1;
constexpr unsigned int positionCountBits = 6;
} // namespace

Result compressLk7(std::span<const Byte> input, const Options& options) {
    return compressLhCompressed(input, options, windowSize, positionSymbols, positionCountBits, true);
}

Result decompressLk7(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    return decompressLhCompressed(input, expectedSize, options, windowSize, positionSymbols, positionCountBits, true);
}

} // namespace lha::detail