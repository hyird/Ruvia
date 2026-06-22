#pragma once

#include "ruvia/app/App.h"
#include "ruvia/http/HttpTypes.h"

#include <cstddef>
#include <memory_resource>
#include <string>

namespace ruvia::detail {

inline constexpr std::size_t kCompressionScratchRetainedBytes = 256 * 1024;

bool compressResponseBodyIfAccepted(
    bool acceptsGzip,
    HttpResponse& response,
    const HttpServerOptions::Compression& options,
    std::pmr::string& compressionScratch,
    bool skipBody = false);

}  // namespace ruvia::detail
