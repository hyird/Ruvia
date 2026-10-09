#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "ruvia/http/http_client.h"

namespace ruvia {
struct http_client_upload_config final {
    std::optional<std::uint64_t> content_length_{};
    http_client_request_expectation expectation_{http_client_request_expectation::none};
    std::size_t max_chunk_bytes_{64 * 1024};
    std::chrono::milliseconds continue_timeout_{1000};
};
}  // namespace ruvia
