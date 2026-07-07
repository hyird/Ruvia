#pragma once

#include <cstddef>

#include "ruvia/http/HttpCommon.h"

namespace ruvia::detail {

// The byte ceiling that applies to a request body given its routing mode: stream
// routes use the (opt-in) stream limit, every other route the buffered limit. A
// limit of 0 means "unbounded" and is preserved by the callers. Shared by the
// HTTP/1 and HTTP/2 dispatch paths and HTTP/2 DATA accounting so the selection
// lives in exactly one place.
[[nodiscard]] inline std::size_t requestBodyByteLimit(
    RequestBodyMode bodyMode,
    std::size_t maxStreamBodyBytes,
    std::size_t maxBufferedBodyBytes) noexcept {
    return bodyMode == RequestBodyMode::kStream ? maxStreamBodyBytes : maxBufferedBodyBytes;
}

}  // namespace ruvia::detail
