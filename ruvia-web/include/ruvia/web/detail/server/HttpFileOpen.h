#pragma once

// std::ifstream file-open fallback for the web-layer server drivers. Real OS file
// I/O lives in ruvia-web, not in the pure sans-I/O ruvia-http protocol library --
// ruvia-http only owns the ResponseFileBody descriptor used to frame the response.

#include "ruvia/http/detail/HttpResponseFileBody.h"

#include <fstream>
#include <ios>

namespace ruvia::detail {

[[nodiscard]] inline std::ifstream openResponseFileInput(ResponseFileBody file) {
#if defined(_WIN32)
    return std::ifstream(file.toPath(), std::ios::binary);
#else
    return std::ifstream(file.nativePathCStr(), std::ios::binary);
#endif
}

}  // namespace ruvia::detail
