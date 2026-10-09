#pragma once

#include <vector>

#include "ruvia/http/http_header.h"
#include "ruvia/web/http_client_upload_config.h"

#include "client/http_client_output_queue.h"

namespace ruvia::detail {
class http_client_upload_state final {
public:
    http_client_upload_state(const worker_handle& worker_value, std::pmr::memory_resource* resource, http_client_upload_config config)
        : output_(worker_value, resource),
          config_(config),
          trailers_(resource) {}
    http_client_upload_state(worker_handle&&, std::pmr::memory_resource*, http_client_upload_config) = delete;
    http_client_output_queue output_;
    http_client_upload_config config_;
    std::pmr::vector<http_header> trailers_;
    bool content_released_{};
    bool response_taken_{};
    ::ruvia::operation_scope response_scope_;
};
}  // namespace ruvia::detail
