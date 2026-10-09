#pragma once

#include "ruvia/http/attributes.h"

// Outbound HTTP client protocol models.
//
// OWNERSHIP: these are transport-free HTTP values. http_origin_view and
// http_client_request_view borrow their string storage; http_client_response_head owns
// parsed header storage through PMR. Response content remains owned by the
// external sans-I/O driver that follows the framing plan. Socket/TLS
// configuration, pools, redirect limits, and timeouts also belong there.

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_client_response_head.h"
#include "ruvia/http/http_header.h"

namespace ruvia {

enum class http_scheme : std::uint8_t {
    http,
    https,
};

// Transport-independent sender policy for RFC 9110's only standardized
// request expectation. HTTP/1 and HTTP/2 writers both own generation of the
// Expect field so callers cannot create a wire/state-machine mismatch.
enum class http_client_request_expectation : std::uint8_t {
    none,
    continue_value,
};

struct http_origin_options final {
    borrowed_text host_{};
    std::optional<std::uint16_t> port_{};
};

class http_origin_view final {
public:
    // `host` is a borrowed RFC 3986 uri-host; its storage must outlive this
    // value and its bytes must remain unchanged. IP literals therefore include
    // brackets (for example, "[::1]"). Factories reject an empty or malformed
    // host before an origin can be observed.
    [[nodiscard]] static http_origin_view http(http_origin_options options);

    [[nodiscard]] static http_origin_view https(http_origin_options options);

    [[nodiscard]] constexpr http_scheme scheme() const noexcept {
        return scheme_;
    }

    // RFC 3986 uri-host only; keep the port in port().
    [[nodiscard]] constexpr std::string_view host() const noexcept {
        return host_;
    }

    [[nodiscard]] constexpr std::uint16_t port() const noexcept {
        return port_;
    }

private:
    constexpr http_origin_view(http_scheme scheme, std::string_view host, std::uint16_t port) noexcept
        : host_(host),
          port_(port),
          scheme_(scheme) {}

    std::string_view host_;
    std::uint16_t port_;
    http_scheme scheme_;
};

// RFC 3986 authority for this origin: preserves IP-literal brackets and
// includes the port only when it differs from the scheme's default. The
// returned string owns its bytes in resource; the origin remains borrowed.
[[nodiscard]] std::pmr::string make_http_origin_authority(const http_origin_view& origin,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

class http_client_request_content_view;

class http_client_request_without_content final {
private:
    friend class http_client_request_content_view;

    constexpr http_client_request_without_content() noexcept = default;
};

class http_client_request_bytes_view final {
public:
    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return value_;
    }

private:
    friend class http_client_request_content_view;

    explicit constexpr http_client_request_bytes_view(std::string_view value) noexcept
        : value_(value) {}

    std::string_view value_;
};

// Borrowed outbound request content. `none()` and `bytes("")` are deliberately
// distinct: the latter asks an HTTP/1 writer to emit Content-Length: 0, while
// the former sends no content framing field. Only the active bytes alternative
// exposes a value. The referenced bytes must remain alive and unchanged until
// the external runtime finishes sending them.
class http_client_request_content_view final {
public:
    [[nodiscard]] static constexpr http_client_request_content_view none() noexcept {
        return http_client_request_content_view(http_client_request_without_content());
    }

    [[nodiscard]] static constexpr http_client_request_content_view bytes(
        std::string_view value) noexcept {
        return http_client_request_content_view(http_client_request_bytes_view(value));
    }

    template <typename traits_type, typename allocator_type>
    static http_client_request_content_view bytes_value(
        std::basic_string<char, traits_type, allocator_type>&&) = delete;

    template <typename traits_type, typename allocator_type>
    static http_client_request_content_view bytes_value(
        const std::basic_string<char, traits_type, allocator_type>&&) = delete;

    [[nodiscard]] constexpr const http_client_request_without_content* without_content()
        const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_client_request_without_content>(&content_);
    }
    const http_client_request_without_content* without_content() const&& = delete;

    [[nodiscard]] constexpr const http_client_request_bytes_view* borrowed_bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_client_request_bytes_view>(&content_);
    }
    const http_client_request_bytes_view* borrowed_bytes() const&& = delete;

private:
    using content_type = std::variant<http_client_request_without_content, http_client_request_bytes_view>;

    explicit constexpr http_client_request_content_view(
        http_client_request_without_content content) noexcept
        : content_(content) {}

    explicit constexpr http_client_request_content_view(http_client_request_bytes_view content) noexcept
        : content_(content) {}

    content_type content_;
};

struct http_client_request_view {
    // Zero-cost field wrapper for request text retained by the sans-I/O
    // request/response transaction. String literals, string_view values, and
    // owning-string lvalues remain valid inputs; owning-string temporaries are
    // rejected before they can leave a dangling view in the request.
    ::ruvia::borrowed_text method_{"GET"};
    ::ruvia::borrowed_text target_{"/"};
    // Borrowed header table; its elements and strings must remain alive and
    // unchanged through the synchronous prepare/submit call. HTTP/1 preparation
    // owns the small set of facts needed by the later response exchange.
    std::span<const http_header_view> headers_{};
    http_client_request_content_view content_{http_client_request_content_view::none()};
    // Explicit caller assertion that the complete server-side processing chain
    // is replay-safe. HTTP/3 0-RTT additionally requires a configured early-data
    // client and a bodyless GET or HEAD request.
    bool replay_safe_{};
};

}  // namespace ruvia
