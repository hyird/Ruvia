#pragma once

#include <cstdint>
#include <exception>
#include <optional>
#include <utility>
#include <variant>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/http1/http1_server_request_parser.h"
#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/detail/server/http_final_response_control_plan.h"
#include "ruvia/http/http1_close_policy.h"
#include "ruvia/http/http1_request_connection_plan.h"
#include "ruvia/http/http1_response_head_plan.h"
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

class http1_final_response_commit_result;
class http1_final_response_commit_failure;

class http1_final_response_commit_error final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override {
        switch (error_) {
            case detail::http1_final_response_control_plan_error::invalid_status:
                return "invalid final HTTP response status";
            case detail::http1_final_response_control_plan_error::invalid_connection_field:
                return "invalid HTTP Connection header";
            case detail::http1_final_response_control_plan_error::invalid_upgrade_field:
                return "invalid HTTP Upgrade header";
            case detail::http1_final_response_control_plan_error::upgrade_required:
                return "Upgrade Required response requires an Upgrade protocol";
            case detail::http1_final_response_control_plan_error::te_field_forbidden:
                return "TE is not a response field";
        }
        return "unknown HTTP final response commit failure";
    }

private:
    friend class http1_final_response_commit_failure;

    explicit http1_final_response_commit_error(detail::http1_final_response_control_plan_error error) noexcept
        : error_(error) {}

    detail::http1_final_response_control_plan_error error_;
};

class http1_final_response_commit_failure final {
public:
    [[nodiscard]] http1_final_response_commit_error exception() const noexcept {
        return http1_final_response_commit_error(error_);
    }

private:
    friend class http1_final_response_commit_result;

    explicit http1_final_response_commit_failure(
        const detail::http1_final_response_control_plan_failure& failure) noexcept
        : error_(failure.error()) {}

    detail::http1_final_response_control_plan_error error_;
};

// A final response commit directly owns the authoritative connection contract
// or one typed message failure. Validation completes before Connection is
// mutated, so callers cannot observe a half-committed response after a protocol
// failure or unwrap a second success container.
class http1_final_response_commit_result final {
public:
    [[nodiscard]] const http1_request_connection_plan* committed() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    [[nodiscard]] const http1_request_connection_plan* committed() const&& = delete;

    [[nodiscard]] const http1_final_response_commit_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    [[nodiscard]] const http1_final_response_commit_failure* failure() const&& = delete;

private:
    friend http1_final_response_commit_result http1_commit_final_response(
        http_response&, http1_request_connection_plan);

    using value_type = std::variant<http1_request_connection_plan, http1_final_response_commit_failure>;

    explicit http1_final_response_commit_result(http1_request_connection_plan connection_plan) noexcept
        : value_(connection_plan) {}

    explicit http1_final_response_commit_result(http1_final_response_commit_failure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static http1_final_response_commit_result committed(
        http1_request_connection_plan connection_plan) noexcept {
        return http1_final_response_commit_result(connection_plan);
    }

    [[nodiscard]] static http1_final_response_commit_result failure(
        const detail::http1_final_response_control_plan_failure& failure) noexcept {
        return http1_final_response_commit_result(http1_final_response_commit_failure(failure));
    }

    value_type value_;
};

// Commit response-side HTTP/1 persistence after the runtime has folded in
// request-body completion and server policy. This is the sole protocol mutation:
// it honors an application-provided Connection: close and emits the version-
// appropriate Connection field. Success retains the exact request version for
// head serialization; wire-message failures remain typed.
[[nodiscard]] inline http1_final_response_commit_result http1_commit_final_response(
    http_response& response, http1_request_connection_plan plan) {
    const auto control_result = detail::http1_final_response_control_plan(response);
    if (const auto* failure = control_result.failure()) {
        return http1_final_response_commit_result::failure(*failure);
    }
    const auto& http1_control = *control_result.control();
    const auto response_options = http1_control.connection_options();
    const auto upgrade_protocols = http1_control.upgrade_protocols();
    const bool preserve_upgrade = upgrade_protocols.has_field();
    const bool generate_upgrade_option = preserve_upgrade && !response_options.upgrade();
    if (generate_upgrade_option) {
        if (response_options.has_field()) {
            response.header("Connection", "Upgrade",
                http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
        } else {
            response.header_stable_view("Connection", "Upgrade");
        }
    }
    if (response_options.close()) {
        plan = plan.require_close();
    }
    if (plan.disposition() == http1_close_policy::close_after_response) {
        http1_mark_connection_close(response, preserve_upgrade
                                                  ? http1_connection_close_field_policy::preserve_upgrade
                                                  : http1_connection_close_field_policy::close_only);
    } else if (plan.protocol_version() == http_protocol_version::http10 &&
               !response_options.keep_alive()) {
        if (response_options.has_field() || generate_upgrade_option) {
            response.header("Connection", "keep-alive",
                http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
        } else {
            response.header_stable_view("Connection", "keep-alive");
        }
    }
    return http1_final_response_commit_result::committed(plan);
}

class prepared_http1_response_stream_result;

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
    friend class prepared_http1_response_stream_result;
    friend prepared_http1_response_stream_result prepare_http1_response_stream_head(
        http_response, http_response_stream_kind, const http1_response_stream_plan&, http_response_trailer_intent);
    friend prepared_http1_response_stream_result prepare_http1_known_length_response_stream_head(
        http_response, std::uint64_t, http_response_stream_kind, const http1_response_stream_plan&);

    prepared_http1_response_stream(http_response_stream_head head, http1_response_head_plan response_head_plan,
        http1_request_connection_plan connection_plan) noexcept
        : head_(std::move(head)),
          response_head_plan_(response_head_plan),
          connection_plan_(connection_plan) {}

    http_response_stream_head head_;
    http1_response_head_plan response_head_plan_;
    http1_request_connection_plan connection_plan_;
};

class prepared_http1_response_stream_result final {
public:
    [[nodiscard]] const prepared_http1_response_stream* prepared() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    [[nodiscard]] const prepared_http1_response_stream* prepared() const&& = delete;

    [[nodiscard]] prepared_http1_response_stream* prepared() & noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    [[nodiscard]] prepared_http1_response_stream* prepared() && = delete;

    [[nodiscard]] const http1_final_response_commit_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    [[nodiscard]] const http1_final_response_commit_failure* failure() const&& = delete;

private:
    friend prepared_http1_response_stream_result prepare_http1_response_stream_head(
        http_response, http_response_stream_kind, const http1_response_stream_plan&, http_response_trailer_intent);
    friend prepared_http1_response_stream_result prepare_http1_known_length_response_stream_head(
        http_response, std::uint64_t, http_response_stream_kind, const http1_response_stream_plan&);

    using value_type = std::variant<prepared_http1_response_stream, http1_final_response_commit_failure>;

    explicit prepared_http1_response_stream_result(prepared_http1_response_stream prepared) noexcept
        : value_(std::move(prepared)) {}

    explicit prepared_http1_response_stream_result(http1_final_response_commit_failure failure) noexcept
        : value_(failure) {}

    value_type value_;
};

[[nodiscard]] inline prepared_http1_response_stream_result prepare_http1_response_stream_head(
    http_response response, http_response_stream_kind kind, const http1_response_stream_plan& plan,
    http_response_trailer_intent trailer_intent) {
    auto commit_plan = plan_http_response_stream_commit(
        plan.framing(), plan.request_method(), response.status(), trailer_intent);
    const auto body_plan = commit_plan.body_plan();
    // HTTP/1.0 cannot delimit an open-ended response stream without closing the
    // connection, but a response whose method/status forbids payload is already
    // self-delimited. Make this decision at head commit, when the response status is
    // finally known, instead of pessimistically baking close into the pre-commit plan.
    const auto planned_connection =
        plan.request_connection_plan().disposition() == http1_close_policy::allow_reuse &&
                plan.close_policy() == http1_close_policy::allow_reuse &&
                (plan.framing() != http_response_stream_framing::http1_close_delimited ||
                    body_plan.body_suppressed())
            ? plan.request_connection_plan()
            : plan.request_connection_plan().require_close();
    const auto commit_result = http1_commit_final_response(response, planned_connection);
    if (const auto* failure = commit_result.failure()) {
        return prepared_http1_response_stream_result(*failure);
    }
    const auto connection_plan = *commit_result.committed();
    auto head = prepare_http_response_stream_head(std::move(response), kind, std::move(commit_plan));
    const auto response_head_plan =
        plan.framing() == http_response_stream_framing::http1_chunked
            ? http1_chunked_response_stream_head_plan(head.commit_plan().body_plan(), connection_plan)
            : http1_close_delimited_response_stream_head_plan(
                  head.commit_plan().body_plan(), connection_plan);
    return prepared_http1_response_stream_result(
        prepared_http1_response_stream(std::move(head), response_head_plan, connection_plan));
}

// Commit an incrementally written response whose decoded representation length
// is already known from the upstream protocol. The shared body policy still
// suppresses payload for HEAD/204/304, while the HTTP/1 head owns a canonical
// length and the runtime may keep the connection reusable on either version.
[[nodiscard]] inline prepared_http1_response_stream_result prepare_http1_known_length_response_stream_head(
    http_response response, std::uint64_t content_length, http_response_stream_kind kind,
    const http1_response_stream_plan& plan) {
    auto commit_plan = plan_http_response_stream_commit(http_response_stream_framing::http1_known_length,
        plan.request_method(), response.status(), http_response_trailer_intent::none);
    const auto planned_connection =
        plan.request_connection_plan().disposition() == http1_close_policy::allow_reuse &&
                plan.close_policy() == http1_close_policy::allow_reuse
            ? plan.request_connection_plan()
            : plan.request_connection_plan().require_close();
    const auto commit_result = http1_commit_final_response(response, planned_connection);
    if (const auto* failure = commit_result.failure()) {
        return prepared_http1_response_stream_result(*failure);
    }
    const auto connection_plan = *commit_result.committed();
    auto head = prepare_http_response_stream_head(std::move(response), kind, std::move(commit_plan));
    const auto response_head_plan = http1_known_length_response_stream_head_plan(
        head.commit_plan().body_plan(), connection_plan, content_length);
    return prepared_http1_response_stream_result(
        prepared_http1_response_stream(std::move(head), response_head_plan, connection_plan));
}

}  // namespace ruvia
