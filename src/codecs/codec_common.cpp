#include "codec_common.h"

#include <limits>

namespace lha::detail {

Result codecFailure(Error error, const char* message) {
    Result result;
    result.error = error;
    result.message = message;
    return result;
}

bool outputSizeAllowed(std::uint64_t size, const Options& options) noexcept {
    return size <= options.maxOutputSize 
        && size <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
}

} // namespace lha::detail