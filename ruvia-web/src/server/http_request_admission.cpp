#include "server/http_request_admission.h"

#include <utility>

#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http_connect_udp.h"

#include "http/http_protocol_error_info.h"
#include "ratelimit/rate_limit_decision.h"
#include "router/route_table.h"
#include "server/http_connection_state.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] protocol_byte_limit request_body_byte_limit(request_body_mode body_mode,
    const std::optional<std::size_t>& max_stream_body_bytes, std::size_t max_buffered_body_bytes,
    std::size_t route_limit) {
    if (body_mode != request_body_mode::stream) {
        const auto limit = route_limit != 0 && route_limit < max_buffered_body_bytes
                               ? route_limit
                               : max_buffered_body_bytes;
        return protocol_byte_limit::limited(limit);
    }
    if (!max_stream_body_bytes.has_value()) {
        return route_limit != 0 ? protocol_byte_limit::limited(route_limit)
                                : protocol_byte_limit::unlimited();
    }
    const auto limit =
        route_limit != 0 && route_limit < *max_stream_body_bytes ? route_limit : *max_stream_body_bytes;
    return protocol_byte_limit::limited(limit);
}

}  // namespace
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
        .db_count_ = io_metrics_.db_count_,
        .db_duration_micros_ = io_metrics_.db_duration_micros_,
        .redis_count_ = io_metrics_.redis_count_,
        .redis_duration_micros_ = io_metrics_.redis_duration_micros_,
    }));
}

void http_response_tail::defer_access(const access_log_sink& access_log, http1_deferred_access_log& deferred,
    std::size_t head_end, http_status_code status) const {
    if (!access_log.callback_) {
        return;
    }
    deferred.defer(head_end, *request_, client_address_, start_, status, io_metrics_);
}

http_request_admission admit_http_request(const http_request_admission_input& input,
    const http_request_admission_policy& policy, const context_services& services) {
    const auto& request = input.request_;
    // Negotiate against every coding. A document root or context::static_file
    // may serve an indexed precompressed sidecar even when this worker has no
    // runtime encoder; the encoder capability travels to representation
    // preparation instead of pruning the client's choices here. A failed
    // negotiation stays typed until the response status is known: a buffered
    // 204/205/304 carries no representation and must not become 406.
    const auto* selection = input.coding_ ? &*input.coding_ : nullptr;
    auto coding_policy = selection != nullptr ? http_response_coding_policy::selected(*selection)
                                              : http_response_coding_policy::no_acceptable_coding();
    // Keyed on the client, not the hop: behind a trusted proxy every request
    // would otherwise share the proxy's single key. Resolved once per request
    // and bound to the services every context of this request is created from.
    const auto connection = services.resolve_conn_info(request);
    const auto client_address = connection.client().address();

    const auto* resolved = input.resolution_.resolved();
    http_request_admission admission(
        http_response_tail(request, coding_policy, client_address, input.start_), services);
    admission.services_.bind_conn_info(connection);
    admission.services_.bind_response_coding(input.coding_);
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
    if (input.expectation_ != nullptr &&
        *input.expectation_ == http_server_expectation_plan::reject_unsupported) {
        return reject(http_request_endpoint::rejection,
            http_request_rejection::error(
                copy_http_protocol_error_info(input.resource_, http_unsupported_expectation_error())));
    }
    if (policy.apply_rate_limit_) {
        if (const auto rejection = decide_request_rate_limit(admission.services_.rate_limiter(), client_address)) {
            return reject(http_request_endpoint::rejection, http_request_rejection::rate_limited(*rejection));
        }
    }
    // Content already in hand (buffered bodies and subrequests) is held
    // to the same ceiling a streaming protocol enforces while reading.
    if (admission.body_limit_.exceeds(request.body_bytes().size())) {
        return reject(http_request_endpoint::rejection,
            http_request_rejection::error(
                copy_http_protocol_error_info(input.resource_, http_request_body_protocol_error(http_request_body_failure::too_large))));
    }
    if (endpoint == nullptr) {
        admission.endpoint_ = http_request_endpoint::buffered;
        return admission;
    }
    if (const auto* tunnel = endpoint->tunnel()) {
        admission.udp_tunnel_ = tunnel->protocol() == "connect-udp";
        if (admission.udp_tunnel_ && validate_http_connect_udp_request(request).has_value()) {
            return reject(http_request_endpoint::rejection,
                http_request_rejection::error(http_error_info(
                    {.status_ = http_status::bad_request, .message_ = "invalid CONNECT-UDP request head"})));
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

task<http_request_admission> admit_and_dispatch_buffered_request(const route_table& routes,
    const http_request_admission_input& input, const http_request_admission_policy& policy,
    const context_services& services, request_memory& memory, http_response& response) {
    auto admission = admit_http_request(input, policy, services);
    if (const auto* rejection = admission.rejection()) {
        // As in the server: a rejection does not inherit the coding promise.
        admission.tail().disable_response_coding();
        response = co_await respond_http_request_rejection(
            routes, input.request_, memory, *rejection, admission.services());
    } else {
        co_await routes.dispatch_buffered_response(input.request_, input.resolution_, memory,
            document_root_binding::none(), admission.services(), admission.static_file_mode(), response);
    }
    co_return admission;
}
}  // namespace ruvia::detail
