#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http1_close_policy.h"
#include "ruvia/http/http_known_method.h"

namespace ruvia {

class prepared_http1_client_request;

namespace detail {

struct http1_client_request_prepare_result_access;

enum class http1_client_initial_content_state : std::uint8_t {
    complete,
    pending,
    awaiting_continue,
};

struct http1_client_exchange_state_access;

}  // namespace detail

// Owning protocol facts for exactly one HTTP/1 response exchange. Preparing a
// request creates this state from the facts that actually entered the wire
// plan; transferring it to the response parser removes every dependency on the
// caller's method and header storage. Only an offered Upgrade value needs owned
// dynamic storage, so ordinary requests remain allocation-free.
class http1_client_exchange_state final {
public:
    http1_client_exchange_state(const http1_client_exchange_state&) = delete;
    http1_client_exchange_state& operator=(const http1_client_exchange_state&) = delete;
    http1_client_exchange_state(http1_client_exchange_state&&) noexcept = default;
    http1_client_exchange_state& operator=(http1_client_exchange_state&&) = delete;

private:
    friend class prepared_http1_client_request;
    friend struct detail::http1_client_request_prepare_result_access;
    friend struct detail::http1_client_exchange_state_access;

    http1_client_exchange_state(
        const http1_client_exchange_state& other, std::pmr::memory_resource* resource)
        : offered_upgrade_protocols_(other.offered_upgrade_protocols_, resource),
          method_(other.method_),
          connection_options_(other.connection_options_),
          close_policy_(other.close_policy_),
          content_state_(other.content_state_) {}

    http1_client_exchange_state(http_known_method method,
        detail::http_connection_options connection_options, http1_close_policy close_policy,
        detail::http1_client_initial_content_state content_state,
        std::pmr::string offered_upgrade_protocols) noexcept
        : offered_upgrade_protocols_(std::move(offered_upgrade_protocols)),
          method_(method),
          connection_options_(connection_options),
          close_policy_(close_policy),
          content_state_(content_state) {}

    std::pmr::string offered_upgrade_protocols_;
    http_known_method method_{http_known_method::unknown};
    detail::http_connection_options connection_options_;
    http1_close_policy close_policy_{http1_close_policy::allow_reuse};
    detail::http1_client_initial_content_state content_state_{
        detail::http1_client_initial_content_state::complete};
};

namespace detail {

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

}  // namespace detail

}  // namespace ruvia
