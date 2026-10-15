#include "server/http_request_admission.h"

#include <utility>

#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_request_body_failure.h"

#include "http/http_protocol_error_info.h"
#include "ratelimit/rate_limit_decision.h"
#include "router/route_early_data.h"
#include "router/route_table.h"
#include "server/http_connection_state.h"
#include "server/request_body_limit.h"

namespace ruvia::detail {

http_request_admission_policy http_request_admission_policy::server(const http_server_options& options) noexcept {
    return {
        .max_stream_body_bytes_ = options.server_.max_stream_body_bytes_,
        .max_buffered_body_bytes_ = options.server_.max_buffered_body_bytes_,
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
    access_log.invoke(access_log_record_access::make({
        .method_ = request_->method(),
        .known_method_ = request_->known_method(),
        .path_ = request_->path(),
        .protocol_version_ = request_->protocol_version(),
        .remote_address_ = client_address_,
        .status_ = status,
        .duration_micros_ = access_log_record_access::duration_since(start_),
    }));
}

void http_response_tail::defer_access(const access_log_sink& access_log, http1_deferred_access_log& deferred,
    std::size_t head_end, http_status_code status) const {
    if (!access_log.callback_) {
        return;
    }
    deferred.defer(head_end, *request_, client_address_, start_, status);
}

http_request_admission admit_http_request(const http_request_admission_input& input,
    const http_request_admission_policy& policy, context_services services) {
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
    // would otherwise share the proxy's single key. Resolved once per request
    // and bound to the services every context of this request is created from.
    services = services.with_conn_info(services.resolve_conn_info(request));
    const auto client_address = services.get_conn_info().client().address();

    const auto* resolved = input.resolution_.resolved();
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

    // RFC 8470: work carried by early data must be replay-safe.
    if (input.received_early_data_ &&
        !early_data_request_allowed(request.known_method(), !request.body_bytes().empty(), input.resolution_)) {
        return reject(http_request_endpoint::too_early,
            http_request_rejection::error(http_error_info({.status_ = http_status::too_early,
                .code_ = "too_early",
                .message_ = "request is not replay-safe for early data"})));
    }
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
    // Content already in hand (HTTP/2/3 buffered bodies, subrequests) is held
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
                http_request_rejection::error(http_error_info(http_not_acceptable_coding_error())));
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
