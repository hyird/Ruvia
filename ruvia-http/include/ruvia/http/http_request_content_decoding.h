#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_body_failure.h"

namespace ruvia {

// Uses the parser's classified header descriptors without rescanning wire names.
// Admission keeps Content-Encoding opaque. Inspect invalid()/unsupported()
// before using codings(); an empty span in either error state is not identity.
[[nodiscard]] http_content_coding_field_result request_content_coding(
    const http_request& request, std::pmr::memory_resource* resource = nullptr) noexcept;

class http_request_content_decode_protocol_failure final {
public:
    [[nodiscard]] http_protocol_error protocol_error() const noexcept {
        switch (error_) {
            case http_content_decode_error::unsupported_coding:
                return http_protocol_error(http_status::unsupported_media_type,
                    "request Content-Encoding is not supported");
            case http_content_decode_error::invalid_content:
                return http_protocol_error(http_status::bad_request, "failed to decode request body");
            case http_content_decode_error::decoded_size_exceeded:
                return http_request_body_failure::too_large().protocol_error();
            case http_content_decode_error::decoder_failure:
                std::terminate();
        }
        std::terminate();
    }

private:
    friend class http_request_content_decode_result;

    explicit constexpr http_request_content_decode_protocol_failure(
        http_content_decode_error error) noexcept
        : error_(error) {}

    http_content_decode_error error_;
};

class http_request_content_decoder_failure final {
private:
    friend class http_request_content_decode_result;

    constexpr http_request_content_decoder_failure() noexcept = default;
};

// Request decoding is role-specific: success, an HTTP protocol failure, and an
// internal decoder failure are mutually exclusive alternatives. Client
// response decoding keeps using the role-neutral decode_http_content().
class http_request_content_decode_result final {
public:
    http_request_content_decode_result(const http_request_content_decode_result&) = delete;
    http_request_content_decode_result& operator=(const http_request_content_decode_result&) = delete;
    http_request_content_decode_result(http_request_content_decode_result&&) noexcept = default;
    http_request_content_decode_result& operator=(http_request_content_decode_result&&) = delete;

    [[nodiscard]] std::pmr::string* decoded() & noexcept {
        return std::get_if<std::pmr::string>(&value_);
    }
    [[nodiscard]] const std::pmr::string* decoded() const& noexcept {
        return std::get_if<std::pmr::string>(&value_);
    }
    std::pmr::string* decoded() && = delete;
    const std::pmr::string* decoded() const&& = delete;

    [[nodiscard]] const http_request_content_decode_protocol_failure* protocol_failure() const& noexcept {
        return std::get_if<http_request_content_decode_protocol_failure>(&value_);
    }
    const http_request_content_decode_protocol_failure* protocol_failure() const&& = delete;

    [[nodiscard]] const http_request_content_decoder_failure* decoder_failure() const& noexcept {
        return std::get_if<http_request_content_decoder_failure>(&value_);
    }
    const http_request_content_decoder_failure* decoder_failure() const&& = delete;

private:
    friend http_request_content_decode_result decode_http_request_content(
        std::span<const http_content_coding>, std::string_view, http_content_decode_options);

    using value_type = std::variant<std::pmr::string, http_request_content_decode_protocol_failure,
        http_request_content_decoder_failure>;

    explicit http_request_content_decode_result(std::pmr::string decoded) noexcept
        : value_(std::move(decoded)) {}

    explicit http_request_content_decode_result(http_content_decode_error error) noexcept
        : value_(http_request_content_decode_protocol_failure(error)) {}

    explicit constexpr http_request_content_decode_result(
        http_request_content_decoder_failure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static constexpr http_request_content_decode_result make_decoder_failure() noexcept {
        return http_request_content_decode_result(http_request_content_decoder_failure());
    }

    value_type value_;
};

[[nodiscard]] inline http_request_content_decode_result decode_http_request_content(
    std::span<const http_content_coding> codings, std::string_view input,
    http_content_decode_options options) {
    auto result_value = decode_http_content(codings, input, options);
    if (auto* decoded = std::get_if<std::pmr::string>(&result_value)) {
        return http_request_content_decode_result(std::move(*decoded));
    }
    const auto error = std::get<http_content_decode_error>(result_value);
    if (error == http_content_decode_error::decoder_failure) {
        return http_request_content_decode_result::make_decoder_failure();
    }
    return http_request_content_decode_result(error);
}
}  // namespace ruvia
