#include "server/http_request_admission.h"

#include <utility>

#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_request_body_failure.h"

#include "http/http_protocol_error_info.h"
#include "ratelimit/rate_limit_decision.h"
#include "router/route_table.h"
#include "server/request_body_limit.h"

namespace ruvia::detail {

http_request_admission_policy http_request_admission_policy::server(const http_server_options& options) noexcept {
    return {
        .handler_deadline_ = options.deadline_ ? std::optional{options.deadline_->handler_} : std::nullopt,
        .max_stream_body_bytes_ = options.max_stream_body_bytes_,
        .max_buffered_body_bytes_ = options.max_buffered_body_bytes_,
        .coding_availability_ = options.compression_.has_value()
                                    ? http_response_coding_availability::identity_and_compression
                                    : http_response_coding_availability::identity_only,
        .apply_rate_limit_ = true,
    };
}

protocol_byte_limit http_request_body_limit(const route_resolution& resolution, request_body_mode mode,
    const http_request_admission_policy& policy) noexcept {
    const auto* resolved = resolution.resolved();
    return request_body_byte_limit(mode, policy.max_stream_body_bytes_, policy.max_buffered_body_bytes_,
        resolved != nullptr ? resolved->route().max_request_body_bytes() : 0);
}

http_request_rejection http_request_rejection::rate_limited(rate_limit_rejection rejection) noexcept {
    return http_request_rejection(rate_limit_rejection_error(), rejection);
}

task<http_response> respond_http_request_rejection(const route_table& routes, const http_request& request,
    request_memory& memory, const http_request_rejection& rejection, context_services services) {
    auto response = co_await routes.handle_error(request, memory, rejection.error(), std::move(services));
    if (const auto* rate_limit = rejection.rate_limit()) {
        apply_rate_limit_rejection_headers(response, *rate_limit);
    }
    co_return response;
}

void http_response_tail::record_access(const access_log_sink& access_log, http_status_code status) const noexcept {
    if (!access_log.callback_) {
        return;
    }
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_).count();
    const access_log_record record(
        *request_, client_address_, status, micros < 0 ? 0 : static_cast<std::uint64_t>(micros));
    access_log.invoke(record);
}

http_request_admission admit_http_request(const http_request_admission_input& input,
    const http_request_admission_policy& policy, context_services services, std::optional<request_deadline>& deadline) {
    const auto& request = input.request_;
    // Negotiate against every coding. A document root or context::static_file
    // may serve an indexed precompressed sidecar even when this worker has no
    // runtime encoder; the encoder capability travels to representation
    // preparation instead of pruning the client's choices here. A failed
    // negotiation stays typed until the response status is known: a buffered
    // 204/205/304 carries no representation and must not become 406.
    const auto* selection = input.coding_.selected();
    auto coding_policy = selection != nullptr ? http_response_coding_policy::selected(*selection)
                                              : http_response_coding_policy::no_acceptable_coding();
    // Keyed on the client, not the hop: behind a trusted proxy every request
    // would otherwise share the proxy's single key. Resolved once per request.
    const auto client_address = services.resolve_conn_info(request).client().address();

    const auto* resolved = input.resolution_.resolved();
    // Armed before every server-layer rejection: a custom on_error handler is
    // a handler too and must observe the request stop token.
    const auto handler_deadline =
        effective_handler_deadline(policy.handler_deadline_, resolved != nullptr ? resolved->route().deadline_ms() : 0);
    if (handler_deadline > std::chrono::milliseconds::zero()) {
        deadline.emplace(services.get_stop_token()).arm(services.worker(), handler_deadline);
    }
    if (deadline.has_value()) {
        services = services.with_request_deadline(*deadline);
    }

    http_request_admission admission(
        http_response_tail(request, coding_policy, client_address, input.start_), std::move(services));
    admission.resolved_ = resolved;
    admission.coding_availability_ = policy.coding_availability_;
    const auto* endpoint = resolved != nullptr ? &resolved->route().endpoint() : nullptr;
    admission.body_limit_ = http_request_body_limit(input.resolution_,
        endpoint != nullptr ? endpoint->request_body_mode() : request_body_mode::buffered, policy);

    const auto reject = [&](http_request_endpoint kind, http_request_rejection rejection) {
        admission.endpoint_ = kind;
        admission.rejection_ = rejection;
        return std::move(admission);
    };

    // Expect extensions are valid HTTP syntax. The protocol reports the
    // semantic fact; this product implements only 100-continue and chooses the
    // RFC 9110-permitted 417 before reading request content.
    if (input.expectation_ != nullptr) {
        if (const auto* rejection = input.expectation_->rejection()) {
            return reject(http_request_endpoint::rejection,
                http_request_rejection::error(copy_http_protocol_error_info(input.resource_, rejection->protocol_error())));
        }
    }
    if (policy.apply_rate_limit_) {
        const auto decision = decide_request_rate_limit(admission.services_.rate_limiter(), client_address);
        if (const auto* rejection = decision.rejection()) {
            return reject(http_request_endpoint::rejection, http_request_rejection::rate_limited(*rejection));
        }
    }
    // Content already in hand (buffered bodies and subrequests) is held
    // to the same ceiling a streaming protocol enforces while reading.
    if (admission.body_limit_.exceeds(request.body_bytes().size())) {
        return reject(http_request_endpoint::rejection,
            http_request_rejection::error(
                copy_http_protocol_error_info(input.resource_, http_request_body_failure::too_large().protocol_error())));
    }
    if (endpoint == nullptr) {
        admission.endpoint_ = http_request_endpoint::buffered;
        return admission;
    }
    if (const auto* tunnel = endpoint->tunnel()) {
        admission.udp_tunnel_ = tunnel->protocol() == "connect-udp";
        if (admission.udp_tunnel_ && validate_http_connect_udp_request(request).index() != 0) {
            return reject(http_request_endpoint::rejection,
                http_request_rejection::error(http_error_info(
                    {.status_ = http_status::bad_request, .message_ = "invalid CONNECT-UDP request head"})));
        }
        admission.endpoint_ = http_request_endpoint::tunnel;
        return admission;
    }
    if (endpoint->get_websocket() != nullptr) {
        // An upgrade selects no HTTP response representation.
        admission.endpoint_ = http_request_endpoint::websocket;
        return admission;
    }
    if (endpoint->response_stream() != nullptr) {
        // A response stream commits its representation before a buffered
        // status can be inspected, so a failed negotiation is final here.
        if (admission.tail_.coding_policy().negotiation_failed()) {
            return reject(http_request_endpoint::not_acceptable,
                http_request_rejection::error(http_error_info({.status_ = http_status::not_acceptable,
                    .code_ = "not_acceptable",
                    .message_ = "no acceptable response content coding"})));
        }
        admission.endpoint_ = http_request_endpoint::response_stream;
        return admission;
    }
    const auto* buffered = endpoint->buffered();
    admission.endpoint_ = buffered != nullptr && buffered->request_body_mode() == request_body_mode::stream
                              ? http_request_endpoint::stream_body
                              : http_request_endpoint::buffered;
    return admission;
}

}  // namespace ruvia::detail
