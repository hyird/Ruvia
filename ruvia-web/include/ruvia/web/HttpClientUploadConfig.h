#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "ruvia/http/HttpClient.h"

namespace ruvia {
struct HttpClientUploadConfig final {
    std::optional<std::uint64_t> contentLength{};
    HttpClientRequestExpectation expectation{HttpClientRequestExpectation::kNone};
    std::size_t maxChunkBytes{64 * 1024};
    std::chrono::milliseconds continueTimeout{1000};
};
}  // namespace ruvia
