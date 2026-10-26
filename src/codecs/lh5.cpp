#include "codec_modules.h"

namespace lha::detail {

// LH5 codec
// 8 KiB sliding window and supports a maximum of 256 bytes of matching length

Result compressLh5(std::span<const Byte> input, const Options& options) {
    return compressLhCompressed(input, options, 8192, 14, 4);
}

Result decompressLh5(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    return decompressLhCompressed(input, expectedSize, options, 8192, 14, 4);
}

} // namespace lha::detail

