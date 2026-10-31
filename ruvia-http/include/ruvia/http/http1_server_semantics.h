#pragma once

#include <cstdint>
#include <exception>
#include <optional>
#include <utility>
#include <variant>

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
        const http1_server_request_parse_state&, http1_close_policy, http1_request_body_consumption) noexcept;

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
// product policy (for example, a per-connection request limit) and the request
// body's consumption state; HTTP retains the request disposition and candidate
// framing until the response status is known. Commit can therefore distinguish
// a body-allowed HTTP/1.0 stream, which requires close delimiting, from a
// body-suppressed response that is already self-delimited. A route that consumed
// the complete request body before streaming passes `complete`; the parser's
// body plan still describes the original request framing, so planning for an
// unread body passes `body_plan_.consumption_if_unread()`.
[[nodiscard]] inline http1_response_stream_plan http1_plan_response_stream(
    const http1_server_request_parse_state& parsed_value, http1_close_policy close_policy,
    http1_request_body_consumption consumption) noexcept {
    const auto framing = parsed_value.request_.protocol_version() == http_protocol_version::http11
                             ? http_response_stream_framing::http1_chunked
                             : http_response_stream_framing::http1_close_delimited;
    return http1_response_stream_plan(framing,
        apply_request_body_consumption(parsed_value.connection_plan_, consumption), close_policy,
        parsed_value.request_.known_method());
}

enum class http1_connection_close_field_policy : std::uint8_t { close_only,
    preserve_upgrade };

inline void http1_mark_connection_close(http_response& response,
    http1_connection_close_field_policy field_policy = http1_connection_close_field_policy::close_only) {
    // A runtime close verdict dominates keep-alive. Collapse repeated fields
    // after the socket lifecycle is decided, while preserving the Upgrade
    // option required by any retained Upgrade field and any custom hop-by-hop
    // field names declared in Connection (RFC 9110 section 7.6.1).
    const std::string_view base =
        field_policy == http1_connection_close_field_policy::preserve_upgrade ? "close, Upgrade"
                                                                              : "close";
    std::pmr::string merged(response.memory_resource());
    for (const auto& header : response.headers()) {
        if (!http_ascii_equals_ignore_case(header.name(), "Connection")) {
            continue;
        }
        const auto field_value = header.value();
        std::size_t start = 0;
        while (start <= field_value.size()) {
            const auto comma = field_value.find(',', start);
            const auto end = comma == std::string_view::npos ? field_value.size() : comma;
            const auto option = http_trim_ows(field_value.substr(start, end - start));
            if (!option.empty() && !http_ascii_equals_ignore_case(option, "close") &&
                !http_ascii_equals_ignore_case(option, "keep-alive") &&
                !http_ascii_equals_ignore_case(option, "Upgrade")) {
                if (merged.empty()) {
                    merged.assign(base);
                }
                merged.append(", ");
                merged.append(option);
            }
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }
    }
    response.remove_header("Connection");
    if (merged.empty()) {
        response.header_stable_view("Connection", base);
    } else {
        response.header("Connection", std::move(merged));
    }
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

// The successful head ends HTTP framing and transfers the connection to a
// duplex byte stream. Payload belongs to that stream, not to an HTTP response.
[[nodiscard]] http1_response_head_plan prepare_http1_connect_response_head(
    const http_response& response, http_protocol_version version);

}  // namespace ruvia
