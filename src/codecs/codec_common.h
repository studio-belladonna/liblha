#pragma once

#include <liblha/lha.h>

namespace lha::detail {

[[nodiscard]]
Result codecFailure(Error error, const char* message);

[[nodiscard]]
bool outputSizeAllowed(std::uint64_t size, const Options& options) noexcept;

} // namespace lha::detail
