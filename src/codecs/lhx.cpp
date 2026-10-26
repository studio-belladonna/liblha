#include "codec_common.h"
#include "codec_modules.h"

// LHX codec
// uses a 128–256 KiB dictionary

namespace lha::detail {

namespace {
constexpr std::size_t windowSize = 1u << 19;
constexpr std::size_t positionSymbols = 20;
constexpr unsigned int positionCountBits = 5;
} // namespace

Result compressLhx(std::span<const Byte> input, const Options& options) {
    return compressLhCompressed(input, options, windowSize, positionSymbols, positionCountBits);
}

Result decompressLhx(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    return decompressLhCompressed(input, expectedSize, options, windowSize, positionSymbols, positionCountBits);
}
} // namespace lha::detail

