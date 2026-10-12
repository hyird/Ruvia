#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/web/detail/router/route_modes.h"
#include "ruvia/web/error.h"

#include "context/context_services.h"
#include "http/static_file_variant.h"
#include "ratelimit/rate_limiter.h"
#include "router/route_resolution.h"
#include "server/http_buffered_response.h"
#include "server/http_response_compression.h"
#include "server/http_server_options.h"
#include "server/request_deadline.h"

// Shared admission for HTTP/1 and in-process subrequests. The HTTP/1 session
// reads and frames messages; admission decides coding, client identity,
// deadlines, expectations, rate limits, body ceilings and endpoint kind.

namespace ruvia::detail {

class route_table;

// The deployment limits that shape admission. Servers build it from their
// options; a subrequest builds it from its parent (no rate limit: the parent
// request already passed it).
struct http_request_admission_policy final {
    std::optional<std::chrono::milliseconds> handler_deadline_{};
    std::optional<std::size_t> max_stream_body_bytes_{};
    std::size_t max_buffered_body_bytes_{};
    http_response_coding_availability coding_availability_{http_response_coding_availability::identity_only};
    bool apply_rate_limit_{true};

    [[nodiscard]] static http_request_admission_policy server(const http_server_options& options) noexcept;
};

struct http_request_admission_input final {
    const http_request& request_;
    const route_resolution& resolution_;
    // The parser or subrequest supplies the response-coding selection.
    http_response_coding_selection_result coding_;
    // Request arena for copied rejection diagnostics.
    std::pmr::memory_resource* resource_;
    // The protocol's Expect plan, when it defers the rejection to Web.
    const http_server_expectation_plan* expectation_{};
    // Start of the request for the access-log duration.
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
};

// Effective request-body ceiling. A route may only tighten the deployment limit.
[[nodiscard]] protocol_byte_limit http_request_body_limit(const route_resolution& resolution,
    request_body_mode mode, const http_request_admission_policy& policy) noexcept;

// A server-layer rejection decided before any handler ran. The diagnostic is
// borrowed from static storage or the request arena.
class http_request_rejection final {
public:
    [[nodiscard]] static constexpr http_request_rejection error(http_error_info error) noexcept {
        return http_request_rejection(error, std::nullopt);
    }

    [[nodiscard]] static http_request_rejection rate_limited(rate_limit_rejection rejection) noexcept;

    [[nodiscard]] constexpr const http_error_info& error() const noexcept {
        return error_;
    }

    [[nodiscard]] constexpr const rate_limit_rejection* rate_limit() const noexcept {
        return rate_limit_ ? &*rate_limit_ : nullptr;
    }

private:
    constexpr http_request_rejection(http_error_info error, std::optional<rate_limit_rejection> rate_limit) noexcept
        : error_(error),
          rate_limit_(rate_limit) {}

    http_error_info error_;
    std::optional<rate_limit_rejection> rate_limit_;
};

// Answers a rejection through the application's error handler, adding the
// rate-limit headers when that is why the request was refused.
[[nodiscard]] task<http_response> respond_http_request_rejection(const route_table& routes, const http_request& request,
    request_memory& memory, const http_request_rejection& rejection, context_services services);

// Result of the synchronous first preparation pass. ready() is null only when
// a blocking-pool encode or the application error handler must be awaited
// through http_response_tail::finish_prepare().
class http_response_preparation final {
public:
    [[nodiscard]] const prepared_application_response* ready() const noexcept {
        return ready_ ? &*ready_ : nullptr;
    }

private:
    friend class http_response_tail;

    explicit http_response_preparation(prepared_application_response ready) noexcept
        : ready_(ready),
          first_pass_{http_response_compression_result::make_not_applicable(), false} {}
    explicit http_response_preparation(http_buffered_compression_step first_pass) noexcept
        : first_pass_(first_pass) {}

    std::optional<prepared_application_response> ready_;
    http_buffered_compression_step first_pass_;
};

// The post-response tail: final representation preparation (CORS,
// compression, 406/500 recovery through the application error handler) and
// the access-log record of the committed status.
class http_response_tail final {
public:
    http_response_tail(const http_request& request, http_response_coding_policy coding_policy,
        std::string_view client_address, std::chrono::steady_clock::time_point start) noexcept
        : request_(&request),
          coding_policy_(coding_policy),
          client_address_(client_address),
          start_(start) {}

    [[nodiscard]] const http_response_coding_policy& coding_policy() const noexcept {
        return coding_policy_;
    }

    // A generated terminal response must not inherit the negotiated coding
    // promise (and must not be transformed again).
    void disable_response_coding() noexcept {
        coding_policy_ = http_response_coding_policy::disabled();
    }

    // All arguments borrow the current request owner through completion. Only
    // an uncommitted response may enter. The common response (no blocking-pool
    // encode, no recovery) completes here without a coroutine frame.
    [[nodiscard]] http_response_preparation prepare(
        http_response& response, const http_server_options& options) const;

    // Completes a preparation whose ready() is null, with the arguments given
    // to prepare(). The application error handler runs at most once here.
    [[nodiscard]] task<prepared_application_response> finish_prepare(const http_response_preparation& preparation,
        http_response& response, const http_server_options& options, const route_table& routes,
        request_memory& memory, const context_services& services) const;

    // Records a terminal outcome whose final status was committed. A single
    // null check when no access log is configured.
    void record_access(const access_log_sink& access_log, http_status_code status) const noexcept;

private:
    const http_request* request_;
    http_response_coding_policy coding_policy_;
    std::string_view client_address_;
    std::chrono::steady_clock::time_point start_;
};

enum class http_request_endpoint : std::uint8_t {
    // Rejections: answer rejection() through respond_http_request_rejection.
    rejection,
    not_acceptable,
    // Admitted endpoints (resolved() is non-null for all but buffered).
    websocket,
    tunnel,
    response_stream,
    stream_body,
    buffered,
};

class http_request_admission final {
public:
    [[nodiscard]] http_request_endpoint endpoint() const noexcept {
        return endpoint_;
    }

    [[nodiscard]] const http_request_rejection* rejection() const noexcept {
        return rejection_ ? &*rejection_ : nullptr;
    }

    // Null for unresolved requests (404/405/OPTIONS/document root).
    [[nodiscard]] const resolved_route* resolved() const noexcept {
        return resolved_;
    }

    // CONNECT-UDP (RFC 9298) tunnel; meaningful for endpoint() == tunnel.
    [[nodiscard]] bool udp_tunnel() const noexcept {
        return udp_tunnel_;
    }

    [[nodiscard]] http_response_coding_availability coding_availability() const noexcept {
        return coding_availability_;
    }

    // The connection's request services plus the armed request deadline.
    [[nodiscard]] const context_services& services() const noexcept {
        return services_;
    }

    [[nodiscard]] protocol_byte_limit body_limit() const noexcept {
        return body_limit_;
    }

    [[nodiscard]] static_file_selection_mode static_file_mode() const noexcept {
        return services_.precompressed_static_files() ? static_file_selection_mode::precompressed
                                                      : static_file_selection_mode::identity_only;
    }

    [[nodiscard]] http_response_tail& tail() noexcept {
        return tail_;
    }
    [[nodiscard]] const http_response_tail& tail() const noexcept {
        return tail_;
    }

private:
    friend http_request_admission admit_http_request(const http_request_admission_input& input,
        const http_request_admission_policy& policy, context_services services,
        std::optional<request_deadline>& deadline);

    http_request_admission(http_response_tail tail, context_services services) noexcept
        : tail_(tail),
          services_(std::move(services)) {}

    http_response_tail tail_;
    context_services services_;
    std::optional<http_request_rejection> rejection_;
    const resolved_route* resolved_{};
    protocol_byte_limit body_limit_{protocol_byte_limit::unlimited()};
    http_response_coding_availability coding_availability_{http_response_coding_availability::identity_only};
    http_request_endpoint endpoint_{http_request_endpoint::buffered};
    bool udp_tunnel_{false};
};

// Runs the admission policy once for one request. `services` are the
// connection's request services (interim output, trailers, ...). The handler
// deadline is armed before any rejection so a custom error handler observes
// the request stop token: `deadline` must be disengaged and is emplaced on
// services.get_stop_token() only when a deadline applies.
[[nodiscard]] http_request_admission admit_http_request(const http_request_admission_input& input,
    const http_request_admission_policy& policy, context_services services, std::optional<request_deadline>& deadline);

}  // namespace ruvia::detail
