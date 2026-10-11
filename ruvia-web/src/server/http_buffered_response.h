#pragma once

#include <exception>
#include <optional>
#include <utility>
#include <variant>

#include "ruvia/core/stop_token.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/web/error.h"

#include "server/http_response_compression.h"

namespace ruvia::detail {

class http_response_coding_policy_disabled final {};

// The response lifecycle has a state that is different from HTTP negotiation
// failure: after a generated terminal error, coding application is deliberately
// disabled. Keeping that state distinct from the protocol result removes the
// old optional/reset channel that conflated "406 negotiation" with "do not
// transform this error again".
class http_response_coding_policy final {
public:
    [[nodiscard]] static http_response_coding_policy disabled() noexcept {
        return http_response_coding_policy(http_response_coding_policy_disabled{});
    }

    [[nodiscard]] static http_response_coding_policy selected(
        http_response_coding_selection selection) noexcept {
        return http_response_coding_policy(selection);
    }

    // Keep a negotiation failure alive until a buffered handler has produced
    // its response status. A 204/304 has no response content, so rejecting it
    // before dispatch would turn a representation-free response into a false
    // 406. The identity selection is only a dispatch placeholder; the failure
    // bit below prevents it from becoming a silent identity fallback for a
    // response that actually has a body.
    [[nodiscard]] static http_response_coding_policy no_acceptable_coding() noexcept {
        http_response_coding_qualities absent_header;
        const auto fallback = http_response_coding_selection::select(absent_header);
        const auto* selection = fallback.selected();
        if (selection == nullptr) {
            std::terminate();
        }
        return http_response_coding_policy(*selection, true);
    }

    [[nodiscard]] const http_response_coding_selection* selection() const& noexcept {
        return std::get_if<http_response_coding_selection>(&value_);
    }
    const http_response_coding_selection* selection() const&& = delete;

    [[nodiscard]] bool negotiation_failed() const noexcept {
        return negotiation_failed_;
    }

private:
    explicit http_response_coding_policy(http_response_coding_policy_disabled disabled) noexcept
        : value_(disabled) {}

    explicit http_response_coding_policy(
        http_response_coding_selection selection, bool negotiation_failed = false) noexcept
        : value_(selection),
          negotiation_failed_(negotiation_failed) {}

    using value_type = std::variant<http_response_coding_selection, http_response_coding_policy_disabled>;
    value_type value_;
    bool negotiation_failed_{false};
};

// A selected non-identity coding is only a promise until the response policy
// actually installs Content-Encoding. In particular, no-transform,
// incompressible media, or an existing file body can leave the response as
// identity. If the client explicitly excluded identity, that policy fallback
// is a 406 outcome; an encoder failure is surfaced separately as a 500.
[[nodiscard]] inline bool http_response_needs_not_acceptable(const http_response_coding_policy& policy,
    const http_request& request, const http_response& response) noexcept {
    const auto* selection = policy.selection();
    if (selection == nullptr) {
        return false;
    }
    if (policy.negotiation_failed()) {
        // The policy carries the client's empty acceptable set. Only a
        // response status that permits content can violate it; 204/205/304
        // are representation-free and must not be rejected before the
        // handler's final status is known.
        return plan_http_response_body(request.known_method(), response.status()).status_allows_body();
    }
    return http_response_coding_fallback_forbidden(*selection, request.known_method(), response);
}

[[nodiscard]] inline std::optional<http_error_info> http_buffered_response_preparation_error(
    const http_response_coding_policy& policy, const http_request& request,
    const http_response& response, const http_response_compression_result& compression_result) noexcept {
    const auto* selection = policy.selection();
    if (selection != nullptr && compression_result.failed() &&
        selection->coding() != http_content_coding::identity && !selection->identity_accepted() &&
        plan_http_response_body(request.known_method(), response.status()).status_allows_body()) {
        return http_error_info({.status_ = http_status::internal_server_error,
            .code_ = "response_compression_failed",
            .message_ = "response compression failed"});
    }
    if (http_response_needs_not_acceptable(policy, request, response)) {
        return http_error_info({.status_ = http_status::not_acceptable,
            .code_ = "not_acceptable",
            .message_ = "no acceptable response content coding"});
    }
    return std::nullopt;
}

enum class buffered_response_recovery_mode { negotiated_then_disabled,
    immediately_disabled };
enum class buffered_response_recovery_action { ready,
    handle_error,
    prepare_terminal };

struct buffered_response_recovery_step final {
    buffered_response_recovery_action action_{buffered_response_recovery_action::ready};
    std::optional<http_error_info> error_;
};

// Shared Web policy. A secondary failure preserves the error handler's
// representation and disables coding instead of invoking it again. The shared
// application operation owns preparation and suspension; each protocol owns commit.
class buffered_response_recovery final {
public:
    explicit buffered_response_recovery(
        buffered_response_recovery_mode mode = buffered_response_recovery_mode::negotiated_then_disabled) noexcept
        : mode_(mode) {}

    [[nodiscard]] bool recovered() const noexcept {
        return stage_ != stage::application;
    }

    [[nodiscard]] buffered_response_recovery_step advance(http_response_coding_policy& policy,
        const http_request& request, const http_response& response,
        const http_response_compression_result& compression_result) noexcept {
        if (stage_ == stage::terminal) {
            return {};
        }
        auto error = http_buffered_response_preparation_error(policy, request, response, compression_result);
        if (!error.has_value()) {
            return {};
        }
        if (stage_ == stage::application) {
            if (mode_ == buffered_response_recovery_mode::immediately_disabled) {
                policy = http_response_coding_policy::disabled();
                stage_ = stage::terminal;
            } else {
                stage_ = stage::negotiated_error;
            }
            return {buffered_response_recovery_action::handle_error, std::move(error)};
        }
        policy = http_response_coding_policy::disabled();
        stage_ = stage::terminal;
        return {buffered_response_recovery_action::prepare_terminal, std::nullopt};
    }

private:
    enum class stage { application,
        negotiated_error,
        terminal };
    buffered_response_recovery_mode mode_;
    stage stage_{stage::application};
};

struct prepared_application_response final {
    http_buffered_response_write_plan write_plan_;
    bool recovered_;
};

struct application_response_control final {
    // Terminal transport cancellation is distinct from an application deadline
    // whose error response may still be deliverable. The token is borrowed.
    const stop_token* terminal_stop_{};
    buffered_response_recovery_mode recovery_mode_{buffered_response_recovery_mode::negotiated_then_disabled};
};

}  // namespace ruvia::detail
