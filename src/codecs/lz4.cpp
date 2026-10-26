#include "codec_common.h"
#include "codec_modules.h"

// LZ4 codec
// Similar to LH0

namespace lha::detail {

Result compressLz4(std::span<const Byte> input, const Options& options) {
    if (!outputSizeAllowed(input.size(), options)) {
        return codecFailure(
            Error::OutputLimitExceeded,
            "Input exceeds configured size limit"
        );
    }

    Result result;
    result.data.assign(input.begin(), input.end());
    return result;
}

Result decompressLz4(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    if (!outputSizeAllowed(expectedSize, options)) {
        return codecFailure(
            Error::OutputLimitExceeded,
            "Expected output exceeds configured or platform size limit"
        );
    }

    if (input.size() != static_cast<std::size_t>(expectedSize)) {
        return codecFailure(
            Error::SizeMismatch,
            "Stored input size does not match expected size"
        );
    }

    Result result;
    result.data.assign(input.begin(), input.end());
    return result;
}

} // namespace lha::detail
