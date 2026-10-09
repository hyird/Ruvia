#pragma once

#include <memory_resource>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ruvia/web/detail/http/context/request_bindings.h"
#include "ruvia/web/request_fields.h"

#include "context/context_response_state.h"
#include "context/context_services.h"
#include "context/context_session_state.h"
#include "http/request_query_values.h"

namespace ruvia::detail {

struct request_field_cache final {
    request_field_cache(std::pmr::vector<std::pmr::string>&& owned_storage,
        request_name_value_list&& owned_fields) noexcept
        : storage_(std::move(owned_storage)),
          fields_(std::move(owned_fields)) {}

    std::pmr::vector<std::pmr::string> storage_;
    request_name_value_list fields_;
};

// One arena object owns every request-local context value that is not a
// borrowed pointer: lazy caches, response/session machines, and typed bindings.
class context_request_storage final {
public:
    context_request_storage(context_services service_view, std::pmr::memory_resource* resource)
        : services_(service_view),
          response_state_(resource),
          session_state_(resource) {}

    context_request_storage(const context_request_storage&) = delete;
    context_request_storage& operator=(const context_request_storage&) = delete;

    context_services services_;
    context_response_state response_state_;
    context_session_state session_state_;
    request_bindings request_bindings_;

    std::optional<std::pmr::string> decoded_body_;
    std::optional<request_name_value_list> headers_;
    std::optional<request_query_cache> query_;
    std::optional<request_name_value_list> cookies_;
    std::optional<request_field_cache> route_params_;
    // Monotonic: survives buffered error construction by middleware after the
    // handshake has started, when returning to HTTP response mode is impossible.
    bool websocket_handshake_started_{false};
    bool tunnel_handshake_started_{false};
    bool query_invalid_{false};
    bool route_params_invalid_{false};
};

}  // namespace ruvia::detail
