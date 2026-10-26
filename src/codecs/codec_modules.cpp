#include "codec_modules.h"

#include <array>

namespace lha::detail {

Result compressLh0(std::span<const Byte>, const Options&);
Result decompressLh0(std::span<const Byte>, std::uint64_t, const Options&);

Result compressLh1(std::span<const Byte>, const Options&);
Result decompressLh1(std::span<const Byte>, std::uint64_t, const Options&);

Result compressLh2(std::span<const Byte>, const Options&);
Result decompressLh2(std::span<const Byte>, std::uint64_t, const Options&);

Result compressLh3(std::span<const Byte>, const Options&);
Result decompressLh3(std::span<const Byte>, std::uint64_t, const Options&);

Result compressLh4(std::span<const Byte>, const Options&);
Result decompressLh4(std::span<const Byte>, std::uint64_t, const Options&);

Result compressLh5(std::span<const Byte>, const Options&);
Result decompressLh5(std::span<const Byte>, std::uint64_t, const Options&);

Result compressLh6(std::span<const Byte>, const Options&);
Result decompressLh6(std::span<const Byte>, std::uint64_t, const Options&);

Result compressLh7(std::span<const Byte>, const Options&);
Result decompressLh7(std::span<const Byte>, std::uint64_t, const Options&);

Result compressLz4(std::span<const Byte>, const Options&);
Result decompressLz4(std::span<const Byte>, std::uint64_t, const Options&);

Result compressLz5(std::span<const Byte>, const Options&);
Result decompressLz5(std::span<const Byte>, std::uint64_t, const Options&);

Result compressLzs(std::span<const Byte>, const Options&);
Result decompressLzs(std::span<const Byte>, std::uint64_t, const Options&);

Result compressLhx(std::span<const Byte>, const Options&);
Result decompressLhx(std::span<const Byte>, std::uint64_t, const Options&);

Result compressLk7(std::span<const Byte>, const Options&);
Result decompressLk7(std::span<const Byte>, std::uint64_t, const Options&);

Result compressPm0(std::span<const Byte>, const Options&);
Result decompressPm0(std::span<const Byte>, std::uint64_t, const Options&);

Result compressPm1(std::span<const Byte>, const Options&);
Result decompressPm1(std::span<const Byte>, std::uint64_t, const Options&);

Result compressPm2(std::span<const Byte>, const Options&);
Result decompressPm2(std::span<const Byte>, std::uint64_t, const Options&);

namespace {

constexpr std::array<CodecModule, 16> modules = {{
    {Codec::Lh0, "-lh0-", compressLh0, decompressLh0},
    {Codec::Lh1, "-lh1-", compressLh1, decompressLh1},
    {Codec::Lh2, "-lh2-", compressLh2, decompressLh2},
    {Codec::Lh3, "-lh3-", compressLh3, decompressLh3},
    {Codec::Lh4, "-lh4-", compressLh4, decompressLh4},
    {Codec::Lh5, "-lh5-", compressLh5, decompressLh5},
    {Codec::Lh6, "-lh6-", compressLh6, decompressLh6},
    {Codec::Lh7, "-lh7-", compressLh7, decompressLh7},
    {Codec::Lz4, "-lz4-", compressLz4, decompressLz4},
    {Codec::Lz5, "-lz5-", compressLz5, decompressLz5},
    {Codec::Lzs, "-lzs-", compressLzs, decompressLzs},
    {Codec::Lhx, "-lhx-", compressLhx, decompressLhx},
    {Codec::Lk7, "-lk7-", compressLk7, decompressLk7},
    {Codec::Pm0, "-pm0-", compressPm0, decompressPm0},
    {Codec::Pm1, "-pm1-", compressPm1, decompressPm1},
    {Codec::Pm2, "-pm2-", compressPm2, decompressPm2},
}};

} // namespace

std::span<const CodecModule> codecModules() noexcept {
    return modules;
}

const CodecModule* findCodecModule(Codec codec) noexcept {
    for (const CodecModule& module : modules) {
        if (module.codec == codec) {
            return &module;
        }
    }

    return nullptr;
}

} // namespace lha::detail
