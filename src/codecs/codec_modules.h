#pragma once

#include <liblha/lha.h>

namespace lha::detail {

using CompressFunction = Result (*)(std::span<const Byte>, const Options&);
using DecompressFunction = Result (*)(std::span<const Byte>, std::uint64_t, const Options&);

struct CodecModule {
    Codec codec;
    std::string_view method;
    CompressFunction compress;
    DecompressFunction decompress;
};

[[nodiscard]]
std::span<const CodecModule> codecModules() noexcept;

[[nodiscard]]
const CodecModule* findCodecModule(Codec codec) noexcept;

[[nodiscard]]
Result compressLhCompressed(
    std::span<const Byte> input, 
    const Options& options, 
    std::size_t windowSize, 
    std::size_t positionSymbols, 
    unsigned int positionCountBits, 
    bool lhark = false
);

[[nodiscard]]
Result decompressLhCompressed(
    std::span<const Byte> input, 
    std::uint64_t expectedSize, 
    const Options& options, 
    std::size_t windowSize, 
    std::size_t positionSymbols, 
    unsigned int positionCountBits, 
    bool lhark = false
);

[[nodiscard]]
Result compressLhAdaptive(
    std::span<const Byte> input, 
    const Options& options, 
    std::size_t windowSize, 
    std::size_t maxMatch, 
    std::size_t positionPrefixes, 
    unsigned int lowPositionBits
);

[[nodiscard]]
Result decompressLhAdaptive(
    std::span<const Byte> input, 
    std::uint64_t expectedSize, 
    const Options& options, 
    std::size_t windowSize, 
    std::size_t maxMatch, 
    std::size_t positionPrefixes, 
    unsigned int lowPositionBits
);

} // namespace lha::detail