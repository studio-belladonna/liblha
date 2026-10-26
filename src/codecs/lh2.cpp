#include "codec_common.h"
#include "codec_modules.h"

namespace lha::detail {

// LH2 codec
// This is based on LH1 and supports 8 KiB sliding window
// Supports a maximum of 256 bytes of matching length

Result compressLh2(std::span<const Byte> input, const Options& options) {
    return compressLhAdaptive(input, options, 8192, 256, 128, 6);
}

Result decompressLh2(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    return decompressLhAdaptive(input, expectedSize, options, 8192, 256, 128, 6);
}


} // namespace lha::detail

