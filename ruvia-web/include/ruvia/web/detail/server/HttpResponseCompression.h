#pragma once

#include "ruvia/http/detail/HeaderAcceptUtils.h"
#include "ruvia/web/HttpServerOptions.h"
#include "ruvia/http/HttpTypes.h"

#include <cstddef>
#include <memory_resource>
#include <string>

namespace ruvia::detail {

inline constexpr std::size_t kCompressionScratchRetainedBytes = 256 * 1024;

bool compressResponseBodyIfAccepted(
    HttpContentCoding coding,
    HttpResponse& response,
    const HttpServerOptions::Compression& options,
    std::pmr::string& compressionScratch,
    bool skipBody = false);

}  // namespace ruvia::detail
