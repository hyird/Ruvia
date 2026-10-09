#pragma once

#include "ruvia/http/http1_close_policy.h"
#include "ruvia/http/http1_request_body_plan.h"
#include "ruvia/http/http_protocol_version.h"

namespace ruvia {

// Immutable request-side HTTP/1 persistence contract. Parsing establishes the
// version and initial disposition; later body-consumption policy may only close.
class http1_request_connection_plan final {
public:
    [[nodiscard]] static constexpr http1_request_connection_plan http11_close() noexcept {
        return {http_protocol_version::http11, http1_close_policy::close_after_response};
    }

    [[nodiscard]] constexpr http_protocol_version protocol_version() const noexcept {
        return version_;
    }
    [[nodiscard]] constexpr http1_close_policy disposition() const noexcept {
        return disposition_;
    }
    [[nodiscard]] constexpr http1_request_connection_plan require_close() const noexcept {
        return {version_, http1_close_policy::close_after_response};
    }

private:
    friend constexpr http1_request_connection_plan plan_http10_request_connection(bool, bool) noexcept;
    friend constexpr http1_request_connection_plan plan_http11_request_connection(bool) noexcept;

    constexpr http1_request_connection_plan(http_protocol_version version,
        http1_close_policy disposition) noexcept
        : version_(version),
          disposition_(disposition) {}

    http_protocol_version version_;
    http1_close_policy disposition_;
};

[[nodiscard]] inline constexpr http1_request_connection_plan plan_http10_request_connection(
    bool close, bool keep_alive) noexcept {
    return {http_protocol_version::http10,
        !close && keep_alive ? http1_close_policy::allow_reuse : http1_close_policy::close_after_response};
}

[[nodiscard]] inline constexpr http1_request_connection_plan plan_http11_request_connection(
    bool close) noexcept {
    return {http_protocol_version::http11,
        close ? http1_close_policy::close_after_response : http1_close_policy::allow_reuse};
}

[[nodiscard]] inline constexpr http1_request_connection_plan apply_request_body_consumption(
    http1_request_connection_plan plan, http1_request_body_consumption consumption) noexcept {
    return consumption == http1_request_body_consumption::complete ? plan : plan.require_close();
}

}  // namespace ruvia
