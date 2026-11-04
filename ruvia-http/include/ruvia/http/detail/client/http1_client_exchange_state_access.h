#pragma once

#include <string_view>

#include "ruvia/http/http1_client_exchange_state.h"

namespace ruvia::detail {

struct http1_client_exchange_state_access final {
    [[nodiscard]] static constexpr http_known_method method(
        const http1_client_exchange_state& state_value) noexcept {
        return state_value.method_;
    }

    [[nodiscard]] static constexpr http_connection_options connection_options(
        const http1_client_exchange_state& state_value) noexcept {
        return state_value.connection_options_;
    }

    [[nodiscard]] static constexpr http1_close_policy close_policy(
        const http1_client_exchange_state& state_value) noexcept {
        return state_value.close_policy_;
    }

    [[nodiscard]] static constexpr http1_client_initial_content_state content_state(
        const http1_client_exchange_state& state_value) noexcept {
        return state_value.content_state_;
    }

    [[nodiscard]] static std::string_view offered_upgrade_protocols(
        const http1_client_exchange_state& state_value) noexcept {
        return state_value.offered_upgrade_protocols_;
    }
};

}  // namespace ruvia::detail
