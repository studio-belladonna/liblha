#include "codec_common.h"
#include "codec_modules.h"

// LH6
// 32 KiB sliding window and supports a maximum of 256 bytes of matching length

namespace lha::detail {
Result compressLh6(std::span<const Byte> input, const Options& options) {
    return compressLhCompressed(input, options, 32768, 16, 5);
}

Result decompressLh6(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    return decompressLhCompressed(input, expectedSize, options, 32768, 16, 5);
}
} // namespace lha::detail
