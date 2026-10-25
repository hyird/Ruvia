#pragma once

#include <cstdint>
#include <optional>
#include <type_traits>
#include <variant>

#include "ruvia/http/detail/response/http_response_body_access.h"
#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/http_response.h"

#include "coding/http_content_length.h"

namespace ruvia::detail {

enum class http2_response_head_plan_error : std::uint8_t {
    invalid_content_length,
    connect_tunnel_required,
    response_status_mismatch,
    response_representation_mismatch
};

class http2_response_head_plan final {
public:
    [[nodiscard]] http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    // The normalized Content-Length field to encode, if any. HTTP/2 framing
    // never depends on this value, but RFC 9113 requires any emitted value to
    // agree with the DATA content.
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

    [[nodiscard]] static http2_response_head_plan canonical(
        http_response_body_plan body_plan, std::uint64_t value) noexcept {
        return http2_response_head_plan(body_plan, content_length_mode_type::canonical, value);
    }

    [[nodiscard]] static std::variant<http2_response_head_plan, http2_response_head_plan_error>
    preserve_explicit(http_response_body_plan body_plan, const http_response& response) noexcept {
        if (!response_has_known_header(response, response_header_content_length)) {
            return omit(body_plan);
        }

        const auto value = response.headers().header("content-length").value_or("");
        http_content_length_state state;
        if (state.parse_field(value) != http_content_length_parse_status::ok) {
            return http2_response_head_plan_error::invalid_content_length;
        }
        return http2_response_head_plan(
            body_plan, content_length_mode_type::explicit_value, *state.value());
    }

    [[nodiscard]] static http2_response_head_plan omit(http_response_body_plan body_plan) noexcept {
        return http2_response_head_plan(body_plan, content_length_mode_type::omit);
    }

private:
    enum class content_length_mode_type : std::uint8_t {
        omit,
        canonical,
        explicit_value,
    };

    http2_response_head_plan(http_response_body_plan body_plan, content_length_mode_type content_length_mode,
        std::uint64_t content_length = 0) noexcept
        : body_plan_(body_plan),
          content_length_mode_(content_length_mode),
          content_length_(content_length) {}

    http_response_body_plan body_plan_;
    content_length_mode_type content_length_mode_;
    std::uint64_t content_length_{0};
};

static_assert(std::is_trivially_copyable_v<http2_response_head_plan>);
static_assert(sizeof(http2_response_head_plan) <= 24);

using http2_response_head_plan_result_type =
    std::variant<http2_response_head_plan, http2_response_head_plan_error>;

[[nodiscard]] inline http2_response_head_plan_result_type http2_buffered_response_head_plan(
    const http_buffered_response_write_plan& write_plan, const http_response& response) noexcept {
    const auto body_plan = write_plan.body_plan();
    if (write_plan.response_status() != response.status()) {
        return http2_response_head_plan_error::response_status_mismatch;
    }
    if (!write_plan.matches_response(response)) {
        return http2_response_head_plan_error::response_representation_mismatch;
    }
    // An empty suppressed HEAD body describes the GET representation only
    // through its declared Content-Length, if any (RFC 9110 section 8.6).
    if (body_plan.body_suppressed() && body_plan.status_allows_body() &&
        body_plan.explicit_content_length_allowed() && write_plan.content_length() == 0 &&
        (response_body(response).head_metadata_only() ||
            response_has_known_header(response, response_header_content_length))) {
        return http2_response_head_plan::preserve_explicit(body_plan, response);
    }
    if (body_plan.auto_content_length_allowed()) {
        return http2_response_head_plan::canonical(
            body_plan, body_plan.status_allows_body() ? write_plan.content_length() : 0);
    }
    if (body_plan.explicit_content_length_allowed()) {
        return http2_response_head_plan::preserve_explicit(body_plan, response);
    }
    return http2_response_head_plan::omit(body_plan);
}

[[nodiscard]] inline http2_response_head_plan_result_type http2_streaming_response_head_plan(
    const http_response_body_plan& body_plan, const http_response& response) noexcept {
    if (body_plan.response_status() != response.status()) {
        return http2_response_head_plan_error::response_status_mismatch;
    }
    if (body_plan.auto_content_length_allowed() && !body_plan.status_allows_body()) {
        return http2_response_head_plan::canonical(body_plan, 0);
    }
    if (body_plan.explicit_content_length_allowed()) {
        return http2_response_head_plan::preserve_explicit(body_plan, response);
    }
    return http2_response_head_plan::omit(body_plan);
}

[[nodiscard]] inline http2_response_head_plan_result_type http2_connect_response_head_plan(
    const http_response_body_plan& body_plan) noexcept {
    if (body_plan.content_semantics() == ::ruvia::http_response_content_semantics::connect_tunnel) {
        return http2_response_head_plan::omit(body_plan);
    }
    return http2_response_head_plan_error::connect_tunnel_required;
}

}  // namespace ruvia::detail
