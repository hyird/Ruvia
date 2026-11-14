#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

#include "http_content_codec.h"
#include "http_content_coding.h"
#include "http_content_negotiation.h"
#include "http_error.h"
#include "http_message.h"
#include "http_request.h"
#include "http_response.h"
#include "http_response_body.h"
#include "http_server.h"
#include "stop_token.h"
#include "task.h"
#include "websocket_deflate_codecs.h"
#include "worker_ref.h"

namespace ruvia::detail {

using stream_encoder_lease = http_stream_encoder_lease;

class worker_codecs final {
public:
    worker_codecs() noexcept = default;
    ~worker_codecs() = default;
    worker_codecs(const worker_codecs&) = delete;
    worker_codecs& operator=(const worker_codecs&) = delete;

    [[nodiscard]] http_content_encode_context& encoder(http_content_coding coding) {
        return content_codecs_.encoder(coding);
    }

    [[nodiscard]] stream_encoder_lease acquire_stream_encoder(http_content_coding coding) {
        return content_codecs_.acquire_stream_encoder(coding);
    }

    [[nodiscard]] websocket_deflate_codecs& websocket_deflate() noexcept {
        return websocket_deflate_;
    }

private:
    http_worker_codecs content_codecs_;
    websocket_deflate_codecs websocket_deflate_;
};

// Runtime capability is deliberately separate from Accept-Encoding
// negotiation. A precompressed file sidecar can satisfy a selected coding
// without this process owning an incremental encoder.
enum class http_response_coding_availability : std::uint8_t {
    identity_only,
    identity_and_compression,
};

// Static-file representations remain owned by the static response path.
enum class http_response_compression_source : std::uint8_t {
    buffered,
    stream,
};

enum class http_response_compression_decision : std::uint8_t {
    fixed_representation,
    negotiated_identity,
    encode,
};

// Compression policy and encoder execution are separate outcomes. A response
// may intentionally remain identity (body too small while identity is
// acceptable, no-transform, integrity digest fields, an incompressible media
// type, or an unsupported runtime capability), or the encoder may actually
// fail. Those cases have different meanings when the client has forbidden
// identity and must not collapse back to a bool.
enum class http_response_compression_status : std::uint8_t {
    compressed,
    not_applicable,
    unavailable,
    failed,
};

// The 406 every coding-policy path answers when the client accepts none of the
// codings a response can be sent with. Options rather than http_error_info so
// a throw site can build an http_error from the same value.
[[nodiscard]] constexpr http_error_info_options http_not_acceptable_coding_error() noexcept {
    return {.status_ = http_status::not_acceptable, .code_ = "not_acceptable", .message_ = "no acceptable response content coding"};
}

// Return true when the response cannot satisfy the client's coding policy:
// the negotiation failed, a framework-selected coding fell back to forbidden
// identity, or a handler supplied a pre-encoded representation the client
// excluded. Bodyless statuses are representation-free and therefore never
// need a content-coding fallback.
[[nodiscard]] inline bool http_response_coding_fallback_forbidden(const http_response_coding_selection& selection, http_known_method request_method, const http_response& response, const http_request* request = nullptr) {
    if (!plan_http_response_body(request_method, response.status()).status_allows_body()) {
        return false;
    }
    // An empty 416 carries no content and so no coding to negotiate; its
    // Content-Range describes the selected representation (RFC 9110 15.5.17).
    if (response.status() == http_status::range_not_satisfiable && response.body_bytes().empty()) {
        return false;
    }
    switch (evaluate_http_response_content_coding(selection, response, request)) {
        case http_response_content_coding_status::accepted:
            return false;
        case http_response_content_coding_status::not_acceptable:
        case http_response_content_coding_status::invalid:
            return true;
        case http_response_content_coding_status::absent:
            break;
    }
    if (!selection.acceptable()) {
        return true;
    }
    if (selection.coding() == http_content_coding::identity || selection.identity_accepted()) {
        return false;
    }
    // This guard targets only the framework's silent identity fallback.
    return true;
}

// Outcome of the synchronous compression phase for an in-memory body. An
// eligible body above sync_bytes_ on a worker with a blocking pool is left as
// identity and flagged for offload; every other outcome is final here.
struct http_buffered_compression_step final {
    http_response_compression_status status_;
    bool offload_;
};

// Applies the buffered compression policy without suspending: eligibility,
// size bounds, and encoding up to sync_bytes_. sync_bytes_ is the inline
// ceiling: a larger body is flagged for offload when `offload_available`, and
// otherwise stays identity (not_applicable). Only an offload result requires
// compress_response_offloaded(), so identity and ineligible responses never
// create a coroutine frame. The inline encoder is the worker's reusable one:
// its state stays in worker and only the encoded body comes from the
// response's resource.
[[nodiscard]] http_buffered_compression_step apply_response_compression(const http_response_coding_selection& selection, http_known_method request_method, http_response& response, const compression_config& options, worker_codecs& codecs, bool offload_available);

// Encodes the body flagged by apply_response_compression() on the worker's
// bounded blocking pool with a codec of its own, since worker codecs never leave the
// worker. Rejection by the pool is an intentional identity fallback;
// encoder/commit failures remain typed failures so a client that forbids
// identity receives a terminal error.
[[nodiscard]] task<http_response_compression_status> compress_response_offloaded(http_response_coding_selection selection, http_response& response, const worker_ref& worker_value, const stop_token& stop_token_value = {});

// The single decimal Content-Length a handler declared, or nullopt.
[[nodiscard]] std::optional<std::uint64_t> declared_content_length(const http_response& response) noexcept;

// The size rule for a body of `declared` bytes, shared by buffered bodies,
// HEAD metadata and streams whose head declares a length: below min_bytes_
// (or empty) stays identity unless identity is refused, and above max_bytes_
// always stays identity.
[[nodiscard]] bool declared_length_permits_encoding(const http_response_coding_selection& selection, const compression_config& options, std::uint64_t declared) noexcept;

// Response compression policy. Fixed representations do not vary by coding,
// except an application-encoded body, whose 200-or-406 outcome does; eligible
// representations are annotated before deciding between identity and an
// encoder. Encoder availability never changes Accept-Encoding parsing.
[[nodiscard]] http_response_compression_decision prepare_response_compression(const http_response_coding_selection& selection, http_known_method request_method, http_response& response, http_response_compression_source source_value, http_response_coding_availability availability);

class http_response_coding_policy_disabled final {};

// The response lifecycle has a state that is different from HTTP negotiation
// failure: after a generated terminal error, coding application is deliberately
// disabled. That state stays distinct from the negotiation result, so "406
// negotiation" and "do not transform this error again" are never conflated.
class http_response_coding_policy final {
public:
    [[nodiscard]] static http_response_coding_policy disabled() noexcept {
        return http_response_coding_policy(http_response_coding_policy_disabled{});
    }

    // A failed negotiation stays alive in the selection until a buffered
    // handler has produced its response status: a 204/304 has no response
    // content, so rejecting it before dispatch would turn a representation-free
    // response into a false 406. The selection's identity coding is then only
    // a placeholder that never becomes a silent identity fallback for a
    // response that actually has a body.
    [[nodiscard]] static http_response_coding_policy selected(http_response_coding_selection selection) noexcept {
        return http_response_coding_policy(selection);
    }

    [[nodiscard]] const http_response_coding_selection* selection() const& noexcept {
        return std::get_if<http_response_coding_selection>(&value_);
    }
    const http_response_coding_selection* selection() const&& = delete;

private:
    explicit http_response_coding_policy(http_response_coding_policy_disabled disabled) noexcept :
        value_(disabled) {}

    explicit http_response_coding_policy(http_response_coding_selection selection) noexcept :
        value_(selection) {}

    using value_type = std::variant<http_response_coding_selection, http_response_coding_policy_disabled>;
    value_type value_;
};

// A selected non-identity coding is only a promise until the response policy
// actually installs Content-Encoding. In particular, no-transform,
// incompressible media, or an existing file body can leave the response as
// identity. If the client explicitly excluded identity, that policy fallback
// is a 406 outcome; an encoder failure is surfaced separately as a 500.
[[nodiscard]] inline bool http_response_needs_not_acceptable(const http_response_coding_policy& policy, const http_request& request, const http_response& response) noexcept {
    const auto* selection = policy.selection();
    if (selection == nullptr) {
        return false;
    }
    return http_response_coding_fallback_forbidden(*selection, request.known_method(), response, &request);
}

[[nodiscard]] inline std::optional<http_error_info> http_buffered_response_preparation_error(const http_response_coding_policy& policy, const http_request& request, const http_response& response, http_response_compression_status compression) noexcept {
    const auto* selection = policy.selection();
    if (selection != nullptr && selection->coding() != http_content_coding::identity && !selection->identity_accepted() && plan_http_response_body(request.known_method(), response.status()).status_allows_body()) {
        if (compression == http_response_compression_status::failed) {
            return http_error_info({.status_ = http_status::internal_server_error, .code_ = "response_compression_failed", .message_ = "response compression failed"});
        }
        if (compression == http_response_compression_status::unavailable) {
            return http_error_info({.status_ = http_status::service_unavailable, .code_ = "blocking_pool_unavailable", .message_ = "blocking pool cannot accept the operation"});
        }
    }
    if (http_response_needs_not_acceptable(policy, request, response)) {
        return http_error_info(http_not_acceptable_coding_error());
    }
    return std::nullopt;
}

enum class buffered_response_recovery_action { ready,
    respond_error,
    prepare_terminal };

struct buffered_response_recovery_step final {
    buffered_response_recovery_action action_{buffered_response_recovery_action::ready};
    std::optional<http_error_info> error_;
};

// A secondary failure preserves the error handler's representation and
// disables coding instead of invoking it again. An error response (4xx/5xx)
// goes straight to that terminal path: it is not a representation of the
// target resource (RFC 9110 15.5.7), so the server may disregard
// Accept-Encoding for it (RFC 9110 12.4.1). Replacing it with a 406 would
// disguise the original failure and run the error handler a second time.
class buffered_response_recovery final {
public:
    [[nodiscard]] bool recovered() const noexcept {
        return stage_ != stage::application;
    }

    [[nodiscard]] buffered_response_recovery_step advance(http_response_coding_policy& policy, const http_request& request, const http_response& response, http_response_compression_status compression) noexcept {
        if (stage_ == stage::terminal) {
            return {};
        }
        auto error = http_buffered_response_preparation_error(policy, request, response, compression);
        if (!error.has_value()) {
            return {};
        }
        if (stage_ == stage::application && !response.status().is_error()) {
            stage_ = stage::negotiated_error;
            return {buffered_response_recovery_action::respond_error, std::move(error)};
        }
        policy = http_response_coding_policy::disabled();
        stage_ = stage::terminal;
        return {buffered_response_recovery_action::prepare_terminal, std::nullopt};
    }

private:
    enum class stage { application,
        negotiated_error,
        terminal };
    stage stage_{stage::application};
};

struct prepared_application_response final {
    http_buffered_response_write_plan write_plan_;
    bool recovered_;
};

}  // namespace ruvia::detail
