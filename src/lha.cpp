#include <liblha/lha.h>

#include "codecs/codec_common.h"
#include "codecs/codec_modules.h"

namespace lha {

Result compress(Codec codec, std::span<const Byte> input, const Options& options) {
    if (const detail::CodecModule* module = detail::findCodecModule(codec)) {
        return module->compress(input, options);
    }

    return detail::codecFailure(Error::UnsupportedCodec, "Unknown compression codec");
}

Result decompress(Codec codec, std::span<const Byte> input, std::uint64_t expectedSize, const Options& options) {
    if (const detail::CodecModule* module = detail::findCodecModule(codec)) {
        return module->decompress(input, expectedSize, options);
    }

    return detail::codecFailure(Error::UnsupportedCodec, "Unknown decompression codec");
}

std::string_view methodName(Codec codec) noexcept {
    if (const detail::CodecModule* module = detail::findCodecModule(codec)) {
        return module->method;
    }

    return {};
}

bool codecFromMethod(std::string_view method, Codec& codec) noexcept {
    for (const detail::CodecModule& module : detail::codecModules()) {
        if (module.method == method) {
            codec = module.codec;
            return true;
        }
    }

    return false;
}

const char* errorName(Error error) noexcept {
    switch (error) {
        case Error::None: return "None";
        case Error::UnsupportedCodec: return "UnsupportedCodec";
        case Error::InvalidInput: return "InvalidInput";
        case Error::OutputLimitExceeded: return "OutputLimitExceeded";
        case Error::SizeMismatch: return "SizeMismatch";
        case Error::InvalidArchive: return "InvalidArchive";
        case Error::ChecksumMismatch: return "ChecksumMismatch";
        case Error::InvalidName: return "InvalidName";
        case Error::IoError: return "IoError";
    }
    
    return "Unknown";
}

} // namespace lha