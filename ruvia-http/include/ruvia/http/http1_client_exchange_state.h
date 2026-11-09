#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <utility>

#include "ruvia/http/http1_request_connection_plan.h"
#include "ruvia/http/http_known_method.h"

namespace ruvia {

namespace detail {

enum class http1_client_initial_content_state : std::uint8_t {
    complete,
    pending,
    awaiting_continue,
};

struct http1_client_request_prepare_result_access;

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
    friend struct detail::http1_client_request_prepare_result_access;
    friend struct detail::http1_client_exchange_state_access;

    http1_client_exchange_state(http_known_method method, http1_close_policy close_policy,
        detail::http1_client_initial_content_state content_state,
        std::pmr::string offered_upgrade_protocols) noexcept
        : offered_upgrade_protocols_(std::move(offered_upgrade_protocols)),
          method_(method),
          close_policy_(close_policy),
          content_state_(content_state) {}

    std::pmr::string offered_upgrade_protocols_;
    http_known_method method_{http_known_method::unknown};
    http1_close_policy close_policy_{http1_close_policy::allow_reuse};
    detail::http1_client_initial_content_state content_state_{
        detail::http1_client_initial_content_state::complete};
};

}  // namespace ruvia
