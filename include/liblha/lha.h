#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lha {
using Byte = std::uint8_t;
using Bytes = std::vector<Byte>;

enum class Codec {
    Lh0,
    Lh1,
    Lh2,
    Lh3,
    Lh4,
    Lh5,
    Lh6,
    Lh7,
    Lz4,
    Lz5,
    Lzs,
    Lhx,
    Lk7,
    Pm0,
    Pm1,
    Pm2
};

enum class Error {
    None,
    UnsupportedCodec,
    InvalidInput,
    OutputLimitExceeded,
    SizeMismatch,
    InvalidArchive,
    ChecksumMismatch,
    InvalidName,
    IoError
};

struct Options {
    std::uint64_t maxOutputSize = 256ull * 1024 * 1024;
    std::uint64_t maxArchiveSize = 512ull * 1024 * 1024;
    std::size_t maxEntries = 100000;
    std::size_t maxNameLength = 255;
};

struct Result {
    Bytes data;
    Error error = Error::None;
    std::string message;

    [[nodiscard]]
    explicit operator bool() const noexcept { return error == Error::None; }
};

struct Entry {
    std::string name;
    Bytes data;
    Codec codec = Codec::Lh0;
};

struct ArchiveResult {
    std::vector<Entry> entries;
    Error error = Error::None;
    std::string message;

    [[nodiscard]]
    explicit operator bool() const noexcept { return error == Error::None; }
};

[[nodiscard]]
Result compress(Codec codec, std::span<const Byte> input, const Options& options = {});

[[nodiscard]]
Result decompress(Codec codec, std::span<const Byte> input, std::uint64_t expectedSize, const Options& options = {});

[[nodiscard]]
Result writeArchive(std::span<const Entry> entries, const Options& options = {});

[[nodiscard]]
ArchiveResult readArchive(std::span<const Byte> archive, const Options& options = {});

[[nodiscard]]
Result writePmaArchive(std::span<const Entry> entries, const Options& options = {});

[[nodiscard]]
ArchiveResult readPmaArchive(std::span<const Byte> archive, const Options& options = {});

[[nodiscard]]
Result writeFile(const std::filesystem::path& path, std::span<const Entry> entries, const Options& options = {});

[[nodiscard]]
ArchiveResult readFile(const std::filesystem::path& path, const Options& options = {});

[[nodiscard]]
std::string_view methodName(Codec codec) noexcept;

[[nodiscard]]
bool codecFromMethod(std::string_view method, Codec& codec) noexcept;

[[nodiscard]]
const char* errorName(Error error) noexcept;

} // namespace lha