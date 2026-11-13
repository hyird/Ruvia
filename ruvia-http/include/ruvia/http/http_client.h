#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/ip_address.h"

namespace ruvia::detail {
struct http_client_response_head_access;
}  // namespace ruvia::detail

namespace ruvia {

// A response-side signal for an external request-body writer. The same signal
// is emitted by HTTP/1 response plans and HTTP/2 response events.
enum class http_client_request_content_signal : std::uint8_t {
    continue_value,
    exchange_complete,
};

// Owns response fields.
// Decoding a head without fields does not allocate field storage.
// HEAD response fields remain metadata even for status 205: Content-Length
// does not become the response's received-content length.
class http_client_response_head final {
public:
    http_client_response_head(const http_client_response_head&) = delete;
    http_client_response_head& operator=(const http_client_response_head&) = delete;
    // Moving transfers the owned field storage. The source retains its
    // status and version and exposes empty fields.
    http_client_response_head(http_client_response_head&&) noexcept = default;
    http_client_response_head& operator=(http_client_response_head&&) = delete;

    // The status the response is processed as: a received 600..999 is
    // processed as 500 (RFC 9110 section 15).
    [[nodiscard]] http_status_code status() const noexcept {
        return status_.status();
    }

    // The status code as received, including an invalid 600..999.
    [[nodiscard]] http_received_status received_status() const noexcept {
        return status_;
    }

    [[nodiscard]] http_protocol_version protocol_version() const noexcept {
        return protocol_version_;
    }
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept {
        return content_length_;
    }

    // Parsed field lines retain wire order and repeated metadata fields. In
    // particular, Content-Type values are not parsed as media types, merged,
    // or selected for the application, even when their media-type syntax is invalid.
    // Content-Encoding is also retained without interpreting coding-list syntax;
    // content-decoding APIs validate it independently of message framing.
    [[nodiscard]] std::span<const http_header> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        return headers_;
    }
    [[nodiscard]] std::span<const http_header> headers() const&& = delete;

    // Case-insensitive lookup; the last repeated field wins.
    [[nodiscard]] std::optional<std::string_view> header(
        std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    std::optional<std::string_view> header(std::string_view name) const&& = delete;

    // Transfers the owned parsed fields to a consumer that retains the head's
    // metadata but needs to extend the fields' lifetime independently.
    [[nodiscard]] std::vector<http_header> take_headers() && noexcept {
        return std::move(headers_);
    }

private:
    friend struct detail::http_client_response_head_access;

    http_client_response_head(
        http_received_status status, http_protocol_version protocol_version) noexcept
        : status_(status),
          protocol_version_(protocol_version) {}

    http_received_status status_;
    http_protocol_version protocol_version_;
    std::vector<http_header> headers_;
    std::optional<std::uint64_t> content_length_;
};

}  // namespace ruvia

// Outbound HTTP client protocol models.
//
// OWNERSHIP: these are transport-free HTTP values. http_origin_view and
// http_client_request_view borrow their string storage; http_client_response_head owns
// parsed header storage. Response content remains owned by the
// external sans-I/O driver that follows the framing plan. Socket/TLS
// configuration, pools, redirect limits, and timeouts also belong there.

namespace ruvia {

// Transport-independent sender policy for RFC 9110's only standardized
// request expectation. HTTP/1 and HTTP/2 writers both own generation of the
// Expect field so callers cannot create a wire/state-machine mismatch.
enum class http_client_request_expectation : std::uint8_t {
    none,
    continue_value,
};

// RFC 9110 section 2.2 distinguishes forwarding received fields from generating
// them. Forwarded Content-Type, Origin and CORS request values are preserved;
// Content-Encoding and HTTP/1 Upgrade tolerate legal empty list members.
// This never relaxes field-byte safety, singleton counts or writer ownership.
// The forwarding driver still removes/rebuilds hop-by-hop fields (section 7.6.1).
// OPTIONS Content-Type is required for generated content only (section 9.3.7).
enum class http_request_field_source : std::uint8_t { generated,
    forwarded };

struct http_origin_options final {
    borrowed_text host_{};
    std::optional<port_number> port_{};
};

class http_origin_view final {
public:
    // `host` is a borrowed RFC 3986 uri-host; its storage must remain alive and
    // unchanged whenever this value is consumed. IP literals therefore include
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
// returned string owns its bytes; the origin remains borrowed.
[[nodiscard]] std::string make_http_origin_authority(const http_origin_view& origin);

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
// the external runtime finishes sending them. Owning-string temporaries are
// rejected; literals, string_view values, and owning-string lvalues are accepted.
class http_client_request_content_view final {
public:
    [[nodiscard]] static constexpr http_client_request_content_view none() noexcept {
        return http_client_request_content_view(http_client_request_without_content());
    }

    [[nodiscard]] static constexpr http_client_request_content_view bytes(
        std::string_view value) noexcept {
        return http_client_request_content_view(http_client_request_bytes_view(value));
    }

    [[nodiscard]] static http_client_request_content_view bytes(
        std::span<const std::byte> value) noexcept {
        return bytes(std::string_view(reinterpret_cast<const char*>(value.data()), value.size()));
    }

    template <typename traits_type, typename allocator_type>
    static http_client_request_content_view bytes(
        std::basic_string<char, traits_type, allocator_type>&&) = delete;

    template <typename traits_type, typename allocator_type>
    static http_client_request_content_view bytes(
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
    // unchanged until synchronous preparation/submission consumes them. The
    // HTTP/1 writer permits metadata to overlap its emitted head and owns the
    // small set of facts needed by the later response exchange.
    borrowed_http_headers headers_{};
    http_client_request_content_view content_{http_client_request_content_view::none()};
};

// Borrowed only until open_tunnel() returns its cold operation. Normal CONNECT
// uses authority-form and leaves protocol/target empty. Extended CONNECT uses
// protocol, authority and an origin-form target on HTTP/2.
struct http_client_tunnel_request_view final {
    ::ruvia::borrowed_text authority_{};
    ::ruvia::borrowed_text protocol_{};
    ::ruvia::borrowed_text target_{};
    borrowed_http_headers headers_{};
};

}  // namespace ruvia

// Outbound HTTP redirect protocol helpers.
//
// These values contain no transport policy: redirect limits, cross-origin
// authorization, connection selection, and retries remain owned by the external
// I/O runtime. The helpers only classify HTTP response fields, apply the RFC
// method/content rewrite rules, and resolve one HTTP(S) URI-reference.

namespace ruvia {

enum class http_unique_header_status : std::uint8_t {
    absent,
    found,
    repeated,
};

struct http_client_response_header_lookup final {
    http_unique_header_status status_{};
    std::string_view value_{};
};

[[nodiscard]] http_client_response_header_lookup lookup_unique_http_client_response_header(
    const http_client_response_head& head RUVIA_LIFETIMEBOUND, std::string_view name) noexcept;
[[nodiscard]] http_client_response_header_lookup lookup_unique_http_client_response_header(
    const http_client_response_head&& head, std::string_view name) = delete;

enum class http_client_redirect_content_disposition : std::uint8_t {
    preserve,
    // The I/O owner must omit both the representation and content-specific
    // fields when constructing the redirected request (RFC 9110 Section 15.4).
    drop,
};

struct http_client_redirect_request_plan_options final {
    http_status_code status_;
};

// The method is copied into owned storage so the plan does not
// inherit the request or request-method backing storage lifetime.
class http_client_redirect_request_plan final {
public:
    http_client_redirect_request_plan(const http_client_redirect_request_plan&) = delete;
    http_client_redirect_request_plan& operator=(const http_client_redirect_request_plan&) = delete;
    http_client_redirect_request_plan(http_client_redirect_request_plan&&) noexcept = default;
    http_client_redirect_request_plan& operator=(http_client_redirect_request_plan&&) = delete;

    [[nodiscard]] std::string_view method() const& noexcept RUVIA_LIFETIMEBOUND {
        return method_;
    }
    std::string_view method() const&& = delete;

    [[nodiscard]] constexpr http_client_redirect_content_disposition content_disposition()
        const noexcept {
        return content_disposition_;
    }

private:
    friend http_client_redirect_request_plan plan_http_client_redirect_request(
        const http_client_request_view&, http_client_redirect_request_plan_options);

    http_client_redirect_request_plan(
        std::string_view method, http_client_redirect_content_disposition content_disposition);

    std::string method_;
    http_client_redirect_content_disposition content_disposition_;
};

[[nodiscard]] http_client_redirect_request_plan plan_http_client_redirect_request(
    const http_client_request_view& request, http_client_redirect_request_plan_options options);

// This classification has no alternative-specific payload, so an enum is the
// complete result rather than a status coupled to unrelated fields.
enum class http_client_origin_authority_status : std::uint8_t {
    same_origin,
    different_origin,
    invalid_authority,
};

// Compares effective ports and URI hosts. IPv6 literals compare by address,
// independent of zero compression, leading zeros or an embedded IPv4 tail.
// Other hosts retain URI case/percent-encoding equivalence; no DNS lookup.
[[nodiscard]] http_client_origin_authority_status classify_http_client_origin_authority(
    const http_origin_view& origin, std::string_view authority) noexcept;

enum class http_client_redirect_resolution_error : std::uint8_t {
    invalid_current_target,
    invalid_location,
    // The Location names a scheme this client cannot follow (anything other
    // than http/https). The response itself is well-formed; the I/O owner
    // decides whether to surface the response or fail the exchange.
    unsupported_scheme,
};

struct http_client_redirect_target_options final {
    borrowed_text current_target_{};
    borrowed_text location_{};
};

// A followable redirect destination: the resolved origin plus the origin-form
// target on that origin. `cross_origin()` is true whenever the destination
// scheme/host/port triple differs from the request origin; RFC 9110 Section
// 15.4 and the fetch specification require the I/O owner to drop credentials
// (Authorization, Proxy-Authorization, cookie material not scoped to the new
// origin) before following a cross-origin redirect.
class http_client_resolved_redirect final {
public:
    http_client_resolved_redirect(const http_client_resolved_redirect&) = delete;
    http_client_resolved_redirect& operator=(const http_client_resolved_redirect&) = delete;
    http_client_resolved_redirect(http_client_resolved_redirect&&) noexcept = default;
    http_client_resolved_redirect& operator=(http_client_resolved_redirect&&) = delete;

    [[nodiscard]] std::string_view target() const& noexcept RUVIA_LIFETIMEBOUND {
        return target_;
    }
    std::string_view target() const&& = delete;

    [[nodiscard]] http_scheme scheme() const noexcept {
        return scheme_;
    }

    // RFC 3986 uri-host of the destination; IP literals keep their brackets,
    // matching the http_origin_view factory contract.
    [[nodiscard]] std::string_view host() const& noexcept RUVIA_LIFETIMEBOUND {
        return host_;
    }
    std::string_view host() const&& = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    [[nodiscard]] bool cross_origin() const noexcept {
        return cross_origin_;
    }

    // Borrows host() storage: the returned origin is valid only while this
    // resolved redirect is alive.
    [[nodiscard]] http_origin_view origin() const& RUVIA_LIFETIMEBOUND;
    http_origin_view origin() const&& = delete;

private:
    friend std::variant<http_client_resolved_redirect, http_client_redirect_resolution_error>
    resolve_http_client_redirect_target(const http_origin_view&, http_client_redirect_target_options);

    http_client_resolved_redirect(http_scheme scheme, std::string host, std::uint16_t port,
        std::string target, bool cross_origin) noexcept
        : scheme_(scheme),
          host_(std::move(host)),
          port_(port),
          target_(std::move(target)),
          cross_origin_(cross_origin) {}

    http_scheme scheme_;
    std::string host_;
    std::uint16_t port_;
    std::string target_;
    bool cross_origin_;
};

// Cross-origin-capable resolution: either one owned followable destination or
// one typed failure. A different http/https origin is a success alternative
// classified by cross_origin(); the I/O owner applies its own cross-origin
// policy (credential strip, TLS-downgrade refusal) on top of the classified
// destination.
[[nodiscard]] std::variant<http_client_resolved_redirect, http_client_redirect_resolution_error>
resolve_http_client_redirect_target(
    const http_origin_view& origin, http_client_redirect_target_options options);

}  // namespace ruvia
