#pragma once

#include "ruvia/web/http_client_types.h"

#include "client/http_client_output_queue.h"

namespace ruvia::detail {
class http_client_tunnel_state final {
public:
    http_client_tunnel_state(const worker_handle& worker_value, std::pmr::memory_resource* resource, http_client_tunnel_config config)
        : output_(worker_value, resource),
          config_(config) {}
    http_client_output_queue output_;
    http_client_tunnel_config config_;
    bool udp_{};
    bool accepted_{};
    bool receive_ended_{};
};
}  // namespace ruvia::detail
