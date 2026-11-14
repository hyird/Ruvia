#include "http_request_admission.h"

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "context_access.h"
#include "http1_session_state.h"
#include "http_connect_udp.h"
#include "http_cors.h"
#include "http_error.h"
#include "http_request.h"
#include "http_request_body_failure.h"
#include "http_response.h"
#include "http_response_body.h"
#include "rate_limit.h"
#include "response_coding_policy.h"
#include "route_table.h"
#include "server_options.h"
#include "static_root_index.h"
#include "task.h"
#include "worker_ref.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] protocol_byte_limit request_body_byte_limit(request_body_mode body_mode, const std::optional<std::size_t>& max_stream_body_bytes, std::size_t max_buffered_body_bytes, std::size_t route_limit) {
    if (body_mode != request_body_mode::stream) {
        const auto limit = route_limit != 0 && route_limit < max_buffered_body_bytes ? route_limit : max_buffered_body_bytes;
        return protocol_byte_limit::limited(limit);
    }
    if (!max_stream_body_bytes.has_value()) {
        return route_limit != 0 ? protocol_byte_limit::limited(route_limit) : protocol_byte_limit::unlimited();
    }
    const auto limit = route_limit != 0 && route_limit < *max_stream_body_bytes ? route_limit : *max_stream_body_bytes;
    return protocol_byte_limit::limited(limit);
}

// RFC 9110 §7.4 against the transport connection's facts. A secured
// connection without a recorded certificate cannot vouch for any host.
[[nodiscard]] bool connection_carries_target(const http_request& request, const context_services& services) noexcept {
    const auto origin = decide_http_target_origin(request, {.secured_ = services.get_conn_info().tls() != nullptr, .trusted_gateway_ = services.peer_trusted()});
    switch (origin.action()) {
        case http_target_origin_action::accept:
            return true;
        case http_target_origin_action::verify_certificate: {
            auto* certificate = services.served_certificate();
            return certificate != nullptr && certificate->covers(origin);
        }
        case http_target_origin_action::misdirected:
            break;
    }
    return false;
}

// One synchronous representation pass: apply CORS (`cors`, when set) and the
// listener's Alt-Svc, and run the compression policy up to its offload
// boundary, which only a worker with a blocking pool can cross.
[[nodiscard]] http_buffered_compression_step prepare_representation(const http_request& request, const http_response_coding_policy& policy, std::string_view alternative_service, const http_cors_policy* cors, http_response& response, const http_server_options& options, const worker_ref& worker) {
    if (cors != nullptr) {
        (void)cors->apply_response_headers(request, response);
    }
    apply_alternative_service(response, alternative_service);
    if (options.compression_.has_value()) {
        if (const auto* selection = policy.selection()) {
            return apply_response_compression(*selection, request.known_method(), response, *options.compression_, *options.codecs_, worker.blocking_pool() != nullptr);
        }
    }
    return {http_response_compression_status::not_applicable, false};
}

}  // namespace
http_request_admission_policy http_request_admission_policy::server(const http_server_options& options) noexcept {
    return {
        .max_stream_body_bytes_ = options.server_.max_stream_body_bytes_,
        .max_buffered_body_bytes_ = options.server_.max_buffered_body_bytes_,
        .apply_rate_limit_ = true,
        .verify_target_origin_ = true,
    };
}

protocol_byte_limit http_request_body_limit(const route_resolution& resolution, request_body_mode mode, const http_request_admission_policy& policy) noexcept {
    const auto* resolved = resolution.resolved();
    return request_body_byte_limit(mode, policy.max_stream_body_bytes_, policy.max_buffered_body_bytes_, resolved != nullptr ? resolved->route().max_request_body_bytes() : 0);
}

http_request_rejection http_request_rejection::rate_limited(rate_limit_rejection rejection) noexcept {
    return http_request_rejection(rate_limit_rejection_error(), rejection);
}

task<http_response> respond_http_request_rejection(const route_table& routes, const http_request& request, http_request_rejection rejection, const context_services& services) {
    auto response = co_await routes.respond_error(request, rejection.error(), services);
    if (const auto* rate_limit = rejection.rate_limit()) {
        apply_rate_limit_rejection_headers(response, *rate_limit);
    }
    co_return response;
}

void http_response_tail::record_access(const access_log_sink& access_log, http_status_code status) const noexcept {
    if (!access_log.callback_) {
        return;
    }
    access_log.invoke(access_log_record_access::make({
        .method_ = request_->method(),
        .known_method_ = request_->known_method(),
        .path_ = request_->path(),
        .protocol_version_ = request_->protocol_version(),
        .remote_address_ = remote_address_,
        .client_address_ = client_address_,
        .status_ = status,
        .duration_micros_ = access_log_record_access::duration_since(start_),
        .db_count_ = io_metrics_.db_count_,
        .db_duration_micros_ = io_metrics_.db_duration_micros_,
        .redis_count_ = io_metrics_.redis_count_,
        .redis_duration_micros_ = io_metrics_.redis_duration_micros_,
    }));
}

void http_response_tail::defer_access(const access_log_sink& access_log, http1_deferred_access_log& deferred, std::size_t head_end, http_status_code status) const {
    if (!access_log.callback_) {
        return;
    }
    deferred.defer(head_end, *request_, client_address_, start_, status, io_metrics_);
}

http_request_admission admit_http_request(const http_request_admission_input& input, const http_request_admission_policy& policy, const context_services& services) {
    const auto& request = input.request_;
    // The host's compression and CORS policies cover request stream responses
    // too; the application-wide rate limit belongs to the routes alone, and a
    // router applies its own.
    const bool request_stream = static_cast<bool>(services.request_stream());
    const auto coding = input.coding_;
    // Negotiate against every coding. A document root or context::static_file
    // may serve an indexed precompressed sidecar even when this worker has no
    // runtime encoder; the encoder capability travels to representation
    // preparation instead of pruning the client's choices here. A failed
    // negotiation stays in the selection until the response status is known: a
    // buffered 204/205/304 carries no representation and must not become 406.
    auto coding_policy = http_response_coding_policy::selected(coding);
    // Keyed on the client, not the hop: behind a trusted proxy every request
    // would otherwise share the proxy's single key. Resolved once per request
    // and bound to the services every context of this request is created from.
    const auto* resolved = input.resolution_.resolved();
    http_request_admission admission(http_response_tail(request, coding_policy, {}, {}, input.start_), services);
    if (services.peer_trusted()) {
        admission.services_.bind_conn_info(services.resolve_conn_info(request));
    }
    const auto& connection = admission.services_.get_conn_info();
    const auto remote_address = connection.remote().address();
    const auto client_address = connection.client().address();
    admission.tail_.bind_addresses(remote_address, client_address);
    admission.tail_.bind_alternative_service(services.alternative_service());
    admission.services_.bind_response_coding(coding);
    if (input.pinned_arrival_.has_value()) {
        admission.services_.pin_arrival(*input.pinned_arrival_);
    }
    if (resolved != nullptr) {
        admission.services_.bind_route_body_limit(resolved->route().max_request_body_bytes());
    }
    admission.resolved_ = resolved;
    const auto* endpoint = resolved != nullptr ? &resolved->route().endpoint() : nullptr;
    admission.body_limit_ = http_request_body_limit(input.resolution_, endpoint != nullptr ? endpoint->request_body_mode() : request_body_mode::buffered, policy);

    const auto reject = [&](http_request_rejection rejection) {
        admission.endpoint_ = http_request_endpoint::rejection;
        admission.rejection_ = rejection;
        return std::move(admission);
    };

    // Before any expectation, limit or endpoint decision: a request this
    // connection cannot carry is not this server's to evaluate.
    if (policy.verify_target_origin_ && !connection_carries_target(request, services)) {
        return reject(http_request_rejection::error(http_error_info({.status_ = http_status::misdirected_request, .message_ = "misdirected request"})));
    }

    // Expect extensions are valid HTTP syntax. The protocol reports the
    // semantic fact; this product implements only 100-continue and chooses the
    // RFC 9110-permitted 417 before reading request content.
    if (input.expectation_ != nullptr && *input.expectation_ == http_server_expectation_plan::reject_unsupported) {
        return reject(http_request_rejection::error([&] {
            const auto err = http_unsupported_expectation_error();
            return http_error_info({.status_ = err.status(), .code_ = err.code(), .message_ = std::string_view(err.what())});
        }()));
    }
    if (policy.apply_rate_limit_ && !request_stream) {
        if (const auto rejection = decide_request_rate_limit(admission.services_.rate_limiter(), client_address, input.pinned_arrival_)) {
            return reject(http_request_rejection::rate_limited(*rejection));
        }
    }
    // Content already in hand (buffered bodies and subrequests) is held
    // to the same ceiling a streaming protocol enforces while reading.
    if (admission.body_limit_.exceeds(request.body_bytes().size())) {
        return reject(http_request_rejection::error([&] {
            const auto err = http_request_body_protocol_error(http_request_body_failure::too_large);
            return http_error_info({.status_ = err.status(), .code_ = err.code(), .message_ = std::string_view(err.what())});
        }()));
    }
    if (endpoint == nullptr) {
        admission.endpoint_ = http_request_endpoint::buffered;
        return admission;
    }
    if (const auto* tunnel = endpoint->tunnel()) {
        admission.udp_tunnel_ = tunnel->protocol() == "connect-udp" || (request_stream && (request.protocol() == "connect-udp" || is_http_connect_udp_upgrade_request(request)));
        if (admission.udp_tunnel_ && validate_http_connect_udp_request(request).has_value()) {
            return reject(http_request_rejection::error(http_error_info({.status_ = http_status::bad_request, .message_ = "invalid CONNECT-UDP request head"})));
        }
        admission.endpoint_ = http_request_endpoint::tunnel;
        return admission;
    }
    if (endpoint->websocket() != nullptr) {
        // An upgrade selects no HTTP response representation.
        admission.endpoint_ = http_request_endpoint::websocket;
        return admission;
    }
    if (endpoint->response_stream() != nullptr) {
        admission.endpoint_ = http_request_endpoint::response_stream;
        return admission;
    }
    const auto* buffered = endpoint->buffered();
    admission.endpoint_ = buffered != nullptr && buffered->request_body_mode() == request_body_mode::stream ? http_request_endpoint::stream_body : http_request_endpoint::buffered;
    return admission;
}

task<http_request_admission> admit_and_dispatch_buffered_request(const route_table& routes, const http_request_admission_input& input, const http_request_admission_policy& policy, const context_services& services, http_response& response) {
    auto admission = admit_http_request(input, policy, services);
    if (const auto* rejection = admission.rejection()) {
        // As in the server: a rejection does not inherit the coding promise.
        admission.tail().disable_response_coding();
        response = co_await respond_http_request_rejection(routes, input.request_, *rejection, admission.services());
    } else {
        co_await routes.dispatch_buffered_response(input.request_, input.resolution_, document_root_binding::none(), admission.services(), admission.static_file_mode(), response);
    }
    co_return admission;
}

http_response_preparation http_response_tail::prepare(http_response& response, const http_server_options& options, const worker_ref& worker) const {
    const auto& request = *request_;
    const auto first_pass = prepare_representation(request, coding_policy_, alternative_service_, cors(options), response, options, worker);
    if (!first_pass.offload_) {
        auto policy = coding_policy_;
        buffered_response_recovery recovery;
        const auto step = recovery.advance(policy, request, response, first_pass.status_);
        if (step.action_ == buffered_response_recovery_action::ready) {
            return http_response_preparation(prepared_application_response{plan_buffered_http_response_write(request.known_method(), response), false});
        }
        return http_response_preparation(policy, recovery, step);
    }
    return http_response_preparation(coding_policy_);
}

task<prepared_application_response> http_response_tail::finish_prepare(const http_response_preparation& preparation, http_response& response, const http_server_options& options, const route_table& routes, const context_services& services) const {
    const auto& request = *request_;
    const auto& deferred = std::get<http_response_preparation::deferred_type>(preparation.state_);
    auto policy = deferred.policy_;
    auto recovery = deferred.recovery_;
    const auto* pending_step = deferred.pending_step_ ? &*deferred.pending_step_ : nullptr;
    http_buffered_compression_step pass{http_response_compression_status::not_applicable, true};
    for (;;) {
        buffered_response_recovery_step step;
        if (pending_step != nullptr) {
            step = *pending_step;
            pending_step = nullptr;
        } else {
            auto compression = pass.status_;
            if (pass.offload_) {
                compression = co_await compress_response_offloaded(*policy.selection(), response, services.worker(), services.get_stop_token());
            }
            step = recovery.advance(policy, request, response, compression);
        }
        if (step.action_ == buffered_response_recovery_action::ready) {
            co_return prepared_application_response{plan_buffered_http_response_write(request.known_method(), response), recovery.recovered()};
        }
        if (step.action_ == buffered_response_recovery_action::respond_error) {
            response = co_await routes.respond_error(request, *step.error_, services);
        }
        pass = prepare_representation(request, policy, alternative_service_, cors(options), response, options, services.worker());
    }
}

}  // namespace ruvia::detail
