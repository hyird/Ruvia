#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include "borrowed_text.h"
#include "http_content_length.h"

namespace ruvia {

// RFC 9110 sender requirements when a request carries content. Explicit
// zero-length framing does not create content; recipients frame it separately.
enum class http_request_content_semantics : std::uint8_t {
    no_additional_requirements,
    forbidden,
    content_type_required,
};

[[nodiscard]] constexpr http_request_content_semantics classify_http_request_content_semantics(std::string_view method) noexcept {
    if (method == "CONNECT" || method == "TRACE") {
        return http_request_content_semantics::forbidden;
    }
    if (method == "OPTIONS") {
        return http_request_content_semantics::content_type_required;
    }
    return http_request_content_semantics::no_additional_requirements;
}

// The one outbound request content contract for HTTP/1, HTTP/2 and HTTP/3:
// none, known length (optionally with the borrowed bytes themselves), or
// streaming. It determines the generated Content-Length (or HTTP/1 chunked
// framing) and whether the head ends the request stream.
// The one empty "no content" alternative shared by every variant in the project.
class http_without_content final {
public:
    constexpr http_without_content() noexcept = default;
};

class http_request_content_plan;

class http_known_length_request_content final {
public:
    [[nodiscard]] constexpr http_content_length length() const noexcept {
        return length_;
    }
    // Set by bytes(); nullopt for known_length(n), whose payload the driver
    // submits after the head.
    [[nodiscard]] constexpr std::optional<std::string_view> bytes() const noexcept {
        return bytes_;
    }

private:
    friend class http_request_content_plan;
    constexpr http_known_length_request_content(http_content_length length, std::optional<std::string_view> bytes_value) noexcept :
        length_(length),
        bytes_(bytes_value) {}
    http_content_length length_;
    std::optional<std::string_view> bytes_;
};

class http_streaming_request_content final {
public:
    [[nodiscard]] constexpr std::optional<http_content_length> expected_length() const noexcept {
        return length_;
    }

private:
    friend class http_request_content_plan;
    explicit constexpr http_streaming_request_content(std::optional<http_content_length> length) noexcept :
        length_(length) {}
    std::optional<http_content_length> length_{};
};

class http_request_content_plan final {
public:
    [[nodiscard]] static constexpr http_request_content_plan none() noexcept {
        return http_request_content_plan(http_without_content());
    }
    // Zero describes no content: it does not require OPTIONS Content-Type and
    // cannot use Expect: 100-continue. TRACE omits the zero Content-Length.
    // The content is submitted after the head, so the head never ends the
    // stream unless the length is zero.
    [[nodiscard]] static constexpr http_request_content_plan known_length(http_content_length length) noexcept {
        return http_request_content_plan(http_known_length_request_content(length, std::nullopt));
    }
    // Borrowed content with its exact length. `bytes("")` differs from `none()`:
    // it asks for an explicit Content-Length: 0. The bytes must remain alive
    // and unchanged until sent; owning temporaries are rejected.
    [[nodiscard]] static constexpr http_request_content_plan bytes(borrowed_text value) noexcept {
        return http_request_content_plan(http_known_length_request_content(http_content_length(value.size()), value.view()));
    }
    [[nodiscard]] static http_request_content_plan bytes(std::span<const std::byte> value) noexcept {
        return bytes(std::string_view(reinterpret_cast<const char*>(value.data()), value.size()));
    }
    template <detail::http_temporary_owning_range<std::byte> range_type>
    static http_request_content_plan bytes(range_type&&) = delete;
    // A known zero keeps the send half open until the request is finished; it
    // cannot send content or wait for 100 Continue.
    [[nodiscard]] static constexpr http_request_content_plan streaming(std::optional<http_content_length> length = {}) noexcept {
        return http_request_content_plan(http_streaming_request_content(length));
    }
    [[nodiscard]] constexpr const http_without_content* without_content() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_without_content>(&value_);
    }
    const http_without_content* without_content() const&& = delete;
    [[nodiscard]] constexpr const http_known_length_request_content* known_length_content() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_known_length_request_content>(&value_);
    }
    const http_known_length_request_content* known_length_content() const&& = delete;
    [[nodiscard]] constexpr const http_streaming_request_content* streaming_content() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_streaming_request_content>(&value_);
    }
    const http_streaming_request_content* streaming_content() const&& = delete;
    // The Content-Length the head declares, if any.
    [[nodiscard]] constexpr std::optional<http_content_length> declared_length() const noexcept {
        if (const auto* known = std::get_if<http_known_length_request_content>(&value_)) {
            return known->length();
        }
        if (const auto* streaming = std::get_if<http_streaming_request_content>(&value_)) {
            return streaming->expected_length();
        }
        return std::nullopt;
    }

private:
    using value_type = std::variant<http_without_content, http_known_length_request_content, http_streaming_request_content>;
    explicit constexpr http_request_content_plan(http_without_content value) noexcept :
        value_(value) {}
    explicit constexpr http_request_content_plan(http_known_length_request_content value) noexcept :
        value_(value) {}
    explicit constexpr http_request_content_plan(http_streaming_request_content value) noexcept :
        value_(value) {}
    value_type value_;
};

}  // namespace ruvia
