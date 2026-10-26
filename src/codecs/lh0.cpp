#include "codec_common.h"
#include "codec_modules.h"

// LH0 codec
// No compression, this simply stores data in to the archive.

namespace lha::detail {

Result compressLh0(std::span<const Byte> input, const Options& options) {
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

Result decompressLh0(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
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
