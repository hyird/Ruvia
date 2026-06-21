#pragma once

#include "ruvia/app/App.h"
#include "ruvia/http/HttpParser.h"
#include "ruvia/http/HttpTypes.h"

#include <memory_resource>
#include <string>

namespace ruvia::detail {

bool compressResponseBodyIfAccepted(
    const HttpRequestFlags& requestFlags,
    HttpResponse& response,
    const HttpServerOptions::Compression& options,
    std::pmr::string* compressionScratch,
    bool skipBody = false);

}  // namespace ruvia::detail
