#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

#include "ruvia/web/context.h"
#include "ruvia/web/detail/http/context/request_bindings.h"

#include "context/context_response_state.h"
#include "context/context_services.h"
#include "context/context_session_state.h"
#include "http/request_query_values.h"
#include "server/inbound_buffer_budget.h"

namespace ruvia::detail {

struct aligned_storage_deleter final {
    std::size_t alignment_{alignof(std::max_align_t)};
    void operator()(std::byte* storage) const noexcept {
        ::operator delete[](storage, std::align_val_t{alignment_});
    }
};

// Owns every request-local context value that is not a borrowed pointer: lazy
// caches, response/session machines, typed bindings, and middleware next() hop
// storage. Synchronous construction borrows the input view, then retains one
// services value here before returning; no reference to the input survives.
class context_request_storage final {
public:
    explicit context_request_storage(const context_services& service_view) noexcept
        : services_(service_view),
          decoded_body_reservation_(services_.inbound_buffers()) {}

    context_request_storage(const context_request_storage&) = delete;
    context_request_storage& operator=(const context_request_storage&) = delete;

    [[nodiscard]] std::byte* allocate_raw(std::size_t size, std::size_t alignment) {
        auto* storage = static_cast<std::byte*>(::operator new[](size, std::align_val_t{alignment}));
        try {
            owned_blocks_.emplace_back(storage, aligned_storage_deleter{alignment});
        } catch (...) {
            ::operator delete[](storage, std::align_val_t{alignment});
            throw;
        }
        return storage;
    }

    context_services services_;
    context_response_state response_state_;
    context_session_state session_state_;
    request_bindings request_bindings_;

    std::optional<std::string> decoded_body_;
    inbound_buffer_reservation decoded_body_reservation_;
    std::optional<request_name_value_list> headers_;
    std::optional<request_query_cache> query_;
    std::optional<request_name_value_list> cookies_;
    std::string decoded_route_params_;
    // Views borrow route storage, the request path, or decoded_route_params_.
    std::optional<request_name_value_list> route_params_;
    std::vector<std::unique_ptr<std::byte[], aligned_storage_deleter>> owned_blocks_;
    bool route_params_invalid_{false};
    bool route_params_unencoded_{false};
};

}  // namespace ruvia::detail
