#pragma once

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
