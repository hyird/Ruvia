#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include "ruvia/core/task.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/web/error.h"

#include "context/context_access.h"
#include "http/inline_http_header_views.h"
#include "http/static_root_index.h"
#include "integration/operation_interrupt.h"
#include "router/route_table.h"
#include "server/http_server_options.h"

// Shared admission for HTTP/1 and in-process subrequests. The HTTP/1 session
// reads and frames messages; admission decides coding, client identity,
// expectations, rate limits, body ceilings and endpoint kind.

#include <algorithm>
#include <array>
#include <coroutine>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/write.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/bytes.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_content_encoder.h"
#include "ruvia/http/http_expectations.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/http/websocket_protocol.h"
#include "ruvia/web/context.h"
#include "ruvia/web/request_stream.h"
#include "ruvia/web/server_config.h"
#include "ruvia/web/streaming.h"

#include "http/http_tunnel_session.h"
#include "http/http_websocket_connection.h"
#include "server/connection_lifecycle.h"

namespace ruvia::detail {

class http1_deferred_access_log;
class route_table;

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

    [[nodiscard]] constexpr const http_error_info& error() const& noexcept RUVIA_LIFETIMEBOUND {
        return error_;
    }
    const http_error_info& error() const&& = delete;

    [[nodiscard]] constexpr const rate_limit_rejection* rate_limit() const& noexcept RUVIA_LIFETIMEBOUND {
        return rate_limit_ ? &*rate_limit_ : nullptr;
    }
    const rate_limit_rejection* rate_limit() const&& = delete;

private:
    constexpr http_request_rejection(http_error_info error, std::optional<rate_limit_rejection> rate_limit) noexcept
        : error_(error),
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

    explicit http_response_preparation(prepared_application_response ready) noexcept
        : state_(ready) {}
    explicit http_response_preparation(http_response_coding_policy policy) noexcept
        : state_(deferred_type{policy, {}, std::nullopt}) {}
    http_response_preparation(http_response_coding_policy policy, buffered_response_recovery recovery,
        buffered_response_recovery_step pending_step) noexcept
        : state_(deferred_type{policy, recovery, pending_step}) {}

    std::variant<prepared_application_response, deferred_type> state_;
};

// The post-response tail: final representation preparation (CORS,
// compression, 406/500 recovery through the application error handler) and
// the access-log record of the committed status.
class http_response_tail final {
public:
    http_response_tail(const http_request& request, http_response_coding_policy coding_policy,
        ip_address remote_address, ip_address client_address,
        std::chrono::steady_clock::time_point start) noexcept
        : request_(&request),
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
    [[nodiscard]] http_response_preparation prepare(
        http_response& response, const http_server_options& options, const worker_ref& worker) const;

    // Completes a preparation whose ready() is null, with the arguments given
    // to prepare(). The application error handler runs at most once here.
    [[nodiscard]] task<prepared_application_response> finish_prepare(const http_response_preparation& preparation,
        http_response& response, const http_server_options& options, const route_table& routes, const context_services& services) const;

    [[nodiscard]] task<prepared_application_response> prepare_or_finish(http_response& response,
        const http_server_options& options, const worker_ref& worker, const route_table& routes, const context_services& services) const {
        const auto first = prepare(response, options, worker);
        if (const auto* ready = first.ready()) {
            co_return *ready;
        }
        co_return co_await finish_prepare(first, response, options, routes, services);
    }

    [[nodiscard]] task<prepared_application_response> prepare_or_finish(http_response& response,
        const http_server_options& options, const route_table& routes,
        const context_services& services) const {
        return prepare_or_finish(response, options, services.worker(), routes, services);
    }

    // Records a terminal outcome whose final status was committed. A single
    // null check when no access log is configured.
    void record_access(const access_log_sink& access_log, http_status_code status) const noexcept;

    // Keeps the record of a pipelined response held back for a coalesced
    // write whose head completes at output offset `head_end`; the log reports
    // it once that write commits the head. A no-op without an access log.
    void defer_access(const access_log_sink& access_log, http1_deferred_access_log& deferred, std::size_t head_end,
        http_status_code status) const;

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
        return services_.precompressed_static_files() ? static_file_selection_mode::precompressed
                                                      : static_file_selection_mode::identity_only;
    }

    [[nodiscard]] http_response_tail& tail() & noexcept RUVIA_LIFETIMEBOUND {
        return tail_;
    }
    http_response_tail& tail() && = delete;

    http_request_admission(const http_request_admission& other) noexcept
        : tail_(other.tail_),
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

    http_request_admission(http_request_admission&& other) noexcept
        : tail_(std::move(other.tail_)),
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
    friend http_request_admission admit_http_request(const http_request_admission_input& input,
        const http_request_admission_policy& policy, const context_services& services);

    http_request_admission(http_response_tail tail, const context_services& services) noexcept
        : tail_(tail),
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
[[nodiscard]] http_request_admission admit_http_request(const http_request_admission_input& input,
    const http_request_admission_policy& policy, const context_services& services);

// Admits a buffered in-memory request and dispatches it through the route
// table or the rejection handler.
[[nodiscard]] task<http_request_admission> admit_and_dispatch_buffered_request(const route_table& routes,
    const http_request_admission_input& input, const http_request_admission_policy& policy,
    const context_services& services, http_response& response);

}  // namespace ruvia::detail

namespace ruvia {
class context;  // only used as context* in a type-erased bind thunk; web supplies the definition
}

namespace ruvia::detail {

// A normal response writer may reject a tunnel but may never establish one.
// Establishment transfers ownership through the protocol's acceptance driver.
inline void validate_response_stream_status(const http_request& request, const http_response& response) {
    if (request.known_method() == http_known_method::connect && response.status().is_successful()) {
        throw std::logic_error("a 2xx response to CONNECT establishes the tunnel; use accept_tunnel()");
    }
    if (response.status() == http_status::switching_protocols) {
        throw std::logic_error("a 101 response requires accept_tunnel() or accept_websocket()");
    }
}

// Plain forwarding functions, not coroutines: each returns the sink's own task,
// so a streamed chunk costs exactly the sink's frame and no wrapper frame.
template <typename sink>
task<stream_commit_result> response_stream_commit_thunk(void* target) {
    return static_cast<sink*>(target)->commit();
}

template <typename sink>
task<void> response_stream_write_thunk(void* target, std::string_view chunk) {
    return static_cast<sink*>(target)->write(chunk);
}

template <typename sink>
task<void> response_stream_end_thunk(void* target, std::span<const http_header_view> trailers) {
    return static_cast<sink*>(target)->end(trailers);
}

template <typename sink>
task<timer_sleep_result> response_stream_sleep_thunk(
    void* target, std::chrono::milliseconds duration, const stop_token& stop_token_value) {
    return static_cast<sink*>(target)->sleep(duration, stop_token_value);
}

template <typename sink>
bool response_stream_aborted_thunk(void* target) noexcept {
    return static_cast<sink*>(target)->aborted();
}

template <typename sink>
void response_stream_bind_context_thunk(void* target, context* context_value) {
    static_cast<sink*>(target)->bind_context(context_value);
}

template <typename sink>
void response_stream_release_context_thunk(void* target) noexcept {
    static_cast<sink*>(target)->release_context();
}

template <typename sink>
bool response_stream_committed_thunk(void* target) noexcept {
    return static_cast<sink*>(target)->committed();
}

template <typename sink>
[[nodiscard]] response_stream_writer make_response_stream_writer(sink& sink_value) noexcept {
    return streaming_access::make_response_stream_writer(&sink_value, sink_value.output_interrupts(), &response_stream_commit_thunk<sink>, &response_stream_write_thunk<sink>,
        &response_stream_end_thunk<sink>, &response_stream_sleep_thunk<sink>,
        &response_stream_bind_context_thunk<sink>, &response_stream_release_context_thunk<sink>,
        &response_stream_committed_thunk<sink>, &response_stream_aborted_thunk<sink>);
}

class response_stream_completed final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class response_stream_dispatch_result;

    explicit constexpr response_stream_completed(http_status_code status) noexcept
        : status_(status) {}

    http_status_code status_;
};

class response_stream_aborted_before_commit final {
private:
    friend class response_stream_dispatch_result;
    constexpr response_stream_aborted_before_commit() noexcept = default;
};

class response_stream_aborted_after_commit final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class response_stream_dispatch_result;
    explicit constexpr response_stream_aborted_after_commit(http_status_code status) noexcept
        : status_(status) {}
    http_status_code status_;
};

// The handler failed after its head was already on the wire. The status is what
// the peer was told; the exception is why it will never receive the rest. The
// status alone cannot be reported to anyone -- it says 200 -- so the exception
// travels with it to the transport, which is the last owner able to report it.
class response_stream_failed_after_commit final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    // Never null.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return exception_;
    }

private:
    friend class response_stream_dispatch_result;

    response_stream_failed_after_commit(http_status_code status, std::exception_ptr exception) noexcept
        : status_(status),
          exception_(std::move(exception)) {}

    http_status_code status_;
    std::exception_ptr exception_;
};

class response_stream_route_response final {
public:
    [[nodiscard]] http_response take_response() && noexcept {
        return std::move(response_);
    }

private:
    friend class response_stream_dispatch_result;

    explicit response_stream_route_response(http_response response) noexcept
        : response_(std::move(response)) {}

    http_response response_;
};

class response_stream_recovered_failure final {
public:
    [[nodiscard]] http_response take_response() && noexcept {
        return std::move(response_);
    }

private:
    friend class response_stream_dispatch_result;

    explicit response_stream_recovered_failure(http_response response) noexcept
        : response_(std::move(response)) {}

    http_response response_;
};

// Each route terminal is one alternative. Commit-bearing alternatives own the
// exact wire status; response-bearing alternatives own the movable response.
// No separate outcome discriminator is needed.
class response_stream_dispatch_result final {
public:
    [[nodiscard]] static response_stream_dispatch_result make_completed(
        http_status_code status) noexcept {
        return response_stream_dispatch_result(response_stream_completed(status));
    }

    [[nodiscard]] static response_stream_dispatch_result make_aborted_before_commit() noexcept {
        return response_stream_dispatch_result(response_stream_aborted_before_commit{});
    }

    [[nodiscard]] static response_stream_dispatch_result make_aborted_after_commit(http_status_code status) noexcept {
        return response_stream_dispatch_result(response_stream_aborted_after_commit(status));
    }

    [[nodiscard]] static response_stream_dispatch_result make_failed_after_commit(
        http_status_code status, std::exception_ptr exception) noexcept {
        return response_stream_dispatch_result(
            response_stream_failed_after_commit(status, std::move(exception)));
    }

    [[nodiscard]] static response_stream_dispatch_result make_route_response(
        http_response response) noexcept {
        return response_stream_dispatch_result(response_stream_route_response(std::move(response)));
    }

    [[nodiscard]] static response_stream_dispatch_result make_recovered_failure(
        http_response response) noexcept {
        return response_stream_dispatch_result(response_stream_recovered_failure(std::move(response)));
    }

    [[nodiscard]] const response_stream_completed* completed() const& noexcept {
        return std::get_if<response_stream_completed>(&value_);
    }
    const response_stream_completed* completed() const&& = delete;

    [[nodiscard]] const response_stream_aborted_before_commit* aborted_before_commit() const& noexcept {
        return std::get_if<response_stream_aborted_before_commit>(&value_);
    }
    const response_stream_aborted_before_commit* aborted_before_commit() const&& = delete;

    [[nodiscard]] const response_stream_aborted_after_commit* aborted_after_commit() const& noexcept {
        return std::get_if<response_stream_aborted_after_commit>(&value_);
    }
    const response_stream_aborted_after_commit* aborted_after_commit() const&& = delete;

    [[nodiscard]] const response_stream_failed_after_commit* failed_after_commit() const& noexcept {
        return std::get_if<response_stream_failed_after_commit>(&value_);
    }
    const response_stream_failed_after_commit* failed_after_commit() const&& = delete;

    [[nodiscard]] response_stream_route_response* route_response() & noexcept {
        return std::get_if<response_stream_route_response>(&value_);
    }
    response_stream_route_response* route_response() && = delete;

    [[nodiscard]] response_stream_recovered_failure* recovered_failure() & noexcept {
        return std::get_if<response_stream_recovered_failure>(&value_);
    }
    response_stream_recovered_failure* recovered_failure() && = delete;

    [[nodiscard]] std::optional<http_status_code> committed_status() const noexcept {
        if (const auto* value = completed()) {
            return value->status();
        }
        if (const auto* value = aborted_after_commit()) {
            return value->status();
        }
        if (const auto* value = failed_after_commit()) {
            return value->status();
        }
        return std::nullopt;
    }

private:
    using value_type = std::variant<response_stream_completed, response_stream_aborted_before_commit,
        response_stream_aborted_after_commit, response_stream_failed_after_commit,
        response_stream_route_response, response_stream_recovered_failure>;

    template <typename alternative_type>
    explicit response_stream_dispatch_result(alternative_type alternative) noexcept
        : value_(std::move(alternative)) {}

    value_type value_;
};

// Only called once sink_value.committed() holds.
template <typename sink>
[[nodiscard]] http_status_code committed_response_stream_status(const sink& sink_value) {
    return sink_value.commit_plan()->response_status();
}

struct response_stream_no_peer_abort final {
    [[nodiscard]] constexpr bool operator()() const noexcept {
        return false;
    }
};

// Drives a response-stream route over an already-constructed sink. Connection
// persistence remains a transport concern when a buffered response is returned.
template <typename sink, typename peer_aborted_type = response_stream_no_peer_abort>
task<response_stream_dispatch_result> dispatch_response_stream_with(sink& sink_value, const route_table& routes_value,
    const http_request& request, const resolved_route& route,
    const context_services& services, peer_aborted_type peer_aborted = {}) {
    auto response_stream = make_response_stream_writer(sink_value);

    std::exception_ptr exception;
    try {
        auto result_value = co_await routes_value.dispatch_response_stream(
            request, route, response_stream, services);
        if (services.connection_aborted() || services.request_reset() || peer_aborted()) {
            if (!sink_value.committed()) {
                co_return response_stream_dispatch_result::make_aborted_before_commit();
            }
            co_return response_stream_dispatch_result::make_aborted_after_commit(
                committed_response_stream_status(sink_value));
        }
        if (!result_value.has_value()) {
            if (!sink_value.committed()) {
                throw std::logic_error("handled response stream has no committed protocol plan");
            }
            co_return response_stream_dispatch_result::make_completed(
                committed_response_stream_status(sink_value));
        }
        if (sink_value.committed()) {
            throw std::logic_error("buffered stream result followed a committed response head");
        }
        co_return response_stream_dispatch_result::make_route_response(std::move(*result_value));
    } catch (const inbound_transport_error&) {
        if (!services.request_reset() && (services.connection_aborted() || peer_aborted())) {
            if (!sink_value.committed()) {
                co_return response_stream_dispatch_result::make_aborted_before_commit();
            }
            co_return response_stream_dispatch_result::make_aborted_after_commit(
                committed_response_stream_status(sink_value));
        }
        exception = std::current_exception();
    } catch (...) {
        exception = std::current_exception();
    }
    if (services.connection_aborted() || services.request_reset()) {
        auto result = sink_value.committed()
                          ? response_stream_dispatch_result::make_aborted_after_commit(
                                committed_response_stream_status(sink_value))
                          : response_stream_dispatch_result::make_aborted_before_commit();
        // The transport is already closed, but a real callback failure still
        // belongs to its configured outlet. Snapshot before invoking user code.
        const auto failure = services.connection_failure();
        const auto remote = services.remote_address();
        failure.invoke(remote, exception);
        co_return result;
    }

    if (sink_value.committed()) {
        // Past the point of no return: the status cannot be changed and no
        // error body can be appended, so the exception is handed to the
        // transport rather than dropped with the connection.
        co_return response_stream_dispatch_result::make_failed_after_commit(
            committed_response_stream_status(sink_value), std::move(exception));
    }
    auto response = co_await routes_value.handle_exception(request, exception, services);
    co_return response_stream_dispatch_result::make_recovered_failure(std::move(response));
}

}  // namespace ruvia::detail

// listener_dispatch::request_stream: what a request stream connection resolves
// every request to in place of the route table, and the one run of the
// application's request_stream callback every transport reaches through it.

namespace ruvia::detail {

class route_entry;

// A request stream route runs no middleware and declares no body limit. A
// tunnel request -- CONNECT, extended CONNECT, or an HTTP/1.1 Upgrade without
// content -- resolves to an upgrade route whose callback commits the tunnel
// itself (accept_tunnel()); any other request to a response stream route with
// a streamed body.
[[nodiscard]] route_resolution resolve_request_stream_route(bool tunnel) noexcept;

// Whether `route` is the tunnel route resolve_request_stream_route() yields.
[[nodiscard]] bool is_request_stream_tunnel_route(const route_entry& route) noexcept;

// Whether an HTTP/1 request of a request stream connection is a tunnel
// request; `content` is whether it carries request content.
[[nodiscard]] bool http1_request_stream_tunnel(const http_request& request, bool content) noexcept;

// Runs the connection's callback for a tunnel request in the context its
// upgrade route built. `accept` commits the success head and establishes the
// tunnel; a refusal writes through the protocol's ordinary response stream,
// which the upgrade driver must bind before dispatching this context.
[[nodiscard]] task<void> serve_request_stream_tunnel(context& context_value, const request_stream_tunnel_accept& accept,
    const request_stream_websocket_accept* accept_websocket = nullptr);

}  // namespace ruvia::detail

namespace ruvia::detail {

// The single writer of every HTTP/1 final head that hands the connection to
// another protocol: a WebSocket or CONNECT-UDP 101 and a CONNECT 2xx. RFC 9110
// section 7.8: when the head is 101 (Switching Protocols) and the request
// carried Expect: 100-continue, the HTTP-owned
// http_request_expectations::continue_before_switching_protocols() decision
// puts a 100 (Continue) head before it in the same write, even when no content
// follows. Any other status is written alone. `head` is the complete encoded
// final head; it must stay valid until the returned task completes.
template <typename stream_type>
[[nodiscard]] task<std::error_code> write_http1_upgrade_response_head(stream_type& stream,
    const http_request_expectations& expectations, http_status_code status, std::string_view head,
    connection_scanner::entry& scanner_entry) {
    constexpr std::size_t continue_head_capacity = 64;
    std::array<char, continue_head_capacity> continue_bytes;
    std::string_view continue_head;
    if (status == http_status::switching_protocols && expectations.continue_before_switching_protocols()) {
        const auto prepared = prepare_http1_interim_response_head(
            http_interim_response_head(http_status::continue_value), std::span<char>(continue_bytes));
        const auto* prepared_head = std::get_if<prepared_http1_interim_response>(&prepared);
        if (prepared_head == nullptr) {
            throw std::logic_error("cannot prepare HTTP/1 100 Continue head");
        }
        continue_head = prepared_head->head();
    }
    const std::array<asio::const_buffer, 2> buffers{asio::buffer(continue_head), asio::buffer(head)};
    const connection_scanner::entry::write_scope write_stall_clock(scanner_entry);
    const auto written = co_await ruvia::async_asio<std::size_t>([&stream, &buffers, &scanner_entry](auto handler) {
        asio::async_write(stream, buffers, connection_write_progress(scanner_entry), std::move(handler));
    });
    co_return written.error_code();
}

// The context retains header storage through handshake construction. The
// protocol plan copies the views before this temporary pointer table is freed.
[[nodiscard]] inline inline_http_header_views<> websocket_response_headers(context& context_value) {
    return inline_http_header_views<>(
        context_access::handshake_headers(context_value));
}

// Flush the exact HTTP-owned 101 handshake prepared by the route. Negotiation is
// not recomputed and no compression bool is returned through a side channel.
// The shared HTTP/1 upgrade-head writer applies the request `expectations`
// (100 Continue before 101, RFC 9110 section 7.8).
template <typename stream_type>
task<std::error_code> write_websocket_handshake(stream_type& stream,
    const websocket_server_handshake& handshake, const http_request_expectations& expectations, connection_scanner::entry& scanner_entry) {
    http_response_head_buffer head;
    handshake.for_each_response_part([&](std::string_view part) {
        head.append(part);
    });
    co_return co_await write_http1_upgrade_response_head(
        stream, expectations, http_status::switching_protocols, head.view(), scanner_entry);
}

}  // namespace ruvia::detail

// The shared shape of every upgrade-capable route (CONNECT / CONNECT-UDP
// tunnels and WebSocket upgrades) across HTTP/1 and HTTP/2: run the
// application chain with a terminal that commits the upgrade and
// owns the resulting session; once a session exists it is always finished,
// otherwise a handler failure propagates and an uncommitted response falls
// back to the ordinary buffered tail.

namespace ruvia::detail {

[[nodiscard]] inline websocket_connection_options make_websocket_session_connection_options(
    const http_server_options& options, websocket_compression negotiated,
    const websocket_deflate_config& deflate, protocol_byte_limit receive_limit) noexcept {
    // Directions without context takeover borrow the worker's codecs, which
    // outlive every connection, so they allocate no codec state here.
    return websocket_connection_options{
        .receive_message_limit_ = receive_limit,
        .compression_ = negotiated,
        .deflater_ = deflate.deflater_,
        .shared_deflate_ = &options.codecs_->websocket_deflate(),
    };
}

// Selects the refusal response the server sends after the callback returns.
[[nodiscard]] inline websocket_rejection reject_request_stream_websocket(
    context& context_value, const websocket_handshake_failure& failure) {
    const auto error = failure.protocol_error();
    auto response = context_value.error(copy_http_protocol_error_info(error));
    failure.apply_required_response_headers(response);
    const websocket_rejection rejection{response.status(), error.code()};
    context_access::set_response(context_value, std::move(response));
    return rejection;
}

struct websocket_accept_access final {
    [[nodiscard]] static websocket_accept_result accepted(websocket& session) noexcept {
        return websocket_accept_result(session);
    }
    [[nodiscard]] static websocket_accept_result rejected(websocket_rejection rejection) noexcept {
        return websocket_accept_result(rejection);
    }
};

// `datagrams` is the route's http_tunnel_route_config::datagrams_, which
// CONNECT-UDP (`udp`) always sets.
[[nodiscard]] inline http_response prepare_tunnel_upgrade_response_head(
    context& context_value, http_protocol_version version, bool udp, bool datagrams) {
    auto head_response = context_access::streaming_head(context_value);
    if (udp) {
        auto negotiated = prepare_http_connect_udp_response(std::move(head_response), version);
        if (negotiated.index() != 0) {
            throw std::invalid_argument("invalid CONNECT-UDP response metadata");
        }
        head_response = std::move(std::get<0>(negotiated));
    } else if (datagrams && head_response.status().is_successful()) {
        // RFC 9297 section 3.4: the route's protocol uses the Capsule Protocol,
        // so its successful response says so with exactly one ?1.
        head_response.header_stable_view("Capsule-Protocol", "?1");
    }
    return head_response;
}

// `establish(context&) -> task<void>` commits the upgrade response head,
// emplaces `session` and runs the route's stream handler on it.
// `finish(session_type&, std::exception_ptr) -> task<void>` closes an
// established session and reports a handler failure; it always runs once a
// session exists, before any handler exception is surfaced.
// Returns a buffered response only when the application answered without
// upgrading; an established or abandoned upgrade returns std::nullopt.
template <typename session_type, typename establish_type, typename finish_type>
[[nodiscard]] task<std::optional<http_response>> run_http_upgrade_route(
    const route_table& routes, const http_request& request, const resolved_route& resolved,
    const context_services& services, std::optional<session_type>& session, establish_type& establish,
    finish_type& finish) {
    const auto terminal = callback_access::bind<task<void>(context&)>(establish);
    std::optional<http_response> buffered;
    std::exception_ptr failure;
    try {
        buffered = co_await routes.dispatch_upgrade(request, resolved, terminal, services);
    } catch (...) {
        failure = std::current_exception();
    }
    if (session.has_value()) {
        co_await finish(*session, failure);
        co_return std::nullopt;
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    co_return buffered;
}

template <typename stream_type>
class websocket_upgrade_commit final {
public:
    static constexpr bool native_stream = websocket_session_connection<stream_type>::native_stream;
    using stream_argument_type = typename websocket_session_connection<stream_type>::stream_argument_type;

    websocket_upgrade_commit(stream_argument_type stream, websocket_compression compression,
        std::string_view subprotocol)
        : stream_(hold_stream(std::forward<stream_argument_type>(stream))),
          compression_(compression) {
        try {
            subprotocol_.assign(subprotocol);
        } catch (...) {
            abort();
            throw;
        }
    }
    [[nodiscard]] stream_argument_type release_stream() noexcept {
        if constexpr (native_stream) {
            return *stream_;
        } else {
            return std::move(stream_);
        }
    }

    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] std::string&& release_subprotocol() noexcept {
        return std::move(subprotocol_);
    }

    void abort() noexcept {
        if constexpr (native_stream) {
            websocket_session_connection<stream_type>::close_stream(*stream_);
        } else {
            stream_.abort();
        }
    }

private:
    using held_stream_type = std::conditional_t<native_stream, stream_type*, stream_type>;

    [[nodiscard]] static held_stream_type hold_stream(stream_argument_type stream) noexcept(native_stream) {
        if constexpr (native_stream) {
            return &stream;
        } else {
            return std::move(stream);
        }
    }

    held_stream_type stream_;
    websocket_compression compression_{};
    std::string subprotocol_;
};

// A committed upgrade, or the refusal its handshake check selected.
template <typename stream_type>
using websocket_upgrade_attempt = std::variant<websocket_upgrade_commit<stream_type>, websocket_rejection>;

template <typename transport_type, typename commit_upgrade_type, typename commit_websocket_type>
struct tunnel_upgrade_session_operation final {
    using session_type = http_tunnel_session<transport_type>;

    const http_request& request_;
    const resolved_route& resolved_;
    const tunnel_route_endpoint& endpoint_;
    const context_services& services_;
    const http_server_options& options_;
    connection_scanner::entry& scanner_entry_;
    inbound_buffer_account& inbound_;
    bool udp_;
    std::string_view pending_;
    commit_upgrade_type& commit_upgrade_;
    commit_websocket_type& commit_websocket_;
    std::optional<session_type>& session_;
    http_tunnel_route_config config_{endpoint_.config()};
    std::optional<websocket_session_connection<transport_type>> websocket_session_{};
    struct websocket_borrow final {
        explicit websocket_borrow(websocket_session_connection<transport_type>& connection)
            : value_(make_websocket_facade(connection)) {}
        websocket value_;
    };
    std::optional<websocket_borrow> websocket_{};

    // Commits the upgrade head the context's draft describes and opens the
    // session over the transport that head hands over.
    [[nodiscard]] task<void> establish(context& context_value) {
        auto head_response = prepare_tunnel_upgrade_response_head(
            context_value, request_.protocol_version(), udp_, config_.datagrams_);
        std::optional<transport_type> transport;
        if constexpr (requires { commit_upgrade_(context_value, head_response).operator co_await(); }) {
            transport.emplace(co_await commit_upgrade_(context_value, head_response));
        } else {
            transport.emplace(commit_upgrade_(context_value, head_response));
        }
        try {
            session_.emplace(std::move(*transport), services_.worker(), &inbound_,
                config_.datagrams_, pending_);
        } catch (...) {
            transport->abort();
            throw;
        }
        // A synchronous commit leaves no co_await in this instantiation;
        // co_return keeps the body a coroutine for every transport.
        co_return;
    }

    [[nodiscard]] task<http_tunnel*> accept_request_stream_tunnel(context& context_value, http_tunnel_route_config config) {
        config_ = route_endpoint::validate_tunnel_config(
            !request_.protocol().empty() || request_.known_method() != http_known_method::connect, udp_, config);
        co_await establish(context_value);
        scanner_entry_.enter_long_lived();
        co_return &session_->tunnel();
    }

    [[nodiscard]] task<websocket_accept_result> accept_request_stream_websocket(
        context& context_value, const websocket_route_endpoint& websocket_endpoint) {
        const auto headers = websocket_response_headers(context_value);
        auto attempt = co_await commit_websocket_(context_value, websocket_endpoint, headers);
        if (const auto* rejection = std::get_if<websocket_rejection>(&attempt)) {
            co_return websocket_accept_access::rejected(*rejection);
        }
        auto* committed = std::get_if<websocket_upgrade_commit<transport_type>>(&attempt);
        try {
            websocket_session_.emplace(committed->release_stream(), services_.worker(), scanner_entry_,
                websocket_endpoint.lifecycle(),
                protocol_byte_limit::limited(options_.server_.max_websocket_message_bytes_),
                pending_,
                make_websocket_session_connection_options(options_, committed->compression(), websocket_endpoint.deflate(),
                    websocket_endpoint.message_limit(options_.server_.max_websocket_message_bytes_)),
                committed->release_subprotocol(), &inbound_);
        } catch (...) {
            committed->abort();
            throw;
        }
        websocket_.emplace(*websocket_session_);
        scanner_entry_.enter_long_lived();
        co_return websocket_accept_access::accepted(websocket_->value_);
    }

    [[nodiscard]] task<void> operator()(context& context_value) {
        if (is_request_stream_tunnel_route(resolved_.route())) {
            auto accept = [this, &context_value](http_status_code, http_tunnel_route_config config) {
                return accept_request_stream_tunnel(context_value, config);
            };
            const auto accept_ref = callback_access::bind<task<http_tunnel*>(http_status_code, http_tunnel_route_config)>(accept);
            auto accept_websocket = [this, &context_value](const websocket_route_endpoint& endpoint) {
                return accept_request_stream_websocket(context_value, endpoint);
            };
            const auto websocket_accept_ref =
                callback_access::bind<task<websocket_accept_result>(const websocket_route_endpoint&)>(accept_websocket);
            std::exception_ptr failure;
            try {
                co_await serve_request_stream_tunnel(context_value, accept_ref, &websocket_accept_ref);
            } catch (...) {
                failure = std::current_exception();
            }
            // Retire borrowed operations before draining the owned connection,
            // exactly as the route handler's stack facade does.
            websocket_.reset();
            if (websocket_session_) {
                co_await finish_websocket_session(*websocket_session_, failure,
                    options_.connection_failure_, services_.remote_address());
            } else if (failure) {
                std::rethrow_exception(failure);
            }
            co_return;
        }
        co_await establish(context_value);
        co_await invoke_tunnel_handler(
            *session_, scanner_entry_, resolved_.stream_handler(), context_value);
    }

    [[nodiscard]] task<void> operator()(session_type& session_value, std::exception_ptr failure) const {
        return finish_tunnel_session(session_value, std::move(failure),
            options_.connection_failure_, services_.remote_address(),
            scanner_entry_, config_.peer_transport_fin_timeout_);
    }
};

template <typename transport_type, typename commit_upgrade_type, typename commit_websocket_type>
[[nodiscard]] task<std::optional<http_response>> execute_tunnel_upgrade_session(
    const route_table& routes, const http_request& request, const resolved_route& resolved, const context_services& services, const http_server_options& options,
    connection_scanner::entry& scanner_entry, inbound_buffer_account& inbound, bool udp,
    std::string_view pending, commit_upgrade_type commit_upgrade, commit_websocket_type commit_websocket) {
    const auto& endpoint = *resolved.route().endpoint().tunnel();
    using operation_type = tunnel_upgrade_session_operation<transport_type, commit_upgrade_type, commit_websocket_type>;
    std::optional<typename operation_type::session_type> session;
    operation_type operation{
        .request_ = request,
        .resolved_ = resolved,
        .endpoint_ = endpoint,
        .services_ = services,
        .options_ = options,
        .scanner_entry_ = scanner_entry,
        .inbound_ = inbound,
        .udp_ = udp,
        .pending_ = pending,
        .commit_upgrade_ = commit_upgrade,
        .commit_websocket_ = commit_websocket,
        .session_ = session,
    };
    co_return co_await run_http_upgrade_route(
        routes, request, resolved, services, session, operation, operation);
}

template <typename stream_type, typename commit_upgrade_type>
struct websocket_upgrade_session_operation final {
    using session_type = websocket_session_connection<stream_type>;

    const resolved_route& resolved_;
    const websocket_route_endpoint& websocket_endpoint_;
    const context_services& services_;
    const http_server_options& options_;
    connection_scanner::entry& scanner_entry_;
    inbound_buffer_account& inbound_;
    std::string_view pending_frames_;
    commit_upgrade_type& commit_upgrade_;
    std::optional<session_type>& session_;

    [[nodiscard]] task<void> operator()(context& context_value) {
        const auto response_headers_value = websocket_response_headers(context_value);
        std::optional<websocket_upgrade_commit<stream_type>> committed;
        if constexpr (requires { commit_upgrade_(context_value, response_headers_value).operator co_await(); }) {
            committed.emplace(co_await commit_upgrade_(context_value, response_headers_value));
        } else {
            committed.emplace(commit_upgrade_(context_value, response_headers_value));
        }
        try {
            session_.emplace(committed->release_stream(), services_.worker(), scanner_entry_,
                websocket_endpoint_.lifecycle(),
                protocol_byte_limit::limited(options_.server_.max_websocket_message_bytes_),
                pending_frames_,
                make_websocket_session_connection_options(
                    options_, committed->compression(), websocket_endpoint_.deflate(),
                    websocket_endpoint_.message_limit(options_.server_.max_websocket_message_bytes_)),
                committed->release_subprotocol(), &inbound_);
        } catch (...) {
            committed->abort();
            throw;
        }
        co_await invoke_websocket_handler(
            *session_, scanner_entry_, resolved_.stream_handler(), context_value);
    }

    [[nodiscard]] task<void> operator()(session_type& session_value, std::exception_ptr failure) const {
        return finish_websocket_session(session_value, std::move(failure),
            options_.connection_failure_, services_.remote_address());
    }
};

template <typename stream_type, typename commit_upgrade_type>
[[nodiscard]] task<std::optional<http_response>> execute_websocket_upgrade_session(
    const route_table& routes, const http_request& request, const resolved_route& resolved, const context_services& services, const http_server_options& options,
    connection_scanner::entry& scanner_entry, inbound_buffer_account& inbound,
    std::string_view pending_frames, commit_upgrade_type commit_upgrade) {
    const auto& websocket_endpoint = *resolved.route().endpoint().websocket();
    using operation_type = websocket_upgrade_session_operation<stream_type, commit_upgrade_type>;
    std::optional<typename operation_type::session_type> session;
    operation_type operation{
        .resolved_ = resolved,
        .websocket_endpoint_ = websocket_endpoint,
        .services_ = services,
        .options_ = options,
        .scanner_entry_ = scanner_entry,
        .inbound_ = inbound,
        .pending_frames_ = pending_frames,
        .commit_upgrade_ = commit_upgrade,
        .session_ = session,
    };
    co_return co_await run_http_upgrade_route(
        routes, request, resolved, services, session, operation, operation);
}

}  // namespace ruvia::detail

namespace ruvia {

class context;

namespace detail {

class response_stream_state final {
public:
    [[nodiscard]] bool committed() const noexcept {
        return commit_plan() != nullptr;
    }

    [[nodiscard]] bool ended() const noexcept {
        return std::holds_alternative<ended_type>(state_);
    }

    [[nodiscard]] bool aborted() const noexcept {
        return std::holds_alternative<aborted_before_commit_type>(state_) ||
               std::holds_alternative<aborted_after_commit_type>(state_);
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept {
        if (const auto* value = std::get_if<body_open_type>(&state_)) {
            return &value->plan_;
        }
        if (const auto* value = std::get_if<ended_type>(&state_)) {
            return &value->plan_;
        }
        if (const auto* value = std::get_if<aborted_after_commit_type>(&state_)) {
            return &value->plan_;
        }
        return nullptr;
    }
    const http_response_stream_commit_plan* commit_plan() const&& = delete;

    void bind_context(context* context_value) {
        if (!std::holds_alternative<unbound_type>(state_)) {
            throw std::logic_error("response stream context is already bound");
        }
        if (context_value == nullptr) {
            throw std::invalid_argument("response stream context binding is incomplete");
        }
        state_.emplace<bound_type>(context_value);
    }

    void release_context() noexcept {
        if (std::holds_alternative<bound_type>(state_)) {
            state_.emplace<detached_type>();
        }
    }

    [[nodiscard]] context& bound_context() const {
        const auto* bound = std::get_if<bound_type>(&state_);
        if (bound == nullptr) {
            if (committed()) {
                throw std::logic_error("response stream is already committed");
            }
            throw std::logic_error("response stream context is not bound");
        }
        return *bound->context_;
    }

    void mark_committed(http_response_stream_commit_plan plan) {
        if (committed() || aborted()) {
            throw std::logic_error("response stream is already committed");
        }
        switch (plan.head_disposition()) {
            case http_response_stream_head_disposition::body_open:
                state_.emplace<body_open_type>(plan);
                break;
            case http_response_stream_head_disposition::message_ended:
                state_.emplace<ended_type>(plan);
                break;
        }
    }

    void mark_ended() {
        if (ended()) {
            return;
        }
        if (auto* value = std::get_if<body_open_type>(&state_)) {
            auto plan = value->plan_;
            state_.emplace<ended_type>(plan);
            return;
        }
        throw std::logic_error("response stream is not committed");
    }

    void mark_aborted() noexcept {
        if (aborted()) {
            return;
        }
        if (const auto* committed_plan = commit_plan()) {
            // A body-suppressed commit is already logically ended, but its head
            // can still fail during transport delivery. Preserve that commit.
            const auto plan = *committed_plan;
            state_.emplace<aborted_after_commit_type>(plan);
            return;
        }
        state_.emplace<aborted_before_commit_type>();
    }

    void ensure_body_allowed() const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        if (!std::holds_alternative<body_open_type>(state_)) {
            throw std::logic_error("response does not allow a stream body");
        }
    }

    // Trailers are optional metadata (RFC 9110 6.5): framing that cannot carry
    // them discards them, so only a status that can never carry content is a
    // handler error here. The caller writes trailers only if trailers_available().
    void ensure_trailers_allowed() const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        const auto* plan = commit_plan();
        if (plan == nullptr || !plan->status_permits_trailers()) {
            throw std::logic_error("response status does not permit trailers");
        }
    }

private:
    struct unbound_type final {};

    struct detached_type final {};

    struct bound_type final {
        explicit bound_type(context* bound_context) noexcept
            : context_(bound_context) {}

        context* context_;
    };

    struct body_open_type final {
        explicit body_open_type(http_response_stream_commit_plan commit_plan) noexcept
            : plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    struct ended_type final {
        explicit ended_type(http_response_stream_commit_plan commit_plan) noexcept
            : plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    struct aborted_before_commit_type final {};

    struct aborted_after_commit_type final {
        explicit aborted_after_commit_type(http_response_stream_commit_plan commit_plan) noexcept
            : plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    using state_type = std::variant<unbound_type, bound_type, detached_type, body_open_type, ended_type,
        aborted_before_commit_type, aborted_after_commit_type>;

    state_type state_;
};

}  // namespace detail
}  // namespace ruvia

namespace ruvia::detail {

// Owns the response-coding lifecycle of HTTP/1 and HTTP/2 stream sinks.
// The transport only delivers encoded slices; it never reconstructs whether a
// coding was selected, whether the body is suppressed, or whether an encoder
// exists. The encoder itself is borrowed from the worker's codecs for the
// stream's lifetime, not constructed per response.
class http_streaming_response_compression final {
public:
    // Application input encoded per step. It bounds the encoded buffer and the
    // codec work between two transport writes independently of the size of
    // one application write.
    static constexpr std::size_t input_slice_bytes = 16 * 1024;

    // `compression` is the server's compression policy, or nullptr when the
    // server compresses nothing.
    http_streaming_response_compression(worker_codecs& codecs,
        http_response_coding_selection selection, const compression_config* compression) noexcept
        : codecs_(codecs),
          selection_(selection),
          compression_(compression) {}
    http_streaming_response_compression(worker_codecs&&,
        http_response_coding_selection, const compression_config*) = delete;

    http_streaming_response_compression(const http_streaming_response_compression&) = delete;
    http_streaming_response_compression& operator=(const http_streaming_response_compression&) = delete;
    http_streaming_response_compression(http_streaming_response_compression&&) = delete;
    http_streaming_response_compression& operator=(http_streaming_response_compression&&) = delete;

    // Applies the representation headers before the protocol-owned stream head
    // is committed. A forbidden identity fallback, including a negotiation the
    // selection already reports as failed, is rejected here, while the encoder
    // itself is delayed until activate() has a final body plan.
    void prepare(const http_request& request, http_response& response) {
        if (!std::holds_alternative<unprepared_type>(state_)) {
            throw std::logic_error("streaming response compression is already prepared");
        }
        const auto request_method = request.known_method();
        const auto decision = prepare_response_compression(selection_, request_method, response,
            http_response_compression_source::stream,
            compression_ != nullptr ? http_response_coding_availability::identity_and_compression
                                    : http_response_coding_availability::identity_only);
        if (decision != http_response_compression_decision::encode || !stream_length_allows_encoding(response)) {
            if (http_response_coding_fallback_forbidden(selection_, request_method, response, &request)) {
                throw http_error(http_not_acceptable_coding_error());
            }
            state_.emplace<inactive_type>();
            return;
        }

        response.apply_content_encoding(http_content_coding_token(selection_.coding()));
        state_.emplace<pending_type>();
    }

    // Binds encoder ownership to the protocol body plan. Body-suppressed
    // responses retain their selected representation metadata but never create
    // an encoder for bytes that the protocol will not send.
    void activate(http_response_body_plan body_plan) {
        if (std::holds_alternative<inactive_type>(state_)) {
            return;
        }
        const auto* pending = std::get_if<pending_type>(&state_);
        if (pending == nullptr) {
            throw std::logic_error("streaming response compression is not pending");
        }
        if (body_plan.body_suppressed()) {
            state_.emplace<inactive_type>();
            return;
        }
        state_.emplace<active_type>(codecs_.acquire_stream_encoder(selection_.coding()));
    }

    [[nodiscard]] bool active() const noexcept {
        return std::holds_alternative<active_type>(state_);
    }

    // A stream-head transaction may prepare representation metadata before the
    // protocol layer accepts the head. If that later preparation fails, the
    // sink is terminal before any body byte can be retried; keep the encoder
    // lifecycle in the same terminal state instead of leaving pending/inactive
    // behind for a second commit attempt.
    void abort() noexcept {
        encoded_chunk_.clear();
        state_.emplace<failed_type>();
    }

    // The one slicing loop of both protocols: encodes one application write in
    // input_slice_bytes steps and hands every non-empty encoded slice to
    // `deliver` (the sink's framed write, returning task<void>). Only the last
    // slice flushes, so each application write ends with exactly one codec
    // flush, which keeps SSE events prompt without flushing every slice.
    template <typename deliver_type>
    task<void> write(std::string_view chunk, deliver_type deliver) {
        std::size_t offset = 0;
        while (offset < chunk.size()) {
            const auto count = (std::min)(input_slice_bytes, chunk.size() - offset);
            const auto slice = chunk.substr(offset, count);
            offset += count;
            encode_slice(slice, offset == chunk.size());
            if (!encoded_chunk_.empty()) {
                co_await deliver(std::string_view(encoded_chunk_));
            }
            release_output();
        }
    }

    void finish() {
        encoded_chunk_.clear();
        if (std::holds_alternative<finished_type>(state_)) {
            return;
        }
        auto* active_state = std::get_if<active_type>(&state_);
        if (active_state == nullptr) {
            throw std::logic_error("streaming response encoder is not active");
        }
        try {
            active_state->lease_.encoder().finish(encoded_chunk_);
            state_.emplace<finished_type>();
        } catch (...) {
            fail_encoder(*active_state);
            throw;
        }
    }

    [[nodiscard]] std::string_view output() const& noexcept {
        return encoded_chunk_;
    }
    std::string_view output() const&& = delete;

    // A sent chunk no longer has a transport borrower. Keep ordinary stream
    // buffers reusable without retaining unusually large writes for the life
    // of a long-running handler.
    void release_output() {
        constexpr std::size_t retained_bytes = 64 * 1024;
        http_clear_string_retaining_small(encoded_chunk_, retained_bytes);
    }

private:
    struct unprepared_type final {};
    // Identity output and protocol-suppressed bodies both need no encoder.
    struct inactive_type final {};
    struct pending_type final {};
    struct finished_type final {};
    struct failed_type final {};

    struct active_type final {
        explicit active_type(stream_encoder_lease lease) noexcept
            : lease_(std::move(lease)) {}

        stream_encoder_lease lease_;
    };

    using state_type = std::variant<unprepared_type, inactive_type, pending_type, finished_type, failed_type, active_type>;

    // A length the head declares sizes the stream like a buffered body; an
    // undeclared length leaves the decision to the negotiated coding.
    [[nodiscard]] bool stream_length_allows_encoding(const http_response& response) const noexcept {
        const auto declared = declared_content_length(response);
        return !declared.has_value() || declared_length_permits_encoding(selection_, *compression_, *declared);
    }

    void encode_slice(std::string_view input, bool flush) {
        auto* active_state = std::get_if<active_type>(&state_);
        if (active_state == nullptr) {
            encoded_chunk_.clear();
            throw std::logic_error("streaming response encoder is not active");
        }
        encoded_chunk_.clear();
        try {
            active_state->lease_.encoder().write(input, encoded_chunk_, flush);
        } catch (...) {
            fail_encoder(*active_state);
            throw;
        }
    }

    // A codec that failed mid-stream is never returned to the worker pool.
    void fail_encoder(active_type& active_state) noexcept {
        active_state.lease_.discard();
        abort();
    }

    worker_codecs& codecs_;
    http_response_coding_selection selection_;
    const compression_config* compression_;
    std::string encoded_chunk_;
    state_type state_;
};

}  // namespace ruvia::detail

namespace ruvia {
class context;
}

namespace ruvia::detail {

// Inactivity belongs to the connection; request bodies and output waits have
// independent per-stream deadlines.
[[nodiscard]] inline connection_scanner::phase multiplexed_inactivity_phase(bool peer_settings_received,
    bool header_block_in_progress, bool stream_active, bool tunnel_active) noexcept {
    if (!peer_settings_received || header_block_in_progress) {
        return connection_scanner::phase::reading_initial;
    }
    if (tunnel_active) {
        return connection_scanner::phase::long_lived;
    }
    return stream_active ? connection_scanner::phase::idle : connection_scanner::phase::awaiting_stream;
}

class multiplexed_request_body_deadline final {
public:
    void open(std::int64_t now_ms) noexcept {
        open_ = true;
        opened_ms_ = now_ms;
        progress_ms_ = now_ms;
    }
    void progress(std::int64_t now_ms) noexcept {
        progress_ms_ = now_ms;
    }
    void close() noexcept {
        open_ = false;
    }
    [[nodiscard]] bool overdue(std::int64_t now_ms,
        const std::optional<timeout_duration>& inactivity,
        const std::optional<timeout_duration>& completion) const noexcept {
        return open_ &&
               ((inactivity.has_value() && now_ms - progress_ms_ >= inactivity->value().count()) ||
                   (completion.has_value() && now_ms - opened_ms_ >= completion->value().count()));
    }

private:
    std::int64_t opened_ms_{0};
    std::int64_t progress_ms_{0};
    bool open_{false};
};

// A timeout drain must end the connection at its bound; an ordinary drain only
// finalizes GOAWAY. Protocol owners keep their own wake/close ordering.
class multiplexed_drain_timer final {
public:
    void request_timeout() noexcept {
        timeout_drain_ = true;
    }
    [[nodiscard]] bool needed(bool terminated, bool final_boundary) const noexcept {
        return !timer_.registered() && !terminated && (!final_boundary || timeout_drain_);
    }
    void mark_expired() noexcept {
        expired_ = timeout_drain_;
    }
    [[nodiscard]] bool expired() const noexcept {
        return expired_;
    }
    void cancel() noexcept {
        timer_.cancel();
    }

    template <typename duration_type, typename expired_type, typename failure_type>
    void arm(const worker_ref& worker_value, duration_type bound,
        expired_type on_expired, failure_type on_failure) noexcept {
        try {
            const auto status = worker_value.schedule_timer(timer_,
                deadline_after(bound), [on_expired](worker_timer_outcome outcome) {
                    // Cancellation belongs to worker shutdown, which closes the session.
                    if (outcome == worker_timer_outcome::expired) {
                        on_expired();
                    }
                });
            if (status == worker_timer_schedule_status::worker_stopping) {
                on_expired();
            }
        } catch (...) {
            on_failure();
        }
    }

private:
    bool timeout_drain_{false};
    bool expired_{false};
    // Unregister before the state captured by its completion can retire.
    worker_timer_registration timer_;
};

// Connection-owned terminal state shared by every Web capability derived from
// one multiplexed HTTP/2 or HTTP/3 session. Stream waits that must end on
// termination combine their own stop token with token().
class multiplexed_session_termination final {
public:
    multiplexed_session_termination() noexcept = default;
    multiplexed_session_termination(const multiplexed_session_termination&) = delete;
    multiplexed_session_termination& operator=(const multiplexed_session_termination&) = delete;

    [[nodiscard]] bool terminated() const noexcept {
        return static_cast<bool>(error_);
    }

    [[nodiscard]] std::error_code error() const noexcept {
        return error_;
    }

    [[nodiscard]] bool terminate(std::error_code error) noexcept {
        if (terminated()) {
            return false;
        }
        error_ = error ? error : std::make_error_code(std::errc::connection_aborted);
        if (stop_source_) {
            stop_source_->request_stop();
        }
        return true;
    }

    // Allocated on first use; already stopped once the session terminated.
    [[nodiscard]] stop_token token() {
        if (!stop_source_) {
            stop_source_.emplace();
            if (terminated()) {
                stop_source_->request_stop();
            }
        }
        return stop_source_->token();
    }

private:
    std::error_code error_;
    std::optional<stop_source> stop_source_;
};

// The one asynchronous wake primitive of a dispatched multiplexed stream. Body
// readers and writers waiting for send window or output budget share it and
// always re-check their own readiness after a wake.
class multiplexed_stream_signal final {
public:
    class output_wait_guard final {
    public:
        explicit output_wait_guard(multiplexed_stream_signal& signal) noexcept
            : signal_(signal) {
            signal_.set_waiting_output(true);
        }
        ~output_wait_guard() noexcept {
            signal_.set_waiting_output(false);
        }
        output_wait_guard(const output_wait_guard&) = delete;
        output_wait_guard& operator=(const output_wait_guard&) = delete;

    private:
        multiplexed_stream_signal& signal_;
    };

    multiplexed_stream_signal(const worker_ref& worker_value, multiplexed_session_termination& termination)
        : signal_(worker_value),
          termination_(termination) {}
    multiplexed_stream_signal(worker_ref&&, multiplexed_session_termination&) = delete;

    void wake() noexcept {
        signal_.notify();
    }

    // The stream ended abnormally (peer reset, local reset): every wait ends.
    void stop() noexcept {
        stopped_ = true;
        if (stop_source_) {
            stop_source_->request_stop();
        }
        wake();
    }

    [[nodiscard]] bool stopped() const noexcept {
        return stopped_ || termination_.terminated();
    }

    [[nodiscard]] stop_token terminal_stop_token() {
        if (!stop_source_) {
            stop_source_.emplace();
        }
        if (stopped()) {
            stop_source_->request_stop();
        }
        return stop_source_->token();
    }

    [[nodiscard]] bool terminated() const noexcept {
        return termination_.terminated();
    }

    [[nodiscard]] std::error_code terminal_error() const noexcept {
        return termination_.error();
    }

    [[nodiscard]] multiplexed_session_termination& termination() & noexcept RUVIA_LIFETIMEBOUND {
        return termination_;
    }
    multiplexed_session_termination& termination() && = delete;

    // Spans one whole wait for peer window / acknowledgements to free output
    // capacity; the connection scanner times it against write_timeout_.
    void set_waiting_output(bool waiting) noexcept {
        waiting_output_ = waiting;
        output_wait_observed_ms_.reset();
        output_wait_expired_ = false;
    }

    [[nodiscard]] bool waiting_output() const noexcept {
        return waiting_output_;
    }

    // True once per wait, when it has been observed for `timeout` in the
    // scanner's clock. The first observation starts the clock.
    [[nodiscard]] bool output_wait_expired(std::int64_t now_ms, timeout_duration timeout) noexcept {
        if (!waiting_output_ || output_wait_expired_) {
            return false;
        }
        if (!output_wait_observed_ms_.has_value()) {
            output_wait_observed_ms_ = now_ms;
        }
        output_wait_expired_ = now_ms - *output_wait_observed_ms_ >= timeout.value().count();
        return output_wait_expired_;
    }

    [[nodiscard]] worker_signal::wait_operation wait() {
        return signal_.wait();
    }

    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND {
        return signal_.worker();
    }
    const worker_ref& worker() const&& = delete;

private:
    worker_signal signal_;
    multiplexed_session_termination& termination_;
    std::optional<stop_source> stop_source_;
    bool stopped_{false};
    bool waiting_output_{false};
    bool output_wait_expired_{false};
    std::optional<std::int64_t> output_wait_observed_ms_;
};

// Streams of one multiplexed connection that may open or read a file body on
// the blocking pool at once; the rest wait holding no read buffer and no pool
// task.
inline constexpr std::size_t multiplexed_file_read_slots = 4;

struct multiplexed_buffered_response_write_result final {
    // The final status, once the response head was submitted.
    std::optional<http_status_code> committed_status_{};
    // Why a file body failed. Without committed_status_ nothing of the
    // response was sent and the stream still awaits one.
    std::optional<response_file_failure> file_failure_{};
};

class multiplexed_file_read_limiter final {
public:
    class slot final {
    public:
        slot(slot&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)) {}
        ~slot() noexcept {
            if (owner_ != nullptr) {
                auto* const owner = std::exchange(owner_, nullptr);
                --owner->active_;
                owner->released_.notify();
            }
        }

        slot(const slot&) = delete;
        slot& operator=(const slot&) = delete;
        slot& operator=(slot&&) = delete;

    private:
        friend class multiplexed_file_read_limiter;

        explicit slot(multiplexed_file_read_limiter& owner) noexcept
            : owner_(&owner) {}

        multiplexed_file_read_limiter* owner_;
    };

    explicit multiplexed_file_read_limiter(
        const worker_ref& worker_value, std::size_t max_slots = multiplexed_file_read_slots) noexcept
        : released_(worker_value),
          max_slots_(max_slots) {}
    multiplexed_file_read_limiter(worker_ref&&, std::size_t = multiplexed_file_read_slots) = delete;
    multiplexed_file_read_limiter(const multiplexed_file_read_limiter&) = delete;
    multiplexed_file_read_limiter& operator=(const multiplexed_file_read_limiter&) = delete;

    // Admission and its return obligation travel as one move-only value.
    template <typename stream_ended_predicate>
    [[nodiscard]] task<std::optional<slot>> acquire(stream_ended_predicate stream_ended) {
        while (active_ >= max_slots_) {
            co_await released_.wait();
            if (stream_ended()) {
                co_return std::nullopt;
            }
        }
        ++active_;
        co_return std::optional<slot>(slot(*this));
    }

    template <typename stream_ended_predicate>
    [[nodiscard]] task<std::optional<multiplexed_buffered_response_write_result>> open_file_body(
        std::optional<response_file_source>& source, const http_response& response,
        const response_file_io& file_io, stream_ended_predicate stream_ended) {
        source.emplace(response, file_io);
        bool opened = false;
        {
            const auto held = co_await acquire(stream_ended);
            if (!held) {
                co_return multiplexed_buffered_response_write_result{};
            }
            opened = co_await source->open_first();
        }
        if (stream_ended()) {
            co_return multiplexed_buffered_response_write_result{};
        }
        if (!opened) {
            co_return multiplexed_buffered_response_write_result{.file_failure_ = *source->failure()};
        }
        co_return std::nullopt;
    }

private:
    worker_signal released_;
    std::size_t max_slots_;
    std::size_t active_{0};
};

// Shared state and lifecycle for HTTP/2 and HTTP/3 streaming response sinks.
// Derived sinks supply the protocol-specific head submission, encoded DATA
// write, terminal finish, and abort check.
template <typename derived_type, typename stream_type>
class multiplexed_response_stream_sink_base {
public:
    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept RUVIA_LIFETIMEBOUND {
        return state_.commit_plan();
    }
    const http_response_stream_commit_plan* commit_plan() const&& = delete;

    [[nodiscard]] bool aborted() const noexcept {
        return state_.aborted() || (!state_.ended() && (derived().stream_aborted() || signal_.terminated()));
    }

    // Routes the stop requests of the writer's output operations.
    [[nodiscard]] operation_interrupt_source& output_interrupts() & noexcept RUVIA_LIFETIMEBOUND {
        return output_interrupts_;
    }
    operation_interrupt_source& output_interrupts() && = delete;

    task<stream_commit_result> commit() {
        if (state_.committed()) {
            throw std::logic_error("response stream is already committed");
        }
        co_await commit_head(http_response_trailer_intent::none);
        co_return state_.ended() ? stream_commit_result::completed : stream_commit_result::body;
    }

    void bind_context(context* context_value) {
        interim_output_ = context_value != nullptr ? context_access::interim_output(*context_value) : nullptr;
        state_.bind_context(context_value);
    }

    void release_context() noexcept {
        state_.release_context();
    }

    task<void> write(std::string_view chunk) {
        throw_if_terminated();
        throw_if_body_failed();
        state_.ensure_body_allowed();
        if (chunk.empty()) {
            co_return;
        }
        if (compression_.active()) {
            try {
                co_await compression_.write(
                    chunk, [this](std::string_view encoded) { return write_encoded(encoded); });
            } catch (...) {
                state_.mark_aborted();
                throw;
            }
            co_return;
        }
        co_await write_encoded(chunk);
    }

    task<void> write_encoded(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }
        try {
            co_await derived().write_encoded_data(chunk);
        } catch (...) {
            state_.mark_aborted();
            throw;
        }
    }

    task<timer_sleep_result> sleep(std::chrono::milliseconds duration, const stop_token& stop_token_value) {
        state_.ensure_body_allowed();
        // Connection termination wakes the sleep through the combined token and
        // then fails it; the caller's own stop still reports stop_requested.
        const stop_group stop_owner(stop_token_value, signal_.termination().token());
        const auto result = co_await sleep_for(signal_.worker(), duration, stop_owner.token());
        if (signal_.terminated()) {
            throw inbound_transport_error(signal_.terminal_error(), derived_type::sleep_terminated_error_message);
        }
        if (derived().stream_aborted()) {
            throw inbound_transport_error(
                std::make_error_code(std::errc::connection_reset), derived_type::sleep_reset_error_message);
        }
        co_return result;
    }

    task<void> end(std::span<const http_header_view> trailers) {
        // An empty terminal call is idempotent once the content ended.
        if (state_.ended()) {
            if (!trailers.empty()) {
                throw std::logic_error("response stream is already ended");
            }
            co_return;
        }
        throw_if_terminated();
        throw_if_body_failed();

        const auto trailer_section = validate_http_response_trailers(trailers);
        const auto trailer_intent = response_trailer_intent(trailer_section);
        if (!state_.committed()) {
            co_await commit_head(trailer_intent);
        }
        throw_if_body_failed();
        if (state_.ended()) {
            co_return;
        }
        if (!trailer_section.empty()) {
            state_.ensure_trailers_allowed();
        }
        if (compression_.active()) {
            try {
                compression_.finish();
            } catch (...) {
                state_.mark_aborted();
                throw;
            }
            co_await write_encoded(compression_.output());
            compression_.release_output();
        }
        try {
            if constexpr (std::is_void_v<decltype(derived().finish_stream_response(trailer_section))>) {
                derived().finish_stream_response(trailer_section);
            } else {
                co_await derived().finish_stream_response(trailer_section);
            }
        } catch (...) {
            state_.mark_aborted();
            throw;
        }
        state_.mark_ended();
    }

protected:
    multiplexed_response_stream_sink_base(stream_type& stream, multiplexed_stream_signal& signal, http_known_method request_method, worker_codecs& codecs,
        http_response_coding_selection response_coding, const compression_config* compression,
        const http_cors_policy* cors) noexcept
        : stream_(stream),
          signal_(signal),
          request_method_(request_method),
          cors_(cors),
          compression_(codecs, response_coding, compression) {}

    void throw_if_terminated() const {
        if (signal_.terminated()) {
            throw inbound_transport_error(signal_.terminal_error(), derived_type::terminated_error_message);
        }
    }

    void throw_if_body_failed() {
        if (const auto& body_failure = stream_.body_failure(); body_failure.has_value()) {
            if (state_.committed()) {
                state_.mark_aborted();
            }
            if (*body_failure == http_request_body_failure::buffer_limit) {
                throw inbound_buffer_limit_error{};
            }
            throw http_request_body_protocol_error(*body_failure);
        }
    }

    stream_type& stream_;
    multiplexed_stream_signal& signal_;
    response_stream_state state_;
    http_interim_response_output* interim_output_{};
    http_known_method request_method_{http_known_method::unknown};
    const http_cors_policy* cors_;
    http_streaming_response_compression compression_;
    operation_interrupt_source output_interrupts_;

private:
    [[nodiscard]] derived_type& derived() noexcept {
        return static_cast<derived_type&>(*this);
    }
    [[nodiscard]] const derived_type& derived() const noexcept {
        return static_cast<const derived_type&>(*this);
    }

    task<void> commit_head(http_response_trailer_intent trailer_intent) {
        throw_if_terminated();
        throw_if_body_failed();
        try {
            if (context_access::output_committed(state_.bound_context())) {
                throw std::logic_error("the request's tunnel is already established");
            }
            auto response = co_await context_access::commit_streaming_head(state_.bound_context());
            validate_response_stream_status(context_access::request(state_.bound_context()), response);
            if (cors_ != nullptr) {
                (void)cors_->apply_response_headers(
                    context_access::request(state_.bound_context()), response);
            }
            throw_if_terminated();
            throw_if_body_failed();
            compression_.prepare(context_access::request(state_.bound_context()), response);
            const auto commit_body_plan = plan_http_response_body(request_method_, response.status());
            compression_.activate(commit_body_plan);
            const auto submitted_head = derived().submit_streaming_head(std::move(response), trailer_intent);
            if (interim_output_ != nullptr) {
                interim_output_->commit_final();
            }
            stream_.mark_response_committed();
            state_.mark_committed(submitted_head);
            derived().wake_writer();
        } catch (...) {
            if (!state_.committed()) {
                compression_.abort();
                state_.mark_aborted();
            }
            throw;
        }
    }
};

// Shared streaming request-body reader loop for HTTP/2 and HTTP/3 streams.
// Derived readers provide protocol-specific body failure, reset check, chunk
// extraction (returning flow-control credit), and end-of-body check.
template <typename derived_type>
class multiplexed_request_body_reader_base {
public:
    // An interrupted read leaves queued chunks and flow-control credit for the
    // next read.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) {
        operation_interrupt interrupt(interrupts_, signal_.worker(), operation, &signal_, &wake_signal,
            "request body read");
        throw_if_body_failed();
        if (auto* continue_output = std::exchange(continue_output_, nullptr)) {
            co_await continue_output->send_continue();
        }
        for (;;) {
            interrupt.throw_if_interrupted();
            throw_if_body_failed();
            if (signal_.terminated()) {
                throw inbound_transport_error(signal_.terminal_error(), derived_type::terminated_error_message);
            }
            if (signal_.stopped() || derived().stream_aborted()) {
                throw inbound_transport_error(
                    std::make_error_code(std::errc::connection_reset), derived_type::reset_error_message);
            }
            const auto chunk = derived().pop_chunk();
            if (!chunk.empty()) {
                co_return ::ruvia::as_bytes(chunk);
            }
            if (derived().body_ended()) {
                co_return std::nullopt;
            }
            co_await signal_.wait();
        }
    }

protected:
    multiplexed_request_body_reader_base(
        multiplexed_stream_signal& signal, http_interim_response_output* continue_output) noexcept
        : signal_(signal),
          continue_output_(continue_output) {}

    multiplexed_stream_signal& signal_;
    http_interim_response_output* continue_output_;
    operation_interrupt_source interrupts_;

private:
    static void wake_signal(void* raw) noexcept {
        static_cast<multiplexed_stream_signal*>(raw)->wake();
    }
    [[nodiscard]] derived_type& derived() noexcept {
        return static_cast<derived_type&>(*this);
    }
    [[nodiscard]] const derived_type& derived() const noexcept {
        return static_cast<const derived_type&>(*this);
    }
    void throw_if_body_failed() const {
        if (const auto& failure = derived().body_failure(); failure.has_value()) {
            if (*failure == http_request_body_failure::buffer_limit) {
                throw inbound_buffer_limit_error{};
            }
            throw http_request_body_protocol_error(*failure);
        }
    }
};

// Shared duplex CONNECT tunnel read loop and WebSocket write disposition
// forwarding for HTTP/2 and HTTP/3 streams.
template <typename derived_type>
class multiplexed_tunnel_transport_base {
public:
    template <typename buffer_type>
    [[nodiscard]] task<bool> read_more(buffer_type& buffer) {
        return read_more(buffer, []() noexcept {}, asio::cancellation_slot{});
    }

    // A cancellation emitted on `slot` wakes the wait; `check` runs before
    // every wait and may throw to abandon it. Input that has not been
    // consumed stays queued for the next read_more().
    template <typename buffer_type, typename check_type>
    [[nodiscard]] task<bool> read_more(buffer_type& buffer, check_type check, asio::cancellation_slot slot) {
        const wake_assignment assignment(signal_, slot);
        for (;;) {
            derived().require_live();
            if (derived().consume_tunnel_chunk(buffer)) {
                co_return true;
            }
            if (derived().receive_ended()) {
                co_return false;
            }
            derived().require_live();
            check();
            co_await signal_.wait();
        }
    }

protected:
    explicit multiplexed_tunnel_transport_base(multiplexed_stream_signal& signal) noexcept
        : signal_(signal) {}

    // While assigned, a cancellation emitted on the slot wakes this stream's waits.
    class wake_assignment final {
    public:
        wake_assignment(multiplexed_stream_signal& signal, asio::cancellation_slot slot)
            : slot_(slot) {
            if (slot_.is_connected()) {
                slot_.assign([&signal](asio::cancellation_type) noexcept { signal.wake(); });
            }
        }
        ~wake_assignment() {
            if (slot_.is_connected()) {
                slot_.clear();
            }
        }
        wake_assignment(const wake_assignment&) = delete;
        wake_assignment& operator=(const wake_assignment&) = delete;

    private:
        asio::cancellation_slot slot_;
    };

    multiplexed_stream_signal& signal_;

private:
    [[nodiscard]] derived_type& derived() noexcept {
        return static_cast<derived_type&>(*this);
    }
    [[nodiscard]] const derived_type& derived() const noexcept {
        return static_cast<const derived_type&>(*this);
    }
};

// `request_stream`: the connection's listener is a request stream one, whose
// every request streams its body (a CONNECT is a tunnel request).
[[nodiscard]] inline std::pair<route_resolution, request_body_mode> resolve_multiplexed_stream_route(
    const route_table& routes, bool request_stream, std::string_view method_token, std::string_view target_path,
    std::string_view authority, std::string_view protocol) noexcept {
    const auto method = classify_http_method(method_token);
    if (request_stream) {
        return {resolve_request_stream_route(method == http_known_method::connect), request_body_mode::stream};
    }
    const auto path = target_path.substr(0, target_path.find('?'));
    const auto extended_protocol = http_ascii_equals_ignore_case(protocol, "websocket")
                                       ? std::string_view("websocket")
                                       : protocol;
    auto resolution = routes.resolve(route_request_view{
        method, method_token, path, authority, extended_protocol});
    auto body_mode = request_body_mode::buffered;
    if (const auto* resolved = resolution.resolved()) {
        body_mode = resolved->route().endpoint().request_body_mode();
        if ((method == http_known_method::connect && resolved->route().endpoint().websocket() != nullptr) ||
            resolved->route().endpoint().tunnel() != nullptr) {
            body_mode = request_body_mode::stream;
        }
    }
    return {std::move(resolution), body_mode};
}

[[nodiscard]] inline task<http_response> respond_http_request_body_failure(const route_table& routes,
    const http_request& request, http_request_body_failure failure,
    const context_services& services, http_response_tail& tail) {
    tail.disable_response_coding();
    return respond_http_request_rejection(routes, request,
        http_request_rejection::error(
            [&] { const auto err = http_request_body_protocol_error(failure); return http_error_info({.status_ = err.status(), .code_ = err.code(), .message_ = std::string_view(err.what())}); }()),
        services);
}

// Completes a multiplexed response-stream route result. Returns true when the
// stream already terminated (peer abort before commit or committed head), or
// false when `response` holds a buffered route/recovered response to finish.
template <typename reset_type>
[[nodiscard]] inline bool complete_multiplexed_response_stream_dispatch(
    response_stream_dispatch_result& result_value, bool request_body_failed,
    const http_server_options& options, std::string_view remote_address,
    http_response_tail& tail, http_response& response, bool& body_failure_handled,
    reset_type&& reset_stream) {
    if (result_value.aborted_before_commit() != nullptr) {
        return true;
    }
    if (const auto committed_status = result_value.committed_status()) {
        if (const auto* failed = result_value.failed_after_commit()) {
            reset_stream(request_body_failed);
            if (!request_body_failed) {
                options.connection_failure_.invoke(remote_address, failed->exception());
            }
        } else if (request_body_failed) {
            reset_stream(true);
        }
        tail.record_access(options.access_log_, *committed_status);
        return true;
    }
    if (auto* route_response = result_value.route_response()) {
        response = std::move(*route_response).take_response();
    } else if (auto* recovered = result_value.recovered_failure()) {
        body_failure_handled = request_body_failed;
        response = std::move(*recovered).take_response();
    } else {
        throw std::logic_error("response stream dispatch returned no terminal alternative");
    }
    return false;
}

template <typename stream_id_type, typename writer_type, typename reset_type>
[[nodiscard]] task<void> finish_multiplexed_buffered_response(stream_id_type stream_id,
    const std::optional<http_request_body_failure>& body_failure, bool body_failure_handled,
    http_response& response, const http_request& request, const route_table& routes, const context_services& request_services,
    const http_server_options& options, std::string_view remote_address,
    http_response_tail& tail, writer_type& writer, reset_type reset_stream) {
    if (body_failure.has_value() && !body_failure_handled) {
        body_failure_handled = true;
        response = co_await respond_http_request_body_failure(
            routes, request, *body_failure, request_services, tail);
    }
    auto preparation = co_await tail.prepare_or_finish(
        response, options, routes, request_services);
    if (body_failure.has_value() && !body_failure_handled) {
        response = co_await respond_http_request_body_failure(
            routes, request, *body_failure, request_services, tail);
        preparation = co_await tail.prepare_or_finish(
            response, options, routes, request_services);
    }
    if (request_services.get_stop_token().stop_requested()) {
        reset_stream(true);
        co_return;
    }
    auto written = co_await writer.write(stream_id, response, preparation.write_plan_);
    if (written.file_failure_ && !written.committed_status_) {
        // Nothing of the response was sent: the stream gets the error
        // response instead of a reset.
        options.connection_failure_.invoke(remote_address, written.file_failure_->fault());
        tail.disable_response_coding();
        response = co_await routes.handle_exception(
            request, written.file_failure_->response_error(), request_services);
        preparation = co_await tail.prepare_or_finish(
            response, options, routes, request_services);
        written = co_await writer.write(stream_id, response, preparation.write_plan_);
        if (written.file_failure_ && !written.committed_status_) {
            reset_stream(false);
        }
    }
    if (written.file_failure_) {
        options.connection_failure_.invoke(remote_address, written.file_failure_->fault());
    }
    if (written.committed_status_) {
        tail.record_access(options.access_log_, *written.committed_status_);
    }
}

}  // namespace ruvia::detail
