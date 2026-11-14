#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "context_access.h"
#include "http_content_negotiation.h"
#include "http_cors.h"
#include "http_error.h"
#include "http_protocol.h"
#include "http_request.h"
#include "http_response.h"
#include "io_operation.h"
#include "ip_address.h"
#include "middleware.h"
#include "rate_limit.h"
#include "response_coding_policy.h"
#include "route_table.h"
#include "server_options.h"
#include "static_root_index.h"
#include "task.h"
#include "worker_ref.h"

// Shared admission for HTTP/1 and in-process subrequests. The HTTP/1 session
// reads and frames messages; admission decides coding, client identity,
// expectations, rate limits, body ceilings and endpoint kind.

namespace ruvia::detail {

class http1_deferred_access_log;

// The deployment limits that shape admission. Servers build it from their
// options; a subrequest builds it from its parent (no rate limit and no
// target-origin check: the parent request already passed both).
struct http_request_admission_policy final {
    std::optional<std::size_t> max_stream_body_bytes_{};
    std::size_t max_buffered_body_bytes_{};
    bool apply_rate_limit_{true};
    // RFC 9110 §7.4: answer 421 when the connection cannot carry the target
    // (decide_http_target_origin against the services' transport facts).
    bool verify_target_origin_{true};
    [[nodiscard]] static http_request_admission_policy server(const http_server_options& options) noexcept;
};

struct http_request_admission_input final {
    const http_request& request_;
    const route_resolution& resolution_;
    // The parser or subrequest supplies the response-coding selection.
    http_response_coding_selection coding_;
    // Request arena for copied rejection diagnostics.
    // The protocol's Expect plan, when it defers the rejection to Web.
    const http_server_expectation_plan* expectation_{};
    // Start of the request for the access-log duration.
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
    // The arrival instant rate-limit decisions use, pinned by an owner that
    // replays requests at chosen instants (test_app); empty reads the steady
    // clock at each decision.
    std::optional<std::chrono::steady_clock::time_point> pinned_arrival_{};
};

// Effective request-body ceiling. A route may only tighten the deployment limit.
[[nodiscard]] protocol_byte_limit http_request_body_limit(const route_resolution& resolution, request_body_mode mode, const http_request_admission_policy& policy) noexcept;

// A server-layer rejection decided before any handler ran. The diagnostic is
// borrowed from static storage or the request arena.
class http_request_rejection final {
public:
    [[nodiscard]] static constexpr http_request_rejection error(http_error_info error) noexcept {
        return http_request_rejection(error, std::nullopt);
    }

    [[nodiscard]] static http_request_rejection rate_limited(rate_limit_rejection rejection) noexcept;

    [[nodiscard]] constexpr const http_error_info& error() const& noexcept RUVIA_LIFETIMEBOUND {
        return error_;
    }
    const http_error_info& error() const&& = delete;

    [[nodiscard]] constexpr const rate_limit_rejection* rate_limit() const& noexcept RUVIA_LIFETIMEBOUND {
        return rate_limit_ ? &*rate_limit_ : nullptr;
    }
    const rate_limit_rejection* rate_limit() const&& = delete;

private:
    constexpr http_request_rejection(http_error_info error, std::optional<rate_limit_rejection> rate_limit) noexcept :
        error_(error),
        rate_limit_(rate_limit) {}

    http_error_info error_;
    std::optional<rate_limit_rejection> rate_limit_;
};

// Answers a rejection through the application's error handler, adding the
// rate-limit headers when that is why the request was refused. The lazy task
// owns this snapshot; its diagnostic text still borrows static/request storage.
[[nodiscard]] task<http_response> respond_http_request_rejection(const route_table& routes, const http_request& request, http_request_rejection rejection, const context_services& services);

// Result of the synchronous first preparation pass. ready() is null only when
// a blocking-pool encode or the application error handler must be awaited
// through http_response_tail::finish_prepare().
class http_response_preparation final {
public:
    [[nodiscard]] const prepared_application_response* ready() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<prepared_application_response>(&state_);
    }
    const prepared_application_response* ready() const&& = delete;

    http_response_preparation(const http_response_preparation&) = default;
    http_response_preparation(http_response_preparation&&) = default;
    http_response_preparation& operator=(const http_response_preparation&) = delete;
    http_response_preparation& operator=(http_response_preparation&&) = delete;

private:
    friend class http_response_tail;

    struct deferred_type final {
        http_response_coding_policy policy_;
        buffered_response_recovery recovery_;
        // Without a saved recovery step, encode on the blocking pool first.
        std::optional<buffered_response_recovery_step> pending_step_;
    };

    explicit http_response_preparation(prepared_application_response ready) noexcept :
        state_(ready) {}
    explicit http_response_preparation(http_response_coding_policy policy) noexcept :
        state_(deferred_type{policy, {}, std::nullopt}) {}
    http_response_preparation(http_response_coding_policy policy, buffered_response_recovery recovery, buffered_response_recovery_step pending_step) noexcept :
        state_(deferred_type{policy, recovery, pending_step}) {}

    std::variant<prepared_application_response, deferred_type> state_;
};

// The post-response tail: final representation preparation (CORS,
// compression, 406/500 recovery through the application error handler) and
// the access-log record of the committed status.
class http_response_tail final {
public:
    http_response_tail(const http_request& request, http_response_coding_policy coding_policy, ip_address remote_address, ip_address client_address, std::chrono::steady_clock::time_point start) noexcept :
        request_(&request),
        coding_policy_(coding_policy),
        remote_address_(remote_address),
        client_address_(client_address),
        start_(start) {}

    [[nodiscard]] const http_response_coding_policy& coding_policy() const& noexcept RUVIA_LIFETIMEBOUND {
        return coding_policy_;
    }
    const http_response_coding_policy& coding_policy() const&& = delete;

    [[nodiscard]] request_io_metrics& io_metrics() const& noexcept RUVIA_LIFETIMEBOUND {
        return io_metrics_;
    }
    request_io_metrics& io_metrics() const&& = delete;

    // A generated terminal response must not inherit the negotiated coding
    // promise (and must not be transformed again).
    void disable_response_coding() noexcept {
        coding_policy_ = http_response_coding_policy::disabled();
    }
    void bind_addresses(ip_address remote_address, ip_address client_address) noexcept {
        remote_address_ = remote_address;
        client_address_ = client_address;
    }
    // The listener's Alt-Svc value (RFC 7838 §3), added to every prepared
    // response that does not carry its own; it borrows listener storage.
    void bind_alternative_service(std::string_view value) noexcept {
        alternative_service_ = value;
    }
    // The CORS policy the response streams and buffered responses of this
    // request apply; null when none does.
    [[nodiscard]] static const http_cors_policy* cors(const http_server_options& options) noexcept {
        return options.cors_.has_value() ? &*options.cors_ : nullptr;
    }

    // All arguments borrow the current request owner through completion. Only
    // an uncommitted response may enter. The common response (no blocking-pool
    // encode, no recovery) completes here without a coroutine frame.
    [[nodiscard]] http_response_preparation prepare(http_response& response, const http_server_options& options, const worker_ref& worker) const;

    // Completes a preparation whose ready() is null, with the arguments given
    // to prepare(). The application error handler runs at most once here.
    [[nodiscard]] task<prepared_application_response> finish_prepare(const http_response_preparation& preparation, http_response& response, const http_server_options& options, const route_table& routes, const context_services& services) const;

    [[nodiscard]] task<prepared_application_response> prepare_or_finish(http_response& response, const http_server_options& options, const worker_ref& worker, const route_table& routes, const context_services& services) const {
        const auto first = prepare(response, options, worker);
        if (const auto* ready = first.ready()) {
            co_return *ready;
        }
        co_return co_await finish_prepare(first, response, options, routes, services);
    }

    [[nodiscard]] task<prepared_application_response> prepare_or_finish(http_response& response, const http_server_options& options, const route_table& routes, const context_services& services) const {
        return prepare_or_finish(response, options, services.worker(), routes, services);
    }

    // Records a terminal outcome whose final status was committed. A single
    // null check when no access log is configured.
    void record_access(const access_log_sink& access_log, http_status_code status) const noexcept;

    // Keeps the record of a pipelined response held back for a coalesced
    // write whose head completes at output offset `head_end`; the log reports
    // it once that write commits the head. A no-op without an access log.
    void defer_access(const access_log_sink& access_log, http1_deferred_access_log& deferred, std::size_t head_end, http_status_code status) const;

private:
    const http_request* request_;
    http_response_coding_policy coding_policy_;
    ip_address remote_address_{};
    ip_address client_address_{};
    std::chrono::steady_clock::time_point start_;
    std::string_view alternative_service_;
    mutable request_io_metrics io_metrics_{};
};

enum class http_request_endpoint : std::uint8_t {
    // Rejections: answer rejection() through respond_http_request_rejection.
    rejection,
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

    [[nodiscard]] const http_request_rejection* rejection() const& noexcept RUVIA_LIFETIMEBOUND {
        return rejection_ ? &*rejection_ : nullptr;
    }
    const http_request_rejection* rejection() const&& = delete;

    // Null for unresolved requests (404/405/OPTIONS/document root).
    [[nodiscard]] const resolved_route* resolved() const& noexcept RUVIA_LIFETIMEBOUND {
        return resolved_;
    }
    const resolved_route* resolved() const&& = delete;

    // CONNECT-UDP (RFC 9298) tunnel; meaningful for endpoint() == tunnel.
    [[nodiscard]] bool udp_tunnel() const noexcept {
        return udp_tunnel_;
    }

    // The connection's request services after applying admission policy.
    [[nodiscard]] const context_services& services() const& noexcept RUVIA_LIFETIMEBOUND {
        return services_;
    }
    const context_services& services() const&& = delete;

    [[nodiscard]] protocol_byte_limit body_limit() const noexcept {
        return body_limit_;
    }

    [[nodiscard]] static_file_selection_mode static_file_mode() const noexcept {
        return services_.precompressed_static_files() ? static_file_selection_mode::precompressed : static_file_selection_mode::identity_only;
    }

    [[nodiscard]] http_response_tail& tail() & noexcept RUVIA_LIFETIMEBOUND {
        return tail_;
    }
    http_response_tail& tail() && = delete;

    http_request_admission(const http_request_admission& other) noexcept :
        tail_(other.tail_),
        services_(other.services_),
        rejection_(other.rejection_),
        resolved_(other.resolved_),
        body_limit_(other.body_limit_),
        endpoint_(other.endpoint_),
        udp_tunnel_(other.udp_tunnel_) {
        if (services_.io_metrics() == &other.tail_.io_metrics()) {
            services_.bind_io_metrics(&tail_.io_metrics());
        }
    }

    http_request_admission& operator=(const http_request_admission& other) noexcept {
        if (this != &other) {
            tail_ = other.tail_;
            services_ = other.services_;
            if (services_.io_metrics() == &other.tail_.io_metrics()) {
                services_.bind_io_metrics(&tail_.io_metrics());
            }
            rejection_ = other.rejection_;
            resolved_ = other.resolved_;
            body_limit_ = other.body_limit_;
            endpoint_ = other.endpoint_;
            udp_tunnel_ = other.udp_tunnel_;
        }
        return *this;
    }

    http_request_admission(http_request_admission&& other) noexcept :
        tail_(std::move(other.tail_)),
        services_(other.services_),
        rejection_(std::move(other.rejection_)),
        resolved_(other.resolved_),
        body_limit_(other.body_limit_),
        endpoint_(other.endpoint_),
        udp_tunnel_(other.udp_tunnel_) {
        if (services_.io_metrics() == &other.tail_.io_metrics()) {
            services_.bind_io_metrics(&tail_.io_metrics());
        }
    }

    http_request_admission& operator=(http_request_admission&& other) noexcept {
        if (this != &other) {
            tail_ = std::move(other.tail_);
            services_ = other.services_;
            if (services_.io_metrics() == &other.tail_.io_metrics()) {
                services_.bind_io_metrics(&tail_.io_metrics());
            }
            rejection_ = std::move(other.rejection_);
            resolved_ = other.resolved_;
            body_limit_ = other.body_limit_;
            endpoint_ = other.endpoint_;
            udp_tunnel_ = other.udp_tunnel_;
        }
        return *this;
    }

private:
    friend http_request_admission admit_http_request(const http_request_admission_input& input, const http_request_admission_policy& policy, const context_services& services);

    http_request_admission(http_response_tail tail, const context_services& services) noexcept :
        tail_(tail),
        services_(services) {
        if (services_.io_metrics() == nullptr) {
            services_.bind_io_metrics(&tail_.io_metrics());
        }
    }
    http_response_tail tail_;
    context_services services_;
    std::optional<http_request_rejection> rejection_;
    const resolved_route* resolved_{};
    protocol_byte_limit body_limit_{protocol_byte_limit::unlimited()};
    http_request_endpoint endpoint_{http_request_endpoint::buffered};
    bool udp_tunnel_{false};
};

// Runs the admission policy once for one request. `services` are the
// connection's request services (stop token, interim output, trailers, ...).
// Admission borrows the input only during this synchronous call and owns its
// request-local service view; binding it never changes the connection's view.
[[nodiscard]] http_request_admission admit_http_request(const http_request_admission_input& input, const http_request_admission_policy& policy, const context_services& services);

// Admits a buffered in-memory request and dispatches it through the route
// table or the rejection handler.
[[nodiscard]] task<http_request_admission> admit_and_dispatch_buffered_request(const route_table& routes, const http_request_admission_input& input, const http_request_admission_policy& policy, const context_services& services, http_response& response);

}  // namespace ruvia::detail
