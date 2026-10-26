#pragma once

#include <exception>
#include <vector>

#include "ruvia/http/http_header.h"
#include "ruvia/web/http_client_types.h"

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
    // An HTTP/1 peer can stop accepting an in-flight upload while still
    // delivering a complete response. The upload operation reports this
    // independently so it cannot poison that response before framing proves
    // whether the response completed.
    std::exception_ptr failure_;
    bool content_released_{};
    bool response_taken_{};
    bool response_active_{};
    ::ruvia::operation_scope response_scope_;
};
}  // namespace ruvia::detail
