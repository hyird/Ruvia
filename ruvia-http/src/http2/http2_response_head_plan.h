#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <system_error>
#include <type_traits>
#include <variant>

#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/detail/server/http_response_write_plan.h"
#include "ruvia/http/http_response.h"

namespace ruvia::detail {

enum class http2_response_head_plan_error : std::uint8_t {
    invalid_content_length,
    connect_tunnel_required,
    response_status_mismatch,
    response_representation_mismatch
};

class http2_response_head_plan_failure final {
public:
    [[nodiscard]] constexpr http2_response_head_plan_error error() const noexcept {
        return error_;
    }

private:
    friend class http2_response_head_plan_result;

    explicit constexpr http2_response_head_plan_failure(http2_response_head_plan_error error) noexcept
        : error_(error) {}

    http2_response_head_plan_error error_;
};

class http2_response_head_plan_result;

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

private:
    friend class http2_response_head_plan_result;

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

class http2_response_head_plan_result final {
public:
    [[nodiscard]] const http2_response_head_plan* plan() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    [[nodiscard]] const http2_response_head_plan* plan() const&& = delete;

    [[nodiscard]] const http2_response_head_plan_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    [[nodiscard]] const http2_response_head_plan_failure* failure() const&& = delete;

private:
    friend http2_response_head_plan_result http2_buffered_response_head_plan(
        const http_buffered_response_write_plan&, const http_response&) noexcept;
    friend http2_response_head_plan_result http2_streaming_response_head_plan(
        const http_response_body_plan&, const http_response&) noexcept;
    friend http2_response_head_plan_result http2_connect_response_head_plan(
        const http_response_body_plan&) noexcept;

    using value_type = std::variant<http2_response_head_plan, http2_response_head_plan_failure>;

    explicit http2_response_head_plan_result(http2_response_head_plan plan) noexcept
        : value_(plan) {}

    explicit http2_response_head_plan_result(http2_response_head_plan_failure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static http2_response_head_plan_result canonical(
        http_response_body_plan body_plan, std::uint64_t value) noexcept {
        return http2_response_head_plan_result(http2_response_head_plan(
            body_plan, http2_response_head_plan::content_length_mode_type::canonical, value));
    }

    [[nodiscard]] static http2_response_head_plan_result preserve_explicit(
        http_response_body_plan body_plan, const http_response& response) noexcept {
        if (!response_has_known_header(response, response_header_content_length)) {
            return omit(body_plan);
        }

        const auto value = response_known_header(response, response_header_content_length);
        std::uint64_t parsed_value = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed_value);
        if (value.empty() || ec != std::errc{} || ptr != value.data() + value.size()) {
            return http2_response_head_plan_result(
                http2_response_head_plan_failure(http2_response_head_plan_error::invalid_content_length));
        }
        return http2_response_head_plan_result(http2_response_head_plan(
            body_plan, http2_response_head_plan::content_length_mode_type::explicit_value, parsed_value));
    }

    [[nodiscard]] static http2_response_head_plan_result omit(http_response_body_plan body_plan) noexcept {
        return http2_response_head_plan_result(
            http2_response_head_plan(body_plan, http2_response_head_plan::content_length_mode_type::omit));
    }

    [[nodiscard]] static http2_response_head_plan_result failure(
        http2_response_head_plan_error error) noexcept {
        return http2_response_head_plan_result(http2_response_head_plan_failure(error));
    }

    value_type value_;
};

[[nodiscard]] inline http2_response_head_plan_result http2_buffered_response_head_plan(
    const http_buffered_response_write_plan& write_plan, const http_response& response) noexcept {
    const auto body_plan = write_plan.body_plan();
    if (write_plan.response_status() != response.status()) {
        return http2_response_head_plan_result::failure(
            http2_response_head_plan_error::response_status_mismatch);
    }
    if (!write_plan.matches_response(response)) {
        return http2_response_head_plan_result::failure(
            http2_response_head_plan_error::response_representation_mismatch);
    }
    if (body_plan.auto_content_length_allowed()) {
        return http2_response_head_plan_result::canonical(
            body_plan, body_plan.status_allows_body() ? write_plan.content_length() : 0);
    }
    return body_plan.explicit_content_length_allowed()
               ? http2_response_head_plan_result::preserve_explicit(body_plan, response)
               : http2_response_head_plan_result::omit(body_plan);
}

[[nodiscard]] inline http2_response_head_plan_result http2_streaming_response_head_plan(
    const http_response_body_plan& body_plan, const http_response& response) noexcept {
    if (body_plan.response_status() != response.status()) {
        return http2_response_head_plan_result::failure(
            http2_response_head_plan_error::response_status_mismatch);
    }
    if (body_plan.auto_content_length_allowed() && !body_plan.status_allows_body()) {
        return http2_response_head_plan_result::canonical(body_plan, 0);
    }
    return body_plan.explicit_content_length_allowed()
               ? http2_response_head_plan_result::preserve_explicit(body_plan, response)
               : http2_response_head_plan_result::omit(body_plan);
}

[[nodiscard]] inline http2_response_head_plan_result http2_connect_response_head_plan(
    const http_response_body_plan& body_plan) noexcept {
    return body_plan.content_semantics() == ::ruvia::http_response_content_semantics::connect_tunnel
               ? http2_response_head_plan_result::omit(body_plan)
               : http2_response_head_plan_result::failure(
                     http2_response_head_plan_error::connect_tunnel_required);
}

}  // namespace ruvia::detail
