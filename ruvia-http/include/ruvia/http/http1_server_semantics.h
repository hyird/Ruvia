#pragma once

#include <cstdint>
#include <exception>
#include <optional>
#include <utility>
#include <variant>

#include "ruvia/http/http1_close_policy.h"
#include "ruvia/http/http1_request_connection_plan.h"
#include "ruvia/http/http1_response_head_plan.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_stream.h"

namespace ruvia {

class http1_response_stream_plan final {
public:
    [[nodiscard]] http_response_stream_framing framing() const noexcept {
        return framing_;
    }

    [[nodiscard]] http1_request_connection_plan request_connection_plan() const noexcept {
        return request_connection_plan_;
    }

    [[nodiscard]] http1_close_policy close_policy() const noexcept {
        return close_policy_;
    }

    [[nodiscard]] http_known_method request_method() const noexcept {
        return request_method_;
    }

private:
    friend http1_response_stream_plan http1_plan_response_stream(
        const http1_server_request_parse_state&, http1_close_policy) noexcept;
    friend http1_response_stream_plan http1_plan_consumed_response_stream(
        const http1_server_request_parse_state&, http1_close_policy) noexcept;

    http1_response_stream_plan(http_response_stream_framing framing,
        http1_request_connection_plan request_connection_plan, http1_close_policy close_policy,
        http_known_method request_method) noexcept
        : framing_(framing),
          request_connection_plan_(request_connection_plan),
          close_policy_(close_policy),
          request_method_(request_method) {}

    http_response_stream_framing framing_;
    http1_request_connection_plan request_connection_plan_;
    http1_close_policy close_policy_{http1_close_policy::close_after_response};
    http_known_method request_method_{http_known_method::unknown};
};

// Pure HTTP/1 response-stream planning. The runtime contributes only its typed
// product policy (for example, a per-connection request limit); HTTP retains the
// request disposition and candidate framing until the response status is known.
// Commit can therefore distinguish a body-allowed HTTP/1.0 stream, which requires
// close delimiting, from a body-suppressed response that is already self-delimited.
[[nodiscard]] inline http1_response_stream_plan http1_plan_response_stream(
    const http1_server_request_parse_state& parsed_value, http1_close_policy close_policy) noexcept {
    const auto request_connection_plan = apply_request_body_consumption(parsed_value.connection_plan_,
        parsed_value.body_plan_.requires_consumption() ? http1_request_body_consumption::incomplete
                                                       : http1_request_body_consumption::complete);
    const auto framing = parsed_value.request_.protocol_version() == http_protocol_version::http11
                             ? http_response_stream_framing::http1_chunked
                             : http_response_stream_framing::http1_close_delimited;
    return http1_response_stream_plan(
        framing, request_connection_plan, close_policy, parsed_value.request_.known_method());
}

// Runtime variant for a route that buffered and consumed the complete request
// body before deciding to stream its response. The parser's body plan still
// describes the original request framing, so this named entry point records the
// completed consumption instead of pessimistically forcing connection close.
[[nodiscard]] inline http1_response_stream_plan http1_plan_consumed_response_stream(
    const http1_server_request_parse_state& parsed_value, http1_close_policy close_policy) noexcept {
    const auto framing = parsed_value.request_.protocol_version() == http_protocol_version::http11
                             ? http_response_stream_framing::http1_chunked
                             : http_response_stream_framing::http1_close_delimited;
    return http1_response_stream_plan(
        framing, parsed_value.connection_plan_, close_policy, parsed_value.request_.known_method());
}

enum class http1_connection_close_field_policy : std::uint8_t { close_only,
    preserve_upgrade };

inline void http1_mark_connection_close(http_response& response,
    http1_connection_close_field_policy field_policy = http1_connection_close_field_policy::close_only) {
    // A runtime close verdict dominates keep-alive. Collapse repeated fields
    // after the socket lifecycle is decided, while preserving the Upgrade
    // option required by any retained Upgrade field.
    response.remove_header("Connection");
    response.header_stable_view("Connection",
        field_policy == http1_connection_close_field_policy::preserve_upgrade ? "close, Upgrade"
                                                                              : "close");
}

namespace detail {
enum class http1_final_response_control_plan_error : std::uint8_t;
}  // namespace detail

class prepared_http1_response_stream;

class http1_final_response_commit_error final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override;

private:
    friend http1_request_connection_plan http1_commit_final_response(
        http_response&, http1_request_connection_plan);

    explicit http1_final_response_commit_error(detail::http1_final_response_control_plan_error error) noexcept
        : error_(error) {}

    detail::http1_final_response_control_plan_error error_;
};

// Commit response-side HTTP/1 persistence after the runtime has folded in
// request-body completion and server policy. This is the sole protocol mutation:
// it honors an application-provided Connection: close and emits the version-
// appropriate Connection field. Success retains the exact request version for
// head serialization; wire-message failures remain typed.
[[nodiscard]] http1_request_connection_plan http1_commit_final_response(
    http_response& response, http1_request_connection_plan plan);

// Commit-time result. This object combines request/version/runtime constraints with
// the response method/status and Connection options, then binds the exact header
// bytes to the authoritative connection disposition.
class prepared_http1_response_stream final {
public:
    [[nodiscard]] http_response& response() & noexcept {
        return head_.response();
    }
    [[nodiscard]] http_response& response() && = delete;

    [[nodiscard]] const http_response& response() const& noexcept {
        return head_.response();
    }
    [[nodiscard]] const http_response& response() const&& = delete;

    [[nodiscard]] const http1_response_head_plan& response_head_plan() const& noexcept {
        return response_head_plan_;
    }
    [[nodiscard]] const http1_response_head_plan& response_head_plan() const&& = delete;

    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const& noexcept {
        return head_.commit_plan();
    }
    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const&& = delete;

    [[nodiscard]] http1_request_connection_plan connection_plan() const noexcept {
        return connection_plan_;
    }

private:
    friend prepared_http1_response_stream prepare_http1_response_stream_head(
        http_response, const http1_response_stream_plan&, http_response_trailer_intent);

    prepared_http1_response_stream(http_response_stream_head head, http1_response_head_plan response_head_plan,
        http1_request_connection_plan connection_plan) noexcept
        : head_(std::move(head)),
          response_head_plan_(response_head_plan),
          connection_plan_(connection_plan) {}

    http_response_stream_head head_;
    http1_response_head_plan response_head_plan_;
    http1_request_connection_plan connection_plan_;
};

[[nodiscard]] prepared_http1_response_stream prepare_http1_response_stream_head(
    http_response response, const http1_response_stream_plan& plan,
    http_response_trailer_intent trailer_intent = http_response_trailer_intent::none);

}  // namespace ruvia
