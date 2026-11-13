#include "server/http_request_admission.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_request_body_error.h"
#include "ruvia/http/http_target_origin.h"

#include "http/http_protocol_error_info.h"
#include "ratelimit/rate_limiter.h"
#include "router/route_table.h"
#include "server/http_connection_state.h"
#include "server/http_response_compression.h"
#include "tls/tls_served_certificate.h"

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

// RFC 9110 §7.4 against the transport connection's facts. A secured
// connection without a recorded certificate cannot vouch for any host.
[[nodiscard]] bool connection_carries_target(const http_request& request, const context_services& services) noexcept {
    const auto origin = decide_http_target_origin(request,
        {.secured_ = services.get_conn_info().tls() != nullptr, .trusted_gateway_ = services.peer_trusted()});
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

}  // namespace
http_request_admission_policy http_request_admission_policy::server(const http_server_options& options) noexcept {
    return {
        .max_stream_body_bytes_ = options.server_.max_stream_body_bytes_,
        .max_buffered_body_bytes_ = options.server_.max_buffered_body_bytes_,
        .apply_rate_limit_ = true,
        .verify_target_origin_ = true,
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

task<http_response> respond_http_request_rejection(const route_table& routes, const http_request& request, http_request_rejection rejection, const context_services& services) {
    auto response = co_await routes.handle_error(request, rejection.error(), services);
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
    http_request_admission admission(
        http_response_tail(request, coding_policy, {}, {}, input.start_), services);
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
    admission.body_limit_ = http_request_body_limit(input.resolution_,
        endpoint != nullptr ? endpoint->request_body_mode() : request_body_mode::buffered, policy);

    const auto reject = [&](http_request_rejection rejection) {
        admission.endpoint_ = http_request_endpoint::rejection;
        admission.rejection_ = rejection;
        return std::move(admission);
    };

    // Before any expectation, limit or endpoint decision: a request this
    // connection cannot carry is not this server's to evaluate.
    if (policy.verify_target_origin_ && !connection_carries_target(request, services)) {
        return reject(http_request_rejection::error(http_error_info(
            {.status_ = http_status::misdirected_request, .message_ = "misdirected request"})));
    }

    // Expect extensions are valid HTTP syntax. The protocol reports the
    // semantic fact; this product implements only 100-continue and chooses the
    // RFC 9110-permitted 417 before reading request content.
    if (input.expectation_ != nullptr &&
        *input.expectation_ == http_server_expectation_plan::reject_unsupported) {
        return reject(http_request_rejection::error(
            [&] { const auto err = http_unsupported_expectation_error(); return http_error_info({.status_ = err.status(), .code_ = err.code(), .message_ = std::string_view(err.what())}); }()));
    }
    if (policy.apply_rate_limit_ && !request_stream) {
        if (const auto rejection = decide_request_rate_limit(
                admission.services_.rate_limiter(), client_address, input.pinned_arrival_)) {
            return reject(http_request_rejection::rate_limited(*rejection));
        }
    }
    // Content already in hand (buffered bodies and subrequests) is held
    // to the same ceiling a streaming protocol enforces while reading.
    if (admission.body_limit_.exceeds(request.body_bytes().size())) {
        return reject(http_request_rejection::error(
            [&] { const auto err = http_request_body_protocol_error(http_request_body_failure::too_large); return http_error_info({.status_ = err.status(), .code_ = err.code(), .message_ = std::string_view(err.what())}); }()));
    }
    if (endpoint == nullptr) {
        admission.endpoint_ = http_request_endpoint::buffered;
        return admission;
    }
    if (const auto* tunnel = endpoint->tunnel()) {
        admission.udp_tunnel_ = tunnel->protocol() == "connect-udp" ||
                                (request_stream && (request.protocol() == "connect-udp" || is_http_connect_udp_upgrade_request(request)));
        if (admission.udp_tunnel_ && validate_http_connect_udp_request(request).has_value()) {
            return reject(http_request_rejection::error(http_error_info(
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
    const context_services& services, http_response& response) {
    auto admission = admit_http_request(input, policy, services);
    if (const auto* rejection = admission.rejection()) {
        // As in the server: a rejection does not inherit the coding promise.
        admission.tail().disable_response_coding();
        response = co_await respond_http_request_rejection(
            routes, input.request_, *rejection, admission.services());
    } else {
        co_await routes.dispatch_buffered_response(input.request_, input.resolution_,
            document_root_binding::none(), admission.services(), admission.static_file_mode(), response);
    }
    co_return admission;
}
}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

// A coded representation is worth sending only when strictly smaller than
// the identity bytes; the encoder stops at this cap instead of finishing.
// When the client refuses identity (RFC 9110 §12.5.3), any acceptable coding
// beats a 406, so the cap is lifted.
[[nodiscard]] http_content_encode_options compressed_body_options(
    std::string_view plain, bool identity_accepted) noexcept {
    if (!identity_accepted) {
        return {.max_encoded_bytes_ = (std::numeric_limits<std::size_t>::max)()};
    }
    return {.max_encoded_bytes_ = plain.empty() ? 0 : plain.size() - 1};
}

// Publishes an encoder result as the response representation. The commit
// stages every affected header before publishing the owned body, so a
// request-resource failure leaves the identity response usable for the typed
// 500 path instead of exposing a mixed body/metadata state.
[[nodiscard]] http_response_compression_status commit_compressed_body(
    http_response& response, http_content_coding coding, std::optional<std::string>&& encoding) {
    if (!encoding.has_value()) {
        return http_response_compression_status::not_applicable;
    }
    response.replace_body_with_content_encoding(std::move(*encoding), http_content_coding_token(coding));
    return http_response_compression_status::compressed;
}

[[nodiscard]] bool response_headers_skip_compression(const http_response& response) noexcept {
    if (response.header("Content-Range").has_value()) {
        return true;
    }
    if (const auto content_type = response.header("Content-Type");
        !content_type.has_value() || !is_http_media_type_compressible(*content_type)) {
        return true;
    }
    const bool has_cache_control = response.header("Cache-Control").has_value();
    cache_control_field_parser cache_parser;
    for (const auto& header : response.headers()) {
        const auto name = header.name();
        switch (name.size()) {
            case 11U:
                if (http_ascii_equals_ignore_case(name, "Repr-Digest") ||
                    http_ascii_equals_ignore_case(name, "Content-MD5")) {
                    return true;
                }
                break;
            case 13U:
                if (http_ascii_equals_ignore_case(name, "Cache-Control")) {
                    cache_parser.update(header.value());
                }
                break;
            case 14U:
                if ((name[8] | 0x20) == 'd' && http_ascii_equals_ignore_case(name, "Content-Digest")) {
                    return true;
                }
                break;
            default:
                break;
        }
    }
    return has_cache_control && cache_parser.finish().has(cache_control_directive::no_transform);
}

}  // namespace

std::optional<std::uint64_t> declared_content_length(const http_response& response) noexcept {
    const auto field = response.header("Content-Length");
    if (!field.has_value()) {
        return std::nullopt;
    }
    std::optional<std::uint64_t> declared;
    bool valid = true;
    http_visit_comma_separated_quoted_field_items(*field, [&](std::string_view item) noexcept {
        if (item.empty()) {
            valid = false;
            return false;
        }
        std::uint64_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(item.data(), item.data() + item.size(), parsed);
        if (ec != std::errc{} || ptr != item.data() + item.size() ||
            (declared.has_value() && *declared != parsed)) {
            valid = false;
            return false;
        }
        declared = parsed;
        return true;
    });
    return valid ? declared : std::nullopt;
}

bool declared_length_permits_encoding(const http_response_coding_selection& selection,
    const compression_config& options, std::uint64_t declared) noexcept {
    // min_bytes_ only says when encoding stops paying off; a client that
    // refuses identity is served a coding at any size instead of a 406.
    return !(((declared == 0 || declared < options.min_bytes_) && selection.identity_accepted()) ||
             declared > options.max_bytes_);
}

namespace {

// An empty HEAD body is a HEAD handler describing the GET representation
// without producing it. Encoding those zero bytes would publish the coded
// length of nothing (RFC 9110 §8.6). Apply the coding GET would select, sized
// by the declared Content-Length, and drop that length: the coded size is
// unknown and §8.6 allows omitting it.
[[nodiscard]] http_buffered_compression_step apply_head_content_encoding(
    const http_response_coding_selection& selection, http_response& response,
    const compression_config& options, std::optional<std::uint64_t> declared) {
    const bool encode = declared.has_value() ? declared_length_permits_encoding(selection, options, *declared)
                                             : !selection.identity_accepted();
    if (!encode) {
        return {http_response_compression_status::not_applicable, false};
    }
    response.apply_content_encoding(http_content_coding_token(selection.coding()));
    return {http_response_compression_status::compressed, false};
}

}  // namespace

http_response_compression_decision prepare_response_compression(
    const http_response_coding_selection& selection, http_known_method request_method,
    http_response& response, http_response_compression_source source_value,
    http_response_coding_availability availability) {
    // A file's bytes are served as the representation their validators and
    // HEAD response describe, whether the transport reads the file or the
    // context already carries them in memory.
    if (source_value == http_response_compression_source::buffered && response.has_file_representation()) {
        return http_response_compression_decision::fixed_representation;
    }
    if (!plan_http_response_body(request_method, response.status()).status_allows_body()) {
        return http_response_compression_decision::fixed_representation;
    }
    // An application-encoded body is kept, but whether it is sent (200) or
    // refused (406) still depends on Accept-Encoding, so caches must key on it
    // (RFC 9110 §12.5.5; Pingora depends_on_accept_encoding).
    if (response.header("Content-Encoding").has_value()) {
        response.add_vary_token("Accept-Encoding");
        return http_response_compression_decision::fixed_representation;
    }
    // Integrity fields describe the content or the selected representation,
    // both of which include the content coding (RFC 9530 §1, §2, §3; legacy
    // Content-MD5 likewise covered the sent content). Encoding here would
    // leave them describing bytes that are not sent.
    if (availability == http_response_coding_availability::identity_only ||
        response.status() == http_status::partial_content ||
        response_headers_skip_compression(response)) {
        return http_response_compression_decision::fixed_representation;
    }

    response.add_vary_token("Accept-Encoding");
    if (selection.coding() == http_content_coding::identity) {
        return http_response_compression_decision::negotiated_identity;
    }
    return http_response_compression_decision::encode;
}

http_buffered_compression_step apply_response_compression(const http_response_coding_selection& selection,
    http_known_method request_method, http_response& response, const compression_config& options,
    worker_codecs& codecs, bool offload_available) {
    constexpr http_buffered_compression_step not_applicable{http_response_compression_status::not_applicable, false};
    const auto body = response.body_bytes();

    const auto decision = prepare_response_compression(selection, request_method, response,
        http_response_compression_source::buffered,
        http_response_coding_availability::identity_and_compression);
    // Negotiation errors above still propagate; only encoding and publication
    // failures become a typed compression result.
    try {
        if (decision == http_response_compression_decision::encode && request_method == http_known_method::head &&
            body.empty() && (response.has_head_metadata_body() || response.header("Content-Length").has_value())) {
            const auto declared = declared_content_length(response);
            if (declared != std::uint64_t{0}) {
                return apply_head_content_encoding(selection, response, options, declared);
            }
        }
        if (decision != http_response_compression_decision::encode ||
            !declared_length_permits_encoding(selection, options, body.size())) {
            return not_applicable;
        }
        // sync_bytes_ is the inline ceiling. Larger bodies encode on the blocking
        // pool; without one they stay identity (or 406 when identity is refused)
        // rather than stall the event loop.
        if (body.size() > options.sync_bytes_) {
            return {http_response_compression_status::not_applicable, offload_available};
        }

        const auto coding = selection.coding();
        auto encoding = codecs.encoder(coding).encode(
            body, compressed_body_options(body, selection.identity_accepted()));
        return {commit_compressed_body(response, coding, std::move(encoding)), false};
    } catch (...) {
        ruvia::report_unhandled_failure("response compression", std::current_exception());
        return {http_response_compression_status::failed, false};
    }
}

task<http_response_compression_status> compress_response_offloaded(http_response_coding_selection selection,
    http_response& response, const worker_ref& worker_value, const stop_token& stop_token_value) {
    try {
        const auto coding = selection.coding();
        const bool identity_accepted = selection.identity_accepted();
        std::string plain(response.body_bytes());
        auto result_value = co_await run_blocking(worker_value, {.stop_token_ = stop_token_value},
            [coding, identity_accepted, plain = std::move(plain)]() {
                return encode_http_content(
                    coding, plain, compressed_body_options(plain, identity_accepted));
            });
        if (result_value.failed()) {
            ruvia::report_unhandled_failure("response compression", result_value.error());
            co_return http_response_compression_status::failed;
        }
        if (!result_value.completed()) {
            // Queue saturation and shutdown are overload/lifecycle outcomes,
            // not broken encoders. Preserve the identity representation when
            // identity is acceptable; otherwise report 503 Service Unavailable.
            co_return http_response_compression_status::unavailable;
        }
        co_return commit_compressed_body(response, coding, std::move(result_value).take_value());
    } catch (...) {
        ruvia::report_unhandled_failure("response compression", std::current_exception());
        // Input ownership, encoder execution and representation publication
        // leave the original identity body intact on failure.
        co_return http_response_compression_status::failed;
    }
}

}  // namespace ruvia::detail
