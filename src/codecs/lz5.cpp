#include "codec_common.h"
#include "codec_modules.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

// LZ5 codec
// Supports a 4 KiB sliding window with a maximum 17 bytes of matching length

// TODO: clean up this mess!!!

namespace lha::detail {

namespace {
constexpr std::size_t windowSize = 4096;
constexpr std::size_t maxMatch = 18;
constexpr std::size_t minMatch = 3;
constexpr std::size_t startOffset = 18;
constexpr std::size_t initialPosition = windowSize - startOffset;
constexpr std::size_t hashSize = 65536;
constexpr std::size_t maxSearchAttempts = 128;

std::array<Byte, windowSize> initialWindow() {
    std::array<Byte, windowSize> window{};
    std::size_t position = 0;

    for (std::size_t value = 0; value < 256; ++value) {
        for (std::size_t count = 0; count < 13; ++count) {
            window[position++] = static_cast<Byte>(value);
        }
    }

    for (std::size_t value = 0; value < 256; ++value) {
        window[position++] = static_cast<Byte>(value);
    }

    for (std::size_t value = 0; value < 256; ++value) {
        window[position++] = static_cast<Byte>(255 - value);
    }

    for (std::size_t count = 0; count < 128; ++count) {
        window[position++] = 0;
    }

    for (std::size_t count = 0; count < 110; ++count) {
        window[position++] = ' ';
    }

    for (std::size_t count = 0; count < startOffset; ++count) {
        window[position++] = 0;
    }

    return window;
}

std::uint16_t hashAt(std::span<const Byte> input, std::size_t position) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(input[position]) * 251u + input[position + 1]) * 251u + input[position + 2]);
}

} // namespace

Result compressLz5(std::span<const Byte> input, const Options& options) {
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
    std::vector<std::size_t> previous(windowSize + input.size(), missing);
    const auto initial = initialWindow();
    std::array<Byte, windowSize> chronologicalInitial{};

    for (std::size_t position = 0; position < windowSize; ++position) {
        chronologicalInitial[position] = initial[(position + initialPosition) % windowSize];
    }

    const auto timelineByte = [&](std::size_t position) -> Byte {
        return position < windowSize ? chronologicalInitial[position] : input[position - windowSize];
    };

    const auto timelineHash = [&](std::size_t position) {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(timelineByte(position)) * 251u + timelineByte(position + 1)) * 251u
            + timelineByte(position + 2));
    };

    for (std::size_t position = 0; position < windowSize; ++position) {
        const std::uint16_t hash = timelineHash(position);
        previous[position] = heads[hash];
        heads[hash] = position;
    }

    Result result;
    std::size_t position = 0;
    while (position < input.size()) {
        const std::size_t flagsOffset = result.data.size();
        result.data.push_back(0);
        Byte flags = 0;

        // christ
        for (unsigned int token = 0; token < 8 && position < input.size(); ++token) {
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

                    candidate = previous[candidate];
                }
            }

            const std::size_t consumed = bestLength >= minMatch ? bestLength : 1;
            if (consumed == 1) {
                flags |= static_cast<Byte>(1u << token);
                result.data.push_back(input[position]);
            } else {
                result.data.push_back(static_cast<Byte>(bestPosition & 0xff));
                result.data.push_back(static_cast<Byte>(((bestPosition >> 4) & 0xf0) | (consumed - minMatch)));
            }

            for (std::size_t index = 0; index < consumed; ++index) {
                const std::size_t current = absolutePosition + index;
                if (current + 2 < windowSize + input.size()) {
                    const std::uint16_t hash = timelineHash(current);
                    previous[current] = heads[hash];
                    heads[hash] = current;
                }
            }

            position += consumed;
        }

        result.data[flagsOffset] = flags;
    }


    return result;
}

Result decompressLz5(std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    if (!outputSizeAllowed(expectedSize, options)) {
        return codecFailure(
            Error::OutputLimitExceeded,
            "Expected output exceeds configured or platform size limit"
        );
    }

    if (input.size() != 0 && expectedSize == 0) {
        return codecFailure(
            Error::SizeMismatch,
            "Compressed input is nonempty for empty output"
        );
    }

    if (expectedSize == 0) 
        return {};

    std::array<Byte, windowSize> window = initialWindow();
    std::size_t windowPosition = initialPosition;

    Bytes output;
    output.reserve(static_cast<std::size_t>(expectedSize));
    std::size_t inputPosition = 0;
    while (output.size() < expectedSize) {
        if (inputPosition >= input.size()) {
            return codecFailure(
                Error::InvalidInput,
                "Truncated LZ5 flag byte"
            );
        }

        const Byte flags = input[inputPosition++];
        for (unsigned int token = 0; token < 8 && output.size() < expectedSize; ++token) {
            if ((flags & (1u << token)) != 0) {
                if (inputPosition >= input.size()) {
                    return codecFailure(
                        Error::InvalidInput,
                        "Truncated LZ5 literal"
                    );
                }

                const Byte value = input[inputPosition++];
                output.push_back(value);
                window[windowPosition] = value;
                windowPosition = (windowPosition + 1) % windowSize;
            } else {
                if (input.size() - inputPosition < 2) {
                    return codecFailure(
                        Error::InvalidInput,
                        "Truncated LZ5 match"
                    );
                }

                const std::size_t matchPosition = input[inputPosition++]
                    | (static_cast<std::size_t>(input[inputPosition] & 0xf0) << 4);
                const std::size_t length = (input[inputPosition++] & 0x0f) + minMatch;
                if (length > expectedSize - output.size()) {
                    return codecFailure(
                        Error::SizeMismatch,
                        "LZ5 match exceeds expected output size"
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
    }

    if (inputPosition != input.size()) {
        return codecFailure(
            Error::InvalidInput,
            "Trailing bytes follow LZ5 stream"
        );
    }

    Result result;
    result.data = std::move(output);
    return result;
}

} // namespace lha::detail