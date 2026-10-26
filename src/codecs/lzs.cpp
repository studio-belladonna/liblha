#include "codec_common.h"
#include "codec_modules.h"
#include "bit_io.h"

#include <array>
#include <limits>
#include <utility>

// LZS codec
// Supports a 2 KiB sliding window with a maximum 17 bytes of matching length

namespace lha::detail {

namespace {
constexpr std::size_t windowSize = 2048;
constexpr std::size_t startOffset = 17;
constexpr std::size_t initialPosition = windowSize - startOffset;
constexpr std::size_t minMatch = 2;
constexpr std::size_t maxMatch = 17;
constexpr std::size_t hashSize = 65536;
constexpr std::size_t maxSearchAttempts = 128;

std::uint16_t hashAt(std::span<const Byte> input, std::size_t position) {
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(input[position]) * 257u + input[position + 1]);
}
} // namespace

Result compressLzs(std::span<const Byte> input, const Options& options) {
    if (!outputSizeAllowed(input.size(), options)) {
        return codecFailure(
            Error::OutputLimitExceeded,
            "Input exceeds configured size limit"
        );
    }

    if (input.empty()) {
        return {};
    }

    const std::size_t missing = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> heads(hashSize, missing);
    std::vector<std::size_t> previous(windowSize, missing);
    const auto timelineByte = [&](std::size_t position) -> Byte {
        return position < windowSize ? static_cast<Byte>(' ') : input[position - windowSize];
    };

    const auto timelineHash = [&](std::size_t position) {
        return static_cast<std::uint16_t>(
            static_cast<std::uint32_t>(timelineByte(position)) * 257u + timelineByte(position + 1));
    };

    for (std::size_t position = 0; position + 1 < windowSize; ++position) {
        const std::uint16_t hash = timelineHash(position);
        previous[position % windowSize] = heads[hash];
        heads[hash] = position;
    }

    BitWriter writer;
    std::size_t position = 0;
    while (position < input.size()) {
        const std::size_t absolutePosition = windowSize + position;
        std::size_t bestLength = 0;
        std::size_t bestPosition = 0;
        if (position + minMatch <= input.size()) {
            std::size_t candidate = heads[hashAt(input, position)];
            std::size_t attempts = 0;
            while (candidate != missing 
                && absolutePosition - candidate <= windowSize
                && attempts++ < maxSearchAttempts
            ) {
                std::size_t length = 0;
                while (length < maxMatch 
                    && position + length < input.size()
                    && timelineByte(candidate + length) == input[position + length]
                ) {
                    ++length;
                }

                if (length > bestLength) {
                    bestLength = length;
                    bestPosition = (candidate + initialPosition) % windowSize;
                    if (length == maxMatch) {
                        break;
                    }
                }

                candidate = previous[candidate % windowSize];
            }
        }

        const std::size_t consumed = bestLength >= minMatch ? bestLength : 1;
        if (consumed == 1) {
            writer.write(1, 1);
            writer.write(8, input[position]);
        } else {
            writer.write(1, 0);
            writer.write(11, static_cast<std::uint32_t>(bestPosition));
            writer.write(4, static_cast<std::uint32_t>(consumed - minMatch));
        }

        for (std::size_t index = 0; index < consumed; ++index) {
            const std::size_t current = absolutePosition + index;
            if (current + 1 < windowSize + input.size()) {
                const std::uint16_t hash = timelineHash(current);
                previous[current % windowSize] = heads[hash];
                heads[hash] = current;
            }
        }

        position += consumed;
    }

    Result result;
    result.data = writer.finish();
    return result;
}

Result decompressLzs(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    if (!outputSizeAllowed(expectedSize, options)) {
        return codecFailure(
            Error::OutputLimitExceeded,
            "Expected output exceeds configured or platform size limit"
        );
    }

    if (expectedSize == 0) {
        if (input.empty()) {
            return {};
        }
        return codecFailure(
            Error::InvalidInput,
            "Unexpected data in empty LZS stream"
        );
    }

    std::array<Byte, windowSize> window{};
    window.fill(' ');
    std::size_t windowPosition = initialPosition;
    Bytes output;
    output.reserve(static_cast<std::size_t>(expectedSize));
    BitReader reader(input);

    while (output.size() < expectedSize) {
        std::uint32_t type = 0;
        if (!reader.read(1, type)) {
            return codecFailure(
                Error::InvalidInput,
                "Truncated LZS command type"
            );
        }

        if (type != 0) {
            std::uint32_t value = 0;
            if (!reader.read(8, value)) {
                return codecFailure(
                    Error::InvalidInput,
                    "Truncated LZS literal"
                );
            }
            const Byte byte = static_cast<Byte>(value);
            output.push_back(byte);
            window[windowPosition] = byte;
            windowPosition = (windowPosition + 1) % windowSize;
        } else {
            std::uint32_t matchPosition = 0;
            std::uint32_t lengthCode = 0;
            if (!reader.read(11, matchPosition) || !reader.read(4, lengthCode)) {
                return codecFailure(
                    Error::InvalidInput,
                    "Truncated LZS match"
                );
            }

            const std::size_t length = static_cast<std::size_t>(lengthCode) + minMatch;
            if (length > expectedSize - output.size()) {
                return codecFailure(
                    Error::SizeMismatch,
                    "LZS match exceeds expected output size"
                );
            }

            for (std::size_t index = 0; index < length; ++index) {
                const Byte value = window[(matchPosition + index) % windowSize];
                output.push_back(value);
                window[windowPosition] = value;
                windowPosition = (windowPosition + 1) % windowSize;
            }
        }
    }

    if (!reader.hasValidPadding()) {
        return codecFailure(
            Error::InvalidInput,
            "Invalid trailing data or padding in LZS stream"
        );
    }

    Result result;
    result.data = std::move(output);
    return result;
}

} // namespace lha::detail