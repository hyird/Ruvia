#pragma once

#include <cstdint>
#include <optional>
#include <type_traits>
#include <variant>

#include "ruvia/http/detail/response/http_response_body_access.h"
#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/http_response.h"

#include "coding/http_content_length.h"

// Content-Length planning for protocols whose frames delimit the content
// (HTTP/2 DATA frames, RFC 9113 §8.1.1; HTTP/3 DATA frames, RFC 9114 §4.1.2).
// Framing never depends on the field there, but any emitted value must agree
// with the content actually sent, so the plan names the one normalized value
// the head encodes and the length the content accounting enforces.

namespace ruvia::detail {

enum class http_framed_response_head_plan_error : std::uint8_t {
    invalid_content_length,
    connect_tunnel_required,
    response_status_mismatch,
    response_representation_mismatch
};

class http_framed_response_head_plan final {
public:
    [[nodiscard]] http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    // The normalized Content-Length field to encode, if any.
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept {
        return content_length_mode_ == content_length_mode_type::omit
                   ? std::nullopt
                   : std::optional<std::uint64_t>(content_length_);
    }

    // Only an application-declared streaming length constrains subsequent DATA.
    // Framework-generated buffered lengths are already bound to their response
    // representation and therefore do not create a streaming accounting limit.
    [[nodiscard]] std::optional<std::uint64_t> streaming_content_length() const noexcept {
        return content_length_mode_ == content_length_mode_type::explicit_value
                   ? std::optional<std::uint64_t>(content_length_)
                   : std::nullopt;
    }

    [[nodiscard]] static http_framed_response_head_plan canonical(
        http_response_body_plan body_plan, std::uint64_t value) noexcept {
        return http_framed_response_head_plan(body_plan, content_length_mode_type::canonical, value);
    }

    [[nodiscard]] static std::variant<http_framed_response_head_plan, http_framed_response_head_plan_error>
    preserve_explicit(http_response_body_plan body_plan, const http_response& response) noexcept {
        if (!response_has_known_header(response, response_header_content_length)) {
            return omit(body_plan);
        }

        const auto value = http_response_header_state_access::known_header_value(
            response.headers(), response_header_content_length);
        http_content_length_state state;
        if (state.parse_field(value) != http_content_length_parse_status::ok) {
            return http_framed_response_head_plan_error::invalid_content_length;
        }
        return http_framed_response_head_plan(
            body_plan, content_length_mode_type::explicit_value, *state.value());
    }

    [[nodiscard]] static http_framed_response_head_plan omit(http_response_body_plan body_plan) noexcept {
        return http_framed_response_head_plan(body_plan, content_length_mode_type::omit);
    }

private:
    enum class content_length_mode_type : std::uint8_t {
        omit,
        canonical,
        explicit_value,
    };

    http_framed_response_head_plan(http_response_body_plan body_plan,
        content_length_mode_type content_length_mode, std::uint64_t content_length = 0) noexcept
        : body_plan_(body_plan),
          content_length_mode_(content_length_mode),
          content_length_(content_length) {}

    http_response_body_plan body_plan_;
    content_length_mode_type content_length_mode_;
    std::uint64_t content_length_{0};
};

static_assert(std::is_trivially_copyable_v<http_framed_response_head_plan>);
static_assert(sizeof(http_framed_response_head_plan) <= 24);

using http_framed_response_head_plan_result =
    std::variant<http_framed_response_head_plan, http_framed_response_head_plan_error>;

[[nodiscard]] inline http_framed_response_head_plan_result http_framed_buffered_response_head_plan(
    const http_buffered_response_write_plan& write_plan, const http_response& response) noexcept {
    const auto body_plan = write_plan.body_plan();
    if (body_plan.response_status() != response.status()) {
        return http_framed_response_head_plan_error::response_status_mismatch;
    }
    if (!write_plan.matches_response(response)) {
        return http_framed_response_head_plan_error::response_representation_mismatch;
    }
    // An empty suppressed HEAD body describes the GET representation only
    // through its declared Content-Length, if any (RFC 9110 section 8.6).
    if (body_plan.body_suppressed() && body_plan.status_allows_body() &&
        body_plan.explicit_content_length_allowed() && write_plan.content_length() == 0 &&
        (response.has_head_metadata_body() ||
            response_has_known_header(response, response_header_content_length))) {
        return http_framed_response_head_plan::preserve_explicit(body_plan, response);
    }
    if (body_plan.auto_content_length_allowed()) {
        return http_framed_response_head_plan::canonical(
            body_plan, body_plan.status_allows_body() ? write_plan.content_length() : 0);
    }
    if (body_plan.explicit_content_length_allowed()) {
        return http_framed_response_head_plan::preserve_explicit(body_plan, response);
    }
    return http_framed_response_head_plan::omit(body_plan);
}

[[nodiscard]] inline http_framed_response_head_plan_result http_framed_streaming_response_head_plan(
    const http_response_body_plan& body_plan, const http_response& response) noexcept {
    if (body_plan.response_status() != response.status()) {
        return http_framed_response_head_plan_error::response_status_mismatch;
    }
    if (body_plan.auto_content_length_allowed() && !body_plan.status_allows_body()) {
        return http_framed_response_head_plan::canonical(body_plan, 0);
    }
    if (body_plan.explicit_content_length_allowed()) {
        return http_framed_response_head_plan::preserve_explicit(body_plan, response);
    }
    return http_framed_response_head_plan::omit(body_plan);
}

// A successful CONNECT opens a tunnel and carries no Content-Length (RFC 9110
// §9.3.6); any other outcome must take the generic final-response path.
[[nodiscard]] inline http_framed_response_head_plan_result http_framed_connect_response_head_plan(
    const http_response_body_plan& body_plan) noexcept {
    if (body_plan.content_semantics() == ::ruvia::http_response_content_semantics::connect_tunnel) {
        return http_framed_response_head_plan::omit(body_plan);
    }
    return http_framed_response_head_plan_error::connect_tunnel_required;
}

}  // namespace ruvia::detail
