#include "codec_common.h"
#include "codec_modules.h"

// LH4 codec
// 4 KiB sliding window and supports a maximum of 256 bytes of matching length

namespace lha::detail {
Result compressLh4(std::span<const Byte> input, const Options& options) {
    return compressLhCompressed(input, options, 4096, 13, 4);
}

Result decompressLh4(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    return decompressLhCompressed(input, expectedSize, options, 4096, 13, 4);
}
 } // namespace lha::detail