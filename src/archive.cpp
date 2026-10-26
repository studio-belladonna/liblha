#include <liblha/lha.h>

#include "codecs/codec_common.h"

#include <fstream>
#include <limits>
#include <utility>

// archive file reading and writing.

namespace lha {
using detail::codecFailure;

namespace {
ArchiveResult archiveFailure(Error error, const char *message) {
    ArchiveResult result;
    result.error = error;
    result.message = message;
    return result;
}

std::uint16_t crc16(std::span<const Byte> data) {
    std::uint16_t crc = 0;
    for (Byte value : data) {
        crc ^= value;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) != 0 ? static_cast<std::uint16_t>((crc >> 1) ^ 0xA001) : static_cast<std::uint16_t>(crc >> 1);
        }
    }
    return crc;
}

std::uint32_t read32(std::span<const Byte> data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) 
        | (static_cast<std::uint32_t>(data[offset + 1]) << 8) 
        | (static_cast<std::uint32_t>(data[offset + 2]) << 16) 
        | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

void append16(Bytes &data, std::uint16_t value) {
    data.push_back(static_cast<Byte>(value & 0xff));
    data.push_back(static_cast<Byte>(value >> 8));
}

void append32(Bytes &data, std::uint32_t value) {
    data.push_back(static_cast<Byte>(value & 0xff));
    data.push_back(static_cast<Byte>((value >> 8) & 0xff));
    data.push_back(static_cast<Byte>((value >> 16) & 0xff));
    data.push_back(static_cast<Byte>(value >> 24));
}

bool hasNul(std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

} // namespace

//ファイル操作

Result writeArchive(std::span<const Entry> entries, const Options &options) {
    if (entries.size() > options.maxEntries) {
        return codecFailure(Error::OutputLimitExceeded, "Entry count exceeds configured limit");
    }

    Result result;
    std::uint64_t totalOutput = 0;
    for (const Entry &entry : entries) {
        if (entry.name.empty()
        ||  entry.name.size() > options.maxNameLength
        ||  entry.name.size() > 232
        ||  hasNul(entry.name)
        ) {
            return codecFailure(Error::InvalidName, "Entry name is empty, too long, or contains a NUL byte");
        }

        if (entry.data.size() > options.maxOutputSize 
        ||  totalOutput > options.maxOutputSize - entry.data.size()
        ) {
            return codecFailure(Error::OutputLimitExceeded, "Archive contents exceed configured output limit");
        }

        if (entry.data.size() > std::numeric_limits<std::uint32_t>::max()) {
            return codecFailure(Error::OutputLimitExceeded, "Entry exceeds the LHA level-0 size limit");
        }

        Result packed = compress(entry.codec, entry.data, options);
        if (!packed) {
            return codecFailure(packed.error, packed.message.c_str());
        }

        if (packed.data.size() > std::numeric_limits<std::uint32_t>::max()) {
            return codecFailure(Error::OutputLimitExceeded, "Packed entry exceeds the LHA level-0 size limit");
        }

        const std::size_t headerSize = 23 + entry.name.size();
        const std::size_t totalSize = headerSize + 2;
        if (result.data.size() > options.maxArchiveSize) {
            return codecFailure(Error::OutputLimitExceeded, "Archive exceeds configured size limit");
        }

        const std::uint64_t remaining = options.maxArchiveSize - result.data.size();
        if (totalSize > remaining
        ||  packed.data.size() > remaining - totalSize
        ||  remaining - totalSize - packed.data.size() < 1
        ) {
            return codecFailure(Error::OutputLimitExceeded, "Archive exceeds configured size limit");
        }
        Bytes header;
        header.reserve(totalSize);
        header.push_back(static_cast<Byte>(headerSize));
        header.push_back(0);

        const std::string_view method = methodName(entry.codec);
        for (char character : method) {
            header.push_back(static_cast<Byte>(character));
        }

        append32(header, static_cast<std::uint32_t>(packed.data.size()));
        append32(header, static_cast<std::uint32_t>(entry.data.size()));
        append32(header, 0);
        header.push_back(0x20);
        header.push_back(0);
        header.push_back(static_cast<Byte>(entry.name.size()));

        for (char character : entry.name) {
            header.push_back(static_cast<Byte>(character));
        }

        append16(header, crc16(entry.data));
        header.push_back('M');

        Byte checksum = 0;
        for (std::size_t i = 2; i < header.size(); ++i) {
            checksum = static_cast<Byte>(checksum + header[i]);
        }

        header[1] = checksum;
        
        result.data.insert(result.data.end(), header.begin(), header.end());
        result.data.insert(result.data.end(), packed.data.begin(), packed.data.end());

        totalOutput += entry.data.size();
    }

    if (result.data.size() >= options.maxArchiveSize) {
        return codecFailure(Error::OutputLimitExceeded, "Archive terminator exceeds configured size limit");
    }

    result.data.push_back(0);
    return result;
}

Result writePmaArchive(std::span<const Entry> entries, const Options &options) {
    for (const Entry &entry : entries) {
        if (entry.codec != Codec::Pm0 
        &&  entry.codec != Codec::Pm1 
        &&  entry.codec != Codec::Pm2
        ) {
            return codecFailure(Error::UnsupportedCodec, "PMA archives require a PMarc compression method");
        }
    }

    Result result = writeArchive(entries, options);
    if (!result) 
        return result;

    std::size_t offset = 0;
    while (offset < result.data.size() && result.data[offset] != 0) {
        const std::size_t headerSize = result.data[offset];
        if (headerSize < 1 || headerSize + 2 > result.data.size() - offset) {
            return codecFailure(Error::InvalidArchive, "Generated PMA header has invalid bounds");
        }

        const std::size_t headerEnd = offset + headerSize + 2;
        const std::uint32_t packedSize = read32(result.data, offset + 7);
        const Byte osType = result.data[headerEnd - 1];
        result.data[offset] = static_cast<Byte>(headerSize - 1);
        result.data[offset + 1] = static_cast<Byte>(result.data[offset + 1] - osType);
        result.data.erase(result.data.begin() + static_cast<std::ptrdiff_t>(headerEnd - 1));
        offset = headerEnd - 1 + packedSize;
    }

    return result;
}

ArchiveResult readArchive(std::span<const Byte> archive, const Options &options) {
    if (archive.size() > options.maxArchiveSize) {
        return archiveFailure(Error::OutputLimitExceeded, "Archive exceeds configured size limit");
    }

    ArchiveResult result;
    std::size_t offset = 0;
    std::uint64_t totalOutput = 0;

    while (offset < archive.size()) {
        const std::size_t headerStart = offset;
        const Byte headerSize = archive[offset];
        if (headerSize == 0) {
            for (std::size_t i = offset + 1; i < archive.size(); ++i) {
                if (archive[i] != 0) {
                    return archiveFailure(Error::InvalidArchive, "Nonzero bytes follow the archive terminator");
                }
            }

            return result;
        }

        if (result.entries.size() >= options.maxEntries) {
            return archiveFailure(Error::OutputLimitExceeded, "Entry count exceeds configured limit");
        }

        if (static_cast<std::size_t>(headerSize) + 2 > archive.size() - offset || headerSize < 23) {
            return archiveFailure(Error::InvalidArchive, "Truncated or undersized LHA level-0 header");
        }

        const std::string_view method(reinterpret_cast<const char *>(archive.data() + offset + 2), 5);
        const std::size_t headerEnd = offset + static_cast<std::size_t>(headerSize) + 2;
        std::uint8_t checksum = 0;

        for (std::size_t i = offset + 2; i < headerEnd; ++i) {
            checksum = static_cast<Byte>(checksum + archive[i]);
        }

        if (checksum != archive[offset + 1]) {
            return archiveFailure(Error::ChecksumMismatch, "LHA header checksum mismatch");
        }

        if (archive[offset + 20] != 0) {
            return archiveFailure(Error::InvalidArchive, "Only LHA level-0 headers are supported by the archive reader");
        }

        const Byte nameLength = archive[offset + 21];
        const std::size_t nameOffset = offset + 22;
        const std::size_t crcOffset = nameOffset + nameLength;
        const bool pmaHeader = method.starts_with("-pm") && headerSize == 22 + static_cast<std::size_t>(nameLength);
        const bool lhaHeader = headerSize == 23 + static_cast<std::size_t>(nameLength);
        if ((!pmaHeader && !lhaHeader) || nameLength == 0 || nameLength > options.maxNameLength) {
            return archiveFailure(Error::InvalidArchive, "Invalid LHA filename length or header layout");
        }

        if (crcOffset + (pmaHeader ? 2 : 3) != headerEnd) {
            return archiveFailure(Error::InvalidArchive, "Invalid LHA header field layout");
        }

        std::string name;
        name.reserve(nameLength);
        for (std::size_t i = 0; i < nameLength; ++i) {
            const Byte character = archive[nameOffset + i];
            if (character == 0) {
                return archiveFailure(Error::InvalidArchive, "LHA filename contains a NUL byte");
            }

            name.push_back(static_cast<char>(character));
        }

        Codec codec;
        if (!codecFromMethod(method, codec)) {
            return archiveFailure(Error::UnsupportedCodec, "LHA archive contains an unknown method");
        }

        const std::uint32_t packedSize = read32(archive, offset + 7);
        const std::uint32_t originalSize = read32(archive, offset + 11);
        const std::uint16_t expectedCrc = static_cast<std::uint16_t>(archive[crcOffset] | (static_cast<std::uint16_t>(archive[crcOffset + 1]) << 8));
        const std::size_t packedOffset = headerEnd;
        if (packedSize > archive.size() - packedOffset) {
            return archiveFailure(Error::InvalidArchive, "Packed entry extends past the end of the archive");
        }

        if (originalSize > options.maxOutputSize 
        || totalOutput > options.maxOutputSize - originalSize
        ) {
            return archiveFailure(Error::OutputLimitExceeded, "Archive contents exceed configured output limit");
        }

        Result unpacked = decompress(codec, archive.subspan(packedOffset, packedSize), originalSize, options);
        if (!unpacked) {
            return archiveFailure(unpacked.error, unpacked.message.c_str());
        }

        if (crc16(unpacked.data) != expectedCrc) {
            return archiveFailure(Error::ChecksumMismatch, "LHA entry CRC mismatch");
        }

        Entry entry;
        entry.name = std::move(name);
        entry.data = std::move(unpacked.data);
        entry.codec = codec;
        result.entries.push_back(std::move(entry));
        totalOutput += originalSize;

        offset = packedOffset + packedSize;
        if (offset <= headerStart) {
            return archiveFailure(Error::InvalidArchive, "Invalid archive entry bounds");
        }
    }

    return archiveFailure(Error::InvalidArchive, "Archive is missing its end marker");
}

ArchiveResult readPmaArchive(std::span<const Byte> archive, const Options &options) {
    while (!archive.empty() && archive.back() == 0x1a) {
        archive = archive.first(archive.size() - 1);
    }

    return readArchive(archive, options);
}

Result writeFile(const std::filesystem::path &path, std::span<const Entry> entries, const Options &options) {
    Result archive = writeArchive(entries, options);
    if (!archive) 
        return archive;

    if (archive.data.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return codecFailure(Error::OutputLimitExceeded, "Archive exceeds platform write limit");
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return codecFailure(Error::IoError, "Unable to open archive for writing");
    }

    if (!archive.data.empty()) {
        output.write(
            reinterpret_cast<const char *>(archive.data.data()),
            static_cast<std::streamsize>(archive.data.size())
        );
    }

    if (!output) {
        return codecFailure(Error::IoError, "Unable to write complete archive");
    }
    return {};
}

ArchiveResult readFile(const std::filesystem::path &path, const Options &options) {
    std::error_code error;
    const std::uintmax_t fileSize = std::filesystem::file_size(path, error);
    if (error){
        return archiveFailure(Error::IoError, "Unable to determine archive file size");
    }

    if (fileSize > options.maxArchiveSize 
    ||  fileSize > std::numeric_limits<std::size_t>::max() 
    ||  fileSize > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())
    ) {
        return archiveFailure(Error::OutputLimitExceeded, "Archive file exceeds configured or platform size limit");
    }

    Bytes data(static_cast<std::size_t>(fileSize));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return archiveFailure(Error::IoError, "Unable to open archive for reading");
    }

    if (!data.empty()) {
        input.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
        if (input.gcount() != static_cast<std::streamsize>(data.size())) {
            return archiveFailure(Error::IoError, "Unable to read complete archive");
        }
    }

    return readArchive(data, options);
}

} // namespace lha
