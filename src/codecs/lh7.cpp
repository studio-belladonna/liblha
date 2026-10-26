#include "codec_common.h"
#include "codec_modules.h"

// LH7
// 64 KiB sliding window and supports a maximum of 256 bytes of matching length

namespace lha::detail {
Result compressLh7(std::span<const Byte> input, const Options& options) {
    return compressLhCompressed(input, options, 65536, 17, 5);
}

Result decompressLh7(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    return decompressLhCompressed(input, expectedSize, options, 65536, 17, 5);
}
} // namespace lha::detail