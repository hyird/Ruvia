#pragma once

#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http_response.h"

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

// Outbound HTTP client protocol models.
//
// OWNERSHIP: these are transport-free HTTP values. http_origin_view and
// http_client_request_view borrow their string storage; http_client_response_head owns
// parsed header storage. Response content remains owned by the
// external sans-I/O driver that follows the framing plan. Socket/TLS
// configuration, pools, redirect limits, and timeouts also belong there.

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

// Outbound HTTP redirect protocol helpers.
//
// These values contain no transport policy: redirect limits, cross-origin
// authorization, connection selection, and retries remain owned by the external
// I/O runtime. The helpers only classify HTTP response fields, apply the RFC
// method/content rewrite rules, and resolve one HTTP(S) URI-reference.

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

namespace detail {

enum class http1_client_initial_content_state : std::uint8_t {
    complete,
    pending,
    awaiting_continue,
};

struct http1_client_request_prepare_result_access;

struct http1_client_exchange_state_access;

}  // namespace detail

// Owning protocol facts for exactly one HTTP/1 response exchange. Preparing a
// request creates this state from the facts that actually entered the wire
// plan; transferring it to the response parser removes every dependency on the
// caller's method and header storage. Only an offered Upgrade value needs owned
// dynamic storage, so ordinary requests remain allocation-free.
class http1_client_exchange_state final {
public:
    http1_client_exchange_state(const http1_client_exchange_state&) = delete;
    http1_client_exchange_state& operator=(const http1_client_exchange_state&) = delete;
    http1_client_exchange_state(http1_client_exchange_state&&) noexcept = default;
    http1_client_exchange_state& operator=(http1_client_exchange_state&&) = delete;

private:
    friend struct detail::http1_client_request_prepare_result_access;
    friend struct detail::http1_client_exchange_state_access;

    http1_client_exchange_state(http_known_method method, http1_close_policy close_policy,
        detail::http1_client_initial_content_state content_state,
        std::string offered_upgrade_protocols) noexcept
        : offered_upgrade_protocols_(std::move(offered_upgrade_protocols)),
          method_(method),
          close_policy_(close_policy),
          content_state_(content_state) {}

    std::string offered_upgrade_protocols_;
    http_known_method method_{http_known_method::unknown};
    http1_close_policy close_policy_{http1_close_policy::allow_reuse};
    detail::http1_client_initial_content_state content_state_{
        detail::http1_client_initial_content_state::complete};
};

// Request-target form of a non-CONNECT request line (RFC 9112 section 3.2).
// CONNECT always uses authority-form through prepare_connect().
enum class http1_request_target_form : std::uint8_t {
    // origin-form, or asterisk-form for OPTIONS *: the request goes to the
    // origin server itself (section 3.2.1).
    origin,
    // absolute-form, for a request through a forward proxy (section 3.2.2):
    // the origin's scheme and authority precede the origin-form target, and
    // the generated Host carries the same authority (section 3.2). OPTIONS *
    // becomes the authority with an empty path (section 3.2.4).
    absolute,
};

// One immutable wire policy for request preparation. Expect is writer-owned so
// the emitted field and the resulting content gate cannot disagree. How long an
// I/O runtime waits before releasing a continue-gated body remains runtime
// policy, not an HTTP message-model setting.
struct http1_client_request_wire_policy final {
    http1_close_policy close_policy_{http1_close_policy::allow_reuse};
    http_client_request_expectation expectation_{http_client_request_expectation::none};
    http_request_field_source field_source_{http_request_field_source::generated};
    http1_request_target_form target_form_{http1_request_target_form::origin};
};

namespace detail {

}  // namespace detail

struct http1_client_request_head_view final {
    borrowed_text method_{"GET"};
    borrowed_text target_{"/"};
    borrowed_http_headers headers_{};
    // A value sends Content-Length; absent sends Transfer-Encoding: chunked.
    std::optional<http_content_length> content_length_{};
};

class http1_client_streaming_request_content final {
public:
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept {
        return length_;
    }
    [[nodiscard]] bool continue_gated() const noexcept {
        return gated_;
    }

private:
    friend struct detail::http1_client_request_prepare_result_access;
    http1_client_streaming_request_content(std::optional<std::uint64_t> length, bool gated) noexcept
        : length_(length),
          gated_(gated) {}
    std::optional<std::uint64_t> length_;
    bool gated_;
};

class http1_client_request_without_content final {
private:
    friend struct detail::http1_client_request_prepare_result_access;

    constexpr http1_client_request_without_content() noexcept = default;
};

class http1_client_immediate_request_content final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend struct detail::http1_client_request_prepare_result_access;

    explicit constexpr http1_client_immediate_request_content(std::string_view bytes_value) noexcept
        : bytes_(bytes_value) {}

    std::string_view bytes_;
};

class http1_client_continue_gated_request_content final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend struct detail::http1_client_request_prepare_result_access;

    explicit constexpr http1_client_continue_gated_request_content(std::string_view bytes_value) noexcept
        : bytes_(bytes_value) {}

    std::string_view bytes_;
};

// Immutable outbound content contract. Continue-gated content means the writer
// generated Expect: 100-continue and the I/O owner must send the head separately;
// it may release those bytes after 100 Continue or according to its finite wait
// policy. Immediate content includes explicit empty content (Content-Length: 0).
// Payload exists only on the two alternatives that actually send content.
class http1_client_request_content_plan final {
public:
    [[nodiscard]] constexpr const http1_client_request_without_content* without_content()
        const& noexcept {
        return std::get_if<http1_client_request_without_content>(&content_);
    }
    const http1_client_request_without_content* without_content() const&& = delete;

    [[nodiscard]] constexpr const http1_client_immediate_request_content* immediate() const& noexcept {
        return std::get_if<http1_client_immediate_request_content>(&content_);
    }
    const http1_client_immediate_request_content* immediate() const&& = delete;

    [[nodiscard]] constexpr const http1_client_continue_gated_request_content* continue_gated()
        const& noexcept {
        return std::get_if<http1_client_continue_gated_request_content>(&content_);
    }
    const http1_client_continue_gated_request_content* continue_gated() const&& = delete;

    [[nodiscard]] constexpr const http1_client_streaming_request_content* streaming() const& noexcept {
        return std::get_if<http1_client_streaming_request_content>(&content_);
    }
    const http1_client_streaming_request_content* streaming() const&& = delete;

private:
    friend struct detail::http1_client_request_prepare_result_access;

    explicit http1_client_request_content_plan(http1_client_streaming_request_content content) noexcept
        : content_(content) {}
    using content_type = std::variant<http1_client_request_without_content,
        http1_client_immediate_request_content, http1_client_continue_gated_request_content, http1_client_streaming_request_content>;

    explicit constexpr http1_client_request_content_plan(
        http1_client_request_without_content content) noexcept
        : content_(content) {}

    explicit constexpr http1_client_request_content_plan(
        http1_client_immediate_request_content content) noexcept
        : content_(content) {}

    explicit constexpr http1_client_request_content_plan(
        http1_client_continue_gated_request_content content) noexcept
        : content_(content) {}

    content_type content_;
};

enum class http1_client_request_prepare_error : std::uint8_t {
    invalid_method,
    invalid_target,
    connect_requires_dedicated_entry,
    invalid_connect_origin,
    invalid_header,
    host_header_managed_by_writer,
    content_length_managed_by_writer,
    transfer_encoding_unsupported,
    // A Trailer field declares trailer fields (RFC 9110 section 6.6.2), which
    // only chunked request content can carry (RFC 9112 section 7.1.2).
    trailer_requires_chunked_content,
    expect_header_managed_by_writer,
    // A malformed Connection list, or an option naming a field the next hop
    // must still see: framing fields, Host, request-origin metadata
    // (Forwarded, X-Forwarded-For, X-Forwarded-Proto, X-Forwarded-Host), or a
    // field intended for all recipients (RFC 9110 section 7.6.1).
    invalid_connection,
    invalid_upgrade,
    upgrade_connection_option_required,
    te_connection_option_required,
    expectation_without_content,
    content_forbidden_for_method,
    options_content_type_required,
    header_too_large,
    invalid_close_policy,
    invalid_expectation,
    // An unknown target form, or absolute-form on the CONNECT entries.
    invalid_target_form,
};

[[nodiscard]] std::string_view http1_client_request_prepare_error_message(
    http1_client_request_prepare_error error) noexcept;

class http1_client_request_buffer_too_small final {
public:
    [[nodiscard]] constexpr std::size_t required_head_bytes() const noexcept {
        return required_head_bytes_;
    }

private:
    friend struct detail::http1_client_request_prepare_result_access;

    explicit constexpr http1_client_request_buffer_too_small(std::size_t required_head_bytes) noexcept
        : required_head_bytes_(required_head_bytes) {}

    std::size_t required_head_bytes_;
};

// Transactionally prepared scatter-gather request. `head()` points into the
// caller-provided output buffer and the active immediate/continue-gated
// alternative points into the request's borrowed content; those sources must
// remain alive and unchanged until sent.
// take_exchange_state() moves out the independent response-side protocol facts;
// head() and content_plan() remain readable for the request write or an explicit retry.
class prepared_http1_client_request final {
public:
    [[nodiscard]] constexpr std::string_view head() const& noexcept {
        return head_;
    }
    [[nodiscard]] constexpr std::string_view head() const&& = delete;

    [[nodiscard]] constexpr const http1_client_request_content_plan& content_plan() const& noexcept {
        return content_plan_;
    }
    [[nodiscard]] constexpr const http1_client_request_content_plan& content_plan() const&& = delete;

    [[nodiscard]] http1_client_exchange_state take_exchange_state() && noexcept {
        return std::move(exchange_state_);
    }

private:
    friend struct detail::http1_client_request_prepare_result_access;

    prepared_http1_client_request(std::string_view head, http1_client_request_content_plan content_plan,
        http1_client_exchange_state exchange_state) noexcept
        : head_(head),
          content_plan_(content_plan),
          exchange_state_(std::move(exchange_state)) {}

    std::string_view head_;
    http1_client_request_content_plan content_plan_;
    http1_client_exchange_state exchange_state_;
};

// HTTP/1.1 request writer for a direct origin or, in absolute-form, a forward proxy. Ordinary requests are allocation-free;
// an Upgrade request owns only its offered protocol value for later 101
// validation. It validates the complete request before touching the caller's
// buffer, generates Host and exact Content-Length, and returns separate
// head/content views for writev-style I/O or Expect: 100-continue gating.
// Generated port and Content-Length tokens are shared by measurement and writing.
// Metadata text and the header descriptor array may overlap head_buffer:
// successful preparation consumes them before overwriting the emitted head.
// Borrowed content must not overlap those written bytes; it may use the unused
// tail of head_buffer and remains unchanged until the runtime sends it.
// CONNECT uses its dedicated entry so authority form cannot be confused with an
// origin-form target.
// Explicit empty content emits Content-Length: 0 without triggering TRACE's
// content prohibition or generated OPTIONS' Content-Type requirement.
class http1_client_request_writer final {
public:
    http1_client_request_writer() noexcept = default;

    [[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
        http1_client_request_prepare_error>
    prepare(const http_origin_view& origin,
        const http_client_request_view& request, std::span<char> head_buffer,
        http1_client_request_wire_policy policy = {}) const;

    // RFC 9298 HTTP/1.1 GET Upgrade. Capsule-Protocol is validated; Upgrade and
    // Connection are driver-owned, and Host remains owned by the writer.
    // RFC 9931 section 6.3 forbids sending UDP payload before the server's
    // successful protocol-upgrade response in HTTP/1.x.
    [[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
        http1_client_request_prepare_error>
    prepare_connect_udp(const http_origin_view& origin,
        borrowed_text target, std::span<const http_header_view> headers, std::span<char> head_buffer,
        http1_client_request_wire_policy policy = {}) const;

    [[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
        http1_client_request_prepare_error>
    prepare_streaming(const http_origin_view& origin,
        const http1_client_request_head_view& request, std::span<char> head_buffer,
        http1_client_request_wire_policy policy = {}) const;

    // When forwarding untrusted TCP payload, RFC 9931 section 8 requires the
    // driver to wait for the successful tunnel plan before sending that payload,
    // unless close_after_response makes the request emit Connection: close.
    [[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
        http1_client_request_prepare_error>
    prepare_connect(const http_origin_view& tunnel_origin,
        std::span<const http_header_view> headers, std::span<char> head_buffer,
        http1_client_request_wire_policy policy = {}) const;

    [[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
        http1_client_request_prepare_error>
    prepare_connect(borrowed_text authority,
        std::span<const http_header_view> headers, std::span<char> head_buffer,
        http1_client_request_wire_policy policy = {}) const;
};

enum class http1_request_content_write_error : std::uint8_t {
    awaiting_continue,
    stopped,
    write_pending,
    length_overflow,
    length_mismatch,
    invalid_trailer,
    trailers_require_chunked,
    output_too_small,
    header_too_large,
};

// Bound to a successfully prepared streaming request's immutable framing facts.
// Payload bytes remain borrowed until commit_chunk(); prefixes/suffixes are
// independent scatter-gather segments. A transport failure must abort the writer.
// release_content() is called on 100 Continue or on the driver's finite timeout.
// A final response before upload completion requires abort(), not an implicit FIN.
class http1_request_content_writer final {
public:
    class chunk final {
    public:
        // Only the prefix borrows this snapshot; retain the chunk until the
        // scatter-gather write has consumed it. The payload still borrows input.
        [[nodiscard]] constexpr std::string_view prefix() const& noexcept RUVIA_LIFETIMEBOUND {
            return std::string_view(prefix_.data(), prefix_size_);
        }
        std::string_view prefix() const&& = delete;

        [[nodiscard]] constexpr std::span<const char> payload() const noexcept {
            return payload_;
        }
        [[nodiscard]] constexpr std::string_view suffix() const noexcept {
            return suffix_;
        }

    private:
        friend class http1_request_content_writer;
        chunk() noexcept = default;

        std::array<char, 2 * sizeof(std::size_t) + 2> prefix_{};
        std::size_t prefix_size_{0};
        std::span<const char> payload_{};
        std::string_view suffix_{};
    };
    explicit http1_request_content_writer(const http1_client_streaming_request_content& plan) noexcept;
    void release_content() noexcept;
    void abort() noexcept;
    [[nodiscard]] std::variant<chunk, http1_request_content_write_error> plan_chunk(std::span<const char> payload) noexcept;
    template <detail::http_temporary_owning_range<char> payload_type>
    std::variant<chunk, http1_request_content_write_error> plan_chunk(payload_type&&) noexcept = delete;
    std::variant<chunk, http1_request_content_write_error> plan_chunk(std::initializer_list<char>) noexcept = delete;
    void commit_chunk();
    // Returns a view into buffer. Transmit it, then commit_finish(). Known-length
    // bodies return an empty view and still require a successful commit_finish().
    // Trailer names and values may borrow buffer. Failure leaves buffer unchanged.
    [[nodiscard]] std::variant<std::string_view, http1_request_content_write_error> plan_finish(
        std::span<char> buffer, std::span<const http_header_view> trailers = {}) noexcept;
    void commit_finish();
    [[nodiscard]] bool finished() const noexcept {
        return finished_;
    }
    [[nodiscard]] std::uint64_t committed_payload_bytes() const noexcept {
        return committed_;
    }

private:
    std::optional<std::uint64_t> length_;
    std::uint64_t committed_{0};
    std::size_t pending_{0};
    bool gated_{false}, stopped_{false}, writing_{false}, finishing_{false}, finished_{false};
};

namespace detail {

struct http1_client_response_parse_result_access;
struct http1_client_response_plan_access;

// A validated HTTP/1 status line, kept by http1_client_response_parser from
// the arrival of its line feed until the head it starts completes.
struct http1_client_parsed_status_line final {
    http_received_status received_status_;
    http_protocol_version protocol_version_;
};

// One request-content lifecycle for the response side of an HTTP/1 exchange.
// Expect and completion are not independent booleans: releasing the gate,
// receiving Continue, and completing content are ordered events. Their phase
// determines whether a later 100 is actionable and whether 101 is legal.
enum class http1_client_request_content_phase : std::uint8_t {
    content_complete,
    content_pending,
    awaiting_continue,
    content_released_awaiting_continue,
    continue_received,
    content_complete_awaiting_continue,
    continue_received_content_complete,
};

}  // namespace detail

// Connection lifecycle after a self-delimited response has been consumed.
// For an informational response, reuse means the same exchange can await its
// final response; it does not make the connection poolable before that final.
// Close-delimited responses, tunnels, and upgrades are separate alternatives.

// Result of notifying the exchange that the external runtime finished writing
// every byte in the prepared request content plan. This event is required before
// a content-bearing request can accept 101 Switching Protocols.
enum class http1_client_request_content_completion_status : std::uint8_t {
    completed,
    already_complete,
    exchange_terminal,
};

class http1_client_informational_response final {
public:
    [[nodiscard]] constexpr http1_close_policy persistence() const noexcept {
        return persistence_;
    }

private:
    friend struct detail::http1_client_response_plan_access;

    explicit constexpr http1_client_informational_response(http1_close_policy persistence) noexcept
        : persistence_(persistence) {}

    http1_close_policy persistence_;
};

class http1_client_response_without_content final {
public:
    [[nodiscard]] constexpr http1_close_policy persistence() const noexcept {
        return persistence_;
    }

private:
    friend struct detail::http1_client_response_plan_access;

    explicit constexpr http1_client_response_without_content(http1_close_policy persistence) noexcept
        : persistence_(persistence) {}

    http1_close_policy persistence_;
};

class http1_client_known_length_response final {
public:
    [[nodiscard]] constexpr std::size_t content_length() const noexcept {
        return content_length_;
    }

    [[nodiscard]] constexpr bool requires_body_consumption() const noexcept {
        return content_length_ != 0;
    }

    [[nodiscard]] constexpr http1_close_policy persistence() const noexcept {
        return persistence_;
    }

private:
    friend struct detail::http1_client_response_plan_access;

    constexpr http1_client_known_length_response(
        std::size_t content_length, http1_close_policy persistence) noexcept
        : content_length_(content_length),
          persistence_(persistence) {}

    std::size_t content_length_;
    http1_close_policy persistence_;
};

class http1_client_chunked_response final {
public:
    // Transfer codings preceding the terminal chunked framing. The runtime
    // removes chunk framing first and then drives this decoder list.
    [[nodiscard]] const http_transfer_codings& transfer_codings() const& noexcept RUVIA_LIFETIMEBOUND {
        return transfer_codings_;
    }
    const http_transfer_codings& transfer_codings() const&& = delete;

    [[nodiscard]] constexpr http1_close_policy persistence() const noexcept {
        return persistence_;
    }

private:
    friend struct detail::http1_client_response_plan_access;

    http1_client_chunked_response(
        http_transfer_codings transfer_codings, http1_close_policy persistence)
        : transfer_codings_(std::move(transfer_codings)),
          persistence_(persistence) {}

    http_transfer_codings transfer_codings_;
    http1_close_policy persistence_;
};

class http1_client_close_delimited_response final {
public:
    // Any non-chunked transfer coding is decoded after EOF delimits the message.
    // This alternative always consumes through EOF and always closes; it exposes
    // no independent persistence field that could contradict those facts.
    [[nodiscard]] const http_transfer_codings& transfer_codings() const& noexcept RUVIA_LIFETIMEBOUND {
        return transfer_codings_;
    }
    const http_transfer_codings& transfer_codings() const&& = delete;

private:
    friend struct detail::http1_client_response_plan_access;

    explicit http1_client_close_delimited_response(http_transfer_codings transfer_codings)
        : transfer_codings_(std::move(transfer_codings)) {}

    http_transfer_codings transfer_codings_;
};

class http1_client_connect_tunnel final {
private:
    friend struct detail::http1_client_response_plan_access;
    constexpr http1_client_connect_tunnel() noexcept = default;
};

class http1_client_protocol_upgrade final {
private:
    friend struct detail::http1_client_response_plan_access;
    constexpr http1_client_protocol_upgrade() noexcept = default;
};

// Immutable RFC 9110/9112 response framing and lifecycle contract. The eight
// alternatives mirror message-length precedence and content semantics directly:
// informational, no-content final, zero-content-with-framing, exact-length,
// final-chunked, close-delimited, CONNECT tunnel, or protocol upgrade.
// Alternative-specific payload is only reachable from the alternative that owns
// it.
class http1_client_response_plan final {
public:
    [[nodiscard]] constexpr const http1_client_informational_response* informational()
        const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_client_informational_response>(&state_);
    }
    const http1_client_informational_response* informational() const&& = delete;

    [[nodiscard]] constexpr const http1_client_response_without_content* without_content()
        const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_client_response_without_content>(&state_);
    }
    const http1_client_response_without_content* without_content() const&& = delete;

    // A 205 response with an ordinary framing. RFC 9110 section 15.3.6 forbids
    // only generating its content, so content the framing delimits is consumed
    // and discarded: http1_client_response_body_decoder never emits it, and the
    // connection stays reusable.
    [[nodiscard]] constexpr bool discards_content() const noexcept {
        return discards_content_;
    }
    [[nodiscard]] constexpr const http1_client_known_length_response* known_length() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_client_known_length_response>(&state_);
    }
    const http1_client_known_length_response* known_length() const&& = delete;

    [[nodiscard]] constexpr const http1_client_chunked_response* chunked() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_client_chunked_response>(&state_);
    }
    const http1_client_chunked_response* chunked() const&& = delete;

    [[nodiscard]] constexpr const http1_client_close_delimited_response* close_delimited()
        const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_client_close_delimited_response>(&state_);
    }
    const http1_client_close_delimited_response* close_delimited() const&& = delete;

    [[nodiscard]] constexpr const http1_client_connect_tunnel* connect_tunnel() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_client_connect_tunnel>(&state_);
    }
    const http1_client_connect_tunnel* connect_tunnel() const&& = delete;

    [[nodiscard]] constexpr const http1_client_protocol_upgrade* protocol_upgrade() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_client_protocol_upgrade>(&state_);
    }
    const http1_client_protocol_upgrade* protocol_upgrade() const&& = delete;

    // Only exchange_complete cancels pending request content. Receiving an early
    // final response without that signal leaves the external writer responsible
    // for finishing the request before the connection can be reused (RFC 9110
    // section 7.5), even though this response-head parser is already terminal.
    [[nodiscard]] constexpr std::optional<http_client_request_content_signal> request_content_signal()
        const noexcept {
        return request_content_signal_;
    }

private:
    friend struct detail::http1_client_response_plan_access;

    using state_type = std::variant<http1_client_informational_response, http1_client_response_without_content,
        http1_client_known_length_response, http1_client_chunked_response,
        http1_client_close_delimited_response, http1_client_connect_tunnel,
        http1_client_protocol_upgrade>;

    http1_client_response_plan(state_type state_value,
        std::optional<http_client_request_content_signal> request_content_signal,
        bool discards_content = false)
        : state_(std::move(state_value)),
          request_content_signal_(request_content_signal),
          discards_content_(discards_content) {}

    state_type state_;
    std::optional<http_client_request_content_signal> request_content_signal_;
    bool discards_content_{false};
};

static_assert(std::is_nothrow_move_constructible_v<http1_client_response_plan>);

// Protocol failures use typed values whose representation does not allocate.
// Resource exhaustion can throw while unfolding fields, storing temporary
// descriptors, or materializing an owning response. Exchange termination is
// NOT a parse error: parsing after the exchange completed or failed throws
// std::logic_error.
enum class http1_client_response_parse_error : std::uint8_t {
    header_too_large,
    invalid_status_line,
    unsupported_http_version,
    invalid_status_code,
    invalid_reason_phrase,
    invalid_header,
    invalid_connection,
    invalid_upgrade,
    invalid_content_length,
    conflicting_content_length,
    invalid_transfer_encoding,
    unsupported_transfer_encoding,
    content_length_and_transfer_encoding,
    invalid_protocol_switch,
};

[[nodiscard]] std::string_view http1_client_response_parse_error_message(
    http1_client_response_parse_error error) noexcept;

class http1_client_response_need_more final {
private:
    friend struct detail::http1_client_response_parse_result_access;

    explicit constexpr http1_client_response_need_more() noexcept = default;
};

// One complete response head. The response owns status/header storage;
// consumed_bytes() is the exact boundary after CRLF CRLF, so a sans-I/O
// driver can feed the remaining bytes to the body/tunnel/upgrade path directly.
class http1_parsed_client_response_head final {
public:
    http1_parsed_client_response_head(const http1_parsed_client_response_head&) = delete;
    http1_parsed_client_response_head& operator=(const http1_parsed_client_response_head&) = delete;
    http1_parsed_client_response_head(http1_parsed_client_response_head&&) noexcept = default;
    http1_parsed_client_response_head& operator=(http1_parsed_client_response_head&&) = delete;

    [[nodiscard]] const http_client_response_head& head() const& noexcept RUVIA_LIFETIMEBOUND {
        return head_;
    }
    [[nodiscard]] const http_client_response_head& head() const&& = delete;

    [[nodiscard]] http_client_response_head take_head() && noexcept {
        return std::move(head_);
    }

    [[nodiscard]] const http1_client_response_plan& plan() const& noexcept RUVIA_LIFETIMEBOUND {
        return plan_;
    }
    [[nodiscard]] const http1_client_response_plan& plan() const&& = delete;

    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend struct detail::http1_client_response_parse_result_access;

    http1_parsed_client_response_head(http_client_response_head head, http1_client_response_plan plan,
        std::size_t consumed_bytes) noexcept
        : head_(std::move(head)),
          plan_(std::move(plan)),
          consumed_bytes_(consumed_bytes) {}

    http_client_response_head head_;
    http1_client_response_plan plan_;
    std::size_t consumed_bytes_{0};
};

static_assert(std::is_nothrow_move_constructible_v<http1_parsed_client_response_head>);

class http1_client_response_parse_result final {
public:
    http1_client_response_parse_result(const http1_client_response_parse_result&) = delete;
    http1_client_response_parse_result& operator=(const http1_client_response_parse_result&) = delete;
    http1_client_response_parse_result(http1_client_response_parse_result&&) noexcept = default;
    http1_client_response_parse_result& operator=(http1_client_response_parse_result&&) = delete;

    [[nodiscard]] const http1_client_response_need_more* need_more() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_client_response_need_more>(&state_);
    }
    const http1_client_response_need_more* need_more() const&& = delete;

    [[nodiscard]] http1_parsed_client_response_head* parsed() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_parsed_client_response_head>(&state_);
    }

    [[nodiscard]] const http1_parsed_client_response_head* parsed() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_parsed_client_response_head>(&state_);
    }
    http1_parsed_client_response_head* parsed() && = delete;
    const http1_parsed_client_response_head* parsed() const&& = delete;

    [[nodiscard]] const http1_client_response_parse_error* failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_client_response_parse_error>(&state_);
    }
    const http1_client_response_parse_error* failure() const&& = delete;

private:
    friend struct detail::http1_client_response_parse_result_access;

    explicit http1_client_response_parse_result(http1_client_response_need_more state_value) noexcept
        : state_(state_value) {}

    explicit http1_client_response_parse_result(http1_parsed_client_response_head state_value) noexcept
        : state_(std::move(state_value)) {}

    explicit http1_client_response_parse_result(http1_client_response_parse_error state_value) noexcept
        : state_(state_value) {}

    std::variant<http1_client_response_need_more, http1_parsed_client_response_head,
        http1_client_response_parse_error>
        state_;
};

static_assert(std::is_nothrow_move_constructible_v<http1_client_response_parse_result>);

// Per-request HTTP/1 response-head state machine. Construction consumes the
// owning exchange state from one successfully prepared request; informational
// responses advance the same
// exchange until a final response, CONNECT tunnel, or protocol switch completes
// it. Header validation is transactional and owning response allocation occurs
// only after protocol validation succeeds. Temporary field descriptors use
// bounded inline storage and spill to heap storage as needed. Response
// obs-fold is replaced with spaces before field validation.
// Auxiliary fields do not select framing: initial Trailer values are opaque
// declaration metadata subject to ordinary field-byte and size limits, not
// trailer-name/list validation, and are accepted without chunked framing.
// A declaration neither promises nor authorizes actual trailers; actual trailer
// fields retain their independent restrictions. TE has no response semantics,
// and advertised Upgrade does not require Connection: Upgrade unless this is a
// negotiated 101 switch. These recipient rules do not relax HTTP/1 sender
// requirements. A 1xx, HEAD,
// 204 or 304 response ends at the first empty line regardless of framing fields
// (RFC 9112 section 6.3); a successful CONNECT begins its tunnel there.
// A rejected CONNECT retains ordinary response framing but always closes after
// that response, in line with RFC 9931 section 8; it never returns to a pool.
// Ignored 1xx/204/304/HEAD/CONNECT framing values are not interpreted.
// Any number of informational heads may precede the final response; each head
// remains subject to the header limits (max_http_header_bytes and
// max_http_header_fields). An invalid status
// line or a line ending in a bare LF fails as soon as it arrives, without
// waiting for the rest of the head.
class http1_client_response_parser final {
public:
    explicit http1_client_response_parser(http1_client_exchange_state exchange_state) noexcept;

    http1_client_response_parser(const http1_client_response_parser&) = delete;
    http1_client_response_parser& operator=(const http1_client_response_parser&) = delete;
    http1_client_response_parser(http1_client_response_parser&&) = delete;
    http1_client_response_parser& operator=(http1_client_response_parser&&) = delete;

    // Reports that the driver released the Expect gate without receiving 100,
    // for example after its finite wait. This neither completes request content
    // nor substitutes for the 100 required before 101. A terminal response-head
    // exchange is unchanged; always honor the plan's request-content signal.
    void release_request_content() noexcept;

    // Reports request-content completion while response heads are still pending.
    // exchange_terminal refers to response-head parsing, not an instruction to
    // stop the external request writer; follow the plan's content signal.
    [[nodiscard]] http1_client_request_content_completion_status complete_request_content() noexcept;

    // After need_more, append to the same logical prefix; after a parsed head,
    // remove consumed_bytes before supplying the following response.
    // Allocation failures propagate without completing or failing the exchange;
    // after reclaiming resources, retry with the same response prefix.
    [[nodiscard]] http1_client_response_parse_result parse(std::string_view buffer);

private:
    [[nodiscard]] static detail::http1_client_request_content_phase
    initial_request_content_phase(const http1_client_exchange_state& state_value) noexcept;

    enum class phase_type : std::uint8_t {
        await_response,
        complete,
        failed,
    };

    http1_client_exchange_state exchange_state_;
    phase_type phase_{phase_type::await_response};
    detail::http1_client_request_content_phase request_content_phase_;
    bool interim_faulty_framing_{false};
    std::size_t header_scan_offset_{0};
    // The pending head's status line, validated as soon as its line feed
    // arrives, and the offset of its CRLF; reset with header_scan_offset_ when
    // a head completes.
    std::optional<detail::http1_client_parsed_status_line> status_line_;
    std::size_t status_line_end_{0};
};

enum class http1_client_response_body_error : unsigned char {
    invalid_framing,
    invalid_transfer_coding,
    incomplete_body,
};

[[nodiscard]] std::string_view http1_client_response_body_error_message(
    http1_client_response_body_error error) noexcept;

class http1_client_response_body_decoder final {
public:
    class need_input_type final {
    public:
        [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
            return consumed_;
        }

    private:
        friend class http1_client_response_body_decoder;
        explicit constexpr need_input_type(std::size_t consumed) noexcept
            : consumed_(consumed) {}
        std::size_t consumed_;
    };

    class output_view_type final {
    public:
        [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
            return consumed_;
        }
        [[nodiscard]] constexpr std::string_view bytes() const& noexcept {
            return bytes_;
        }
        std::string_view bytes() const&& = delete;

    private:
        friend class http1_client_response_body_decoder;
        constexpr output_view_type(std::size_t consumed, std::string_view bytes_value) noexcept
            : consumed_(consumed),
              bytes_(bytes_value) {}
        std::size_t consumed_;
        std::string_view bytes_;
    };

    class validated_trailers_view_type final {
    public:
        [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
            return consumed_;
        }
        [[nodiscard]] constexpr std::span<const http_header> fields() const& noexcept {
            return fields_;
        }
        std::span<const http_header> fields() const&& = delete;

    private:
        friend class http1_client_response_body_decoder;
        constexpr validated_trailers_view_type(std::size_t consumed, std::span<const http_header> fields) noexcept
            : consumed_(consumed),
              fields_(fields) {}
        std::size_t consumed_;
        std::span<const http_header> fields_;
    };

    class complete_type final {
    public:
        [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
            return consumed_;
        }
        [[nodiscard]] constexpr http1_close_policy persistence() const noexcept {
            return persistence_;
        }

    private:
        friend class http1_client_response_body_decoder;
        constexpr complete_type(std::size_t consumed, http1_close_policy persistence) noexcept
            : consumed_(consumed),
              persistence_(persistence) {}
        std::size_t consumed_;
        http1_close_policy persistence_;
    };

    class protocol_failure_type final {
    public:
        [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
            return consumed_;
        }
        [[nodiscard]] constexpr http1_client_response_body_error error() const noexcept {
            return error_;
        }

    private:
        friend class http1_client_response_body_decoder;
        constexpr protocol_failure_type(std::size_t consumed, http1_client_response_body_error error) noexcept
            : consumed_(consumed),
              error_(error) {}
        std::size_t consumed_;
        http1_client_response_body_error error_;
    };

    class decoder_failure_type final {
    public:
        [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
            return consumed_;
        }

    private:
        friend class http1_client_response_body_decoder;
        explicit constexpr decoder_failure_type(std::size_t consumed) noexcept
            : consumed_(consumed) {}
        std::size_t consumed_;
    };

    class result_type final {
    public:
        [[nodiscard]] std::size_t consumed_bytes() const noexcept;
        [[nodiscard]] const need_input_type* need_input() const& noexcept;
        const need_input_type* need_input() const&& = delete;
        [[nodiscard]] const output_view_type* output() const& noexcept;
        const output_view_type* output() const&& = delete;
        [[nodiscard]] const validated_trailers_view_type* trailers() const& noexcept;
        const validated_trailers_view_type* trailers() const&& = delete;
        [[nodiscard]] const complete_type* complete() const& noexcept;
        const complete_type* complete() const&& = delete;
        [[nodiscard]] const protocol_failure_type* protocol_failure() const& noexcept;
        const protocol_failure_type* protocol_failure() const&& = delete;
        [[nodiscard]] const decoder_failure_type* decoder_failure() const& noexcept;
        const decoder_failure_type* decoder_failure() const&& = delete;

    private:
        friend class http1_client_response_body_decoder;
        using value_type = std::variant<need_input_type, output_view_type, validated_trailers_view_type, complete_type,
            protocol_failure_type, decoder_failure_type>;
        template <typename t_type>
        explicit result_type(t_type value) noexcept
            : value_(std::move(value)) {}
        value_type value_;
    };

    // Transfer coding is present only in chunked or close-delimited plans.
    // Fixed-length plans forward wire content without a transfer-decoding stage.
    // A plan that discards_content() consumes the content its framing delimits
    // (without transfer decoding) and never emits output.
    explicit http1_client_response_body_decoder(http1_client_response_plan plan);
    ~http1_client_response_body_decoder() = default;
    http1_client_response_body_decoder(const http1_client_response_body_decoder&) = delete;
    http1_client_response_body_decoder& operator=(const http1_client_response_body_decoder&) = delete;
    http1_client_response_body_decoder(http1_client_response_body_decoder&&) = delete;
    http1_client_response_body_decoder& operator=(http1_client_response_body_decoder&&) = delete;

    // Output borrows available without transfer coding and scratch otherwise;
    // consume it before changing either storage or making the next call.
    // Trailer fields are validated and owned by the decoder, allocated only
    // for a section that keeps a field: names are lowercase,
    // each obs-fold is already replaced with SP (RFC 9112 section 5.2), and
    // fields RFC 9110 section 6.5.1 forbids in response trailers are discarded
    // (RFC 9112 section 7.1.2). They stay valid until the next decode() or
    // finish_input() call, or until the decoder is destroyed.
    // Erase exactly consumed_bytes() from available.
    // After finish_input(), decode() continues with the same EOF state: it can
    // drain buffered input/output, but missing body bytes are an incomplete_body
    // failure rather than a request for more input. Terminal results replay
    // without consuming any following-message bytes.
    [[nodiscard]] result_type decode(std::string_view available, std::span<char> scratch);
    // EOF is monotonic. Supply any remaining buffered bytes; later calls may
    // drain decoder output but can never request more network input.
    [[nodiscard]] result_type finish_input(std::string_view available, std::span<char> scratch);
    template <detail::http_temporary_owning_char_string input_type>
    result_type decode(input_type&&, std::span<char>) = delete;
    template <detail::http_temporary_owning_char_string input_type>
    result_type finish_input(input_type&&, std::span<char>) = delete;

private:
    enum class framing_type : unsigned char { no_body,
        fixed,
        chunked,
        close_delimited };
    enum class transfer_phase_type : unsigned char { needs_framed_input,
        drain_output,
        ended };
    [[nodiscard]] result_type step(std::string_view available, std::span<char> scratch, bool eof);
    [[nodiscard]] result_type step_content(std::string_view available, std::span<char> scratch, bool eof);
    [[nodiscard]] result_type step_fixed(std::string_view available, std::span<char> scratch, bool eof);
    [[nodiscard]] result_type step_close_delimited(std::string_view available, std::span<char> scratch, bool eof);
    [[nodiscard]] result_type step_chunked(std::string_view available, std::span<char> scratch, bool eof);
    [[nodiscard]] std::optional<result_type> drain_pending_chunk(
        std::string_view available, std::size_t& consumed, std::span<char> scratch, bool eof);
    void consume_pending_delimiter(std::string_view available, std::size_t& consumed) noexcept;
    [[nodiscard]] result_type replay_terminal() const noexcept;
    [[nodiscard]] result_type need_input(std::size_t consumed = 0) const noexcept;
    [[nodiscard]] result_type need_input_or_incomplete(std::size_t consumed, bool eof);
    [[nodiscard]] result_type emit(std::size_t consumed, std::string_view bytes);
    [[nodiscard]] result_type emit_transfer_output(std::size_t consumed, std::string_view bytes);
    [[nodiscard]] static result_type with_consumed(const result_type& result, std::size_t consumed) noexcept;
    [[nodiscard]] static result_type with_consumed_prefix(const result_type& result, std::size_t prefix) noexcept;
    [[nodiscard]] result_type validated_trailers(std::size_t consumed, std::span<const http_header> fields);
    [[nodiscard]] result_type complete(std::size_t consumed);
    [[nodiscard]] result_type fail(http1_client_response_body_error error, std::size_t consumed = 0);
    [[nodiscard]] result_type decoder_failure(std::size_t consumed = 0);
    [[nodiscard]] result_type drive_transfer(
        std::string_view input, std::size_t wire_prefix, std::span<char> scratch);
    [[nodiscard]] result_type finish_transfer(std::size_t consumed, std::span<char> scratch);

    using terminal_type = std::variant<complete_type, protocol_failure_type, decoder_failure_type>;
    framing_type framing_;
    http1_close_policy persistence_{http1_close_policy::close_after_response};
    std::optional<http_transfer_coding_stack_decoder> transfer_;
    std::optional<http1_chunked_body_decoder> chunked_;
    std::optional<terminal_type> terminal_;
    std::size_t remaining_{0};
    // The chunk parser advances through a whole body view. These counters let
    // the caller retain its unconsumed wire suffix without storing a borrowed view.
    std::size_t pending_body_bytes_{0};
    std::size_t pending_delimiter_bytes_{0};
    bool discard_content_{false};
    bool eof_{false};
    bool trailers_reported_{false};
    transfer_phase_type transfer_phase_{transfer_phase_type::needs_framed_input};
};

// awaiting_peer_eof: a client completed the close handshake and, per RFC 6455
// section 7.1.1, leaves closing the transport to the server. Its driver keeps
// reading (next_event() discards input) until notify_transport_eof(), which
// leads to the transport-end plan; if no EOF arrives within a reasonable time
// the client may close first, through abort().
enum class websocket_liveness_mode : std::uint8_t { open,
    awaiting_peer_close,
    awaiting_peer_eof,
    inactive };

// The supported Close status space from RFC 6455 section 7.4 and the IANA
// WebSocket Close Code registry. Role-specific restrictions remain with the
// connection. Local-only sentinel codes such as 1005/1006 are not wire values.
class websocket_close_code final {
public:
    template <std::integral integer_type>
    [[nodiscard]] static constexpr websocket_close_code from_value(integer_type value) {
        if (!is_valid_value(value)) {
            throw std::invalid_argument("invalid WebSocket Close status code");
        }
        return websocket_close_code(static_cast<std::uint16_t>(value), validated_tag_type{});
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<websocket_close_code> try_from_value(
        integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return websocket_close_code(static_cast<std::uint16_t>(value), validated_tag_type{});
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        const auto integer_value = +value;
        return (integer_value >= 1000 && integer_value <= 1003) ||
               (integer_value >= 1007 && integer_value <= 1014) ||
               (integer_value >= 3000 && integer_value <= 4999);
    }

    [[nodiscard]] constexpr std::uint16_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(websocket_close_code, websocket_close_code) = default;

private:
    struct validated_tag_type {};

    constexpr websocket_close_code(std::uint16_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint16_t value_;
};

static_assert(std::is_trivially_copyable_v<websocket_close_code>);
static_assert(sizeof(websocket_close_code) == sizeof(std::uint16_t));

namespace websocket_close_status {
inline constexpr auto normal_closure = websocket_close_code::from_value(1000);
inline constexpr auto going_away = websocket_close_code::from_value(1001);
inline constexpr auto internal_error = websocket_close_code::from_value(1011);
}  // namespace websocket_close_status

enum class websocket_opcode : std::uint8_t {
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA
};
// Exact RFC 7692 negotiation, including independent sender dictionaries and
// windows. Omitted max-window parameters imply 15; presence also controls the
// negotiated extension's wire metadata.
struct websocket_compression final {
    bool enabled_{false};
    bool server_no_context_takeover_{true};
    bool client_no_context_takeover_{true};
    std::optional<int> server_max_window_bits_{};
    std::optional<int> client_max_window_bits_{};
    bool operator==(const websocket_compression&) const = default;
};

// zlib parameters of an endpoint's permessage-deflate deflater, which RFC 7692
// leaves to the sender: compression level (0-9) and memLevel (1-9). Only a
// direction negotiated with context takeover keeps a private deflater, created
// on its first compressed message: directions without takeover (the default)
// share the worker's codecs, which use zlib's default memLevel, and allocate no
// codec state per connection. memLevel sizes the hash heads plus pending
// buffer at 2^(memLevel + 9) bytes on top of the 128 KiB window and history of
// a 15-bit window: 128 KiB at 8, 8 KiB at 4, so 4 saves 120 KiB per takeover
// connection at a lower ratio.
struct websocket_deflater_config final {
    int compression_level_{6};
    int mem_level_{8};
};

// The one range check of websocket_deflater_config; throws
// std::invalid_argument outside zlib's ranges.
inline void validate_websocket_deflater_config(websocket_deflater_config config) {
    if (config.compression_level_ < 0 || config.compression_level_ > 9) {
        throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
    }
    if (config.mem_level_ < 1 || config.mem_level_ > 9) {
        throw std::invalid_argument("WebSocket deflate memory level must be between 1 and 9");
    }
}

// Server permessage-deflate policy.
struct websocket_deflate_config final {
    bool enabled_{true};
    // Opt-in: never mix secrets and attacker-controlled content in one dictionary.
    // A peer requesting no-context-takeover still receives independent messages.
    bool context_takeover_{false};
    websocket_deflater_config deflater_{};
};

// Client permessage-deflate offer. compression_ holds the offered extension
// parameters (RFC 7692 section 7.1); offer_client_max_window_bits_ adds the
// valueless client_max_window_bits that lets the server pick the client window
// (section 7.1.2.2) and is required when compression_.client_max_window_bits_
// is set.
struct websocket_client_deflate_offer final {
    websocket_compression compression_{};
    bool offer_client_max_window_bits_{true};
    websocket_deflater_config deflater_{};
};

class websocket_message final {
public:
    // Construct a zero-copy message over caller-owned payload storage, rejecting
    // direct owning-string temporaries. Keep that storage alive and unchanged
    // until the view, or a send operation borrowing it, is no longer in use.
    [[nodiscard]] static constexpr websocket_message borrow(
        websocket_opcode opcode, borrowed_text payload_value) noexcept {
        return websocket_message(opcode, payload_value.view());
    }

    websocket_message(const websocket_message&) noexcept = default;
    websocket_message& operator=(const websocket_message&) noexcept = default;
    websocket_message(websocket_message&&) noexcept = default;
    websocket_message& operator=(websocket_message&&) noexcept = default;

    [[nodiscard]] constexpr websocket_opcode opcode() const noexcept {
        return opcode_;
    }

    // The payload view borrows the connection read buffer: it is valid only
    // until the next websocket read() on the same connection (the same rule as
    // body_reader::read). Copy the payload before the next read if it must
    // outlive the current message.
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

    [[nodiscard]] bool text() const noexcept {
        return opcode_ == websocket_opcode::text;
    }

    [[nodiscard]] bool binary() const noexcept {
        return opcode_ == websocket_opcode::binary;
    }

private:
    constexpr websocket_message(websocket_opcode opcode, std::string_view payload_value) noexcept
        : opcode_(opcode),
          payload_(payload_value) {}

    websocket_opcode opcode_{websocket_opcode::text};
    std::string_view payload_;
};

class websocket_connection;

enum class websocket_transport_disposition : std::uint8_t { keep_open,
    end_transport };
enum class websocket_frame_submit_status : std::uint8_t {
    accepted,
    not_open,
    invalid_opcode,
    message_too_large,
    invalid_text_payload,
    control_frame_too_large,
    // A fragmented data message is still open; only control frames and frames
    // of its own opcode may be submitted until its final fragment.
    message_in_progress
};
enum class websocket_close_submit_status : std::uint8_t {
    accepted,
    already_closing,
    closed,
    invalid_code,
    invalid_reason,
    reason_too_large
};
enum class websocket_abort_disposition : std::uint8_t { abort_transport,
    no_transport_action };
// Why a WebSocket session ended, as this endpoint observed it; none while it is
// still active. The first cause wins: a later Close, EOF or abort never
// replaces it. The peer Close status itself is the close event's.
enum class websocket_termination : std::uint8_t {
    none,
    // The peer started the close handshake.
    peer_close,
    // The peer answered a locally started close handshake.
    local_close,
    // The transport ended without a Close frame (RFC 6455 section 7.1.5, 1006).
    eof,
    // An invalid frame or message (sent Close 1002).
    protocol_error,
    // A text message or Close reason that is not UTF-8 (sent Close 1007).
    invalid_payload_data,
    // A frame, message or inflated message above the receive limit (sent Close 1009).
    message_too_large,
    // A driver deadline: heartbeat Pong, close handshake or operation timeout.
    timeout,
    // Ended locally: abort(), a local failure or the session's teardown.
    aborted,
    // The transport failed to read or write.
    io_error,
};
enum class websocket_output_consume_status : std::uint8_t { pending,
    drained,
    out_of_range };
enum class websocket_connection_role : std::uint8_t { server,
    client };
using websocket_mask_key_type = std::array<char, 4>;
// Client transports must supply a fresh cryptographically random key per call.
// The context is borrowed and must outlive the connection. Failure is fatal to
// the transport: operations may throw and the caller must abort the connection.
using websocket_mask_key_generator_type = bool (*)(void*, websocket_mask_key_type&) noexcept;

// The pending transport output: bytes() followed by borrowed_bytes(), written in
// that order. borrowed_bytes() is a payload the driver submitted by reference
// (websocket_connection::submit_frame with a websocket_borrowed_payload); it is
// empty otherwise.
class websocket_output_plan final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] constexpr std::string_view borrowed_bytes() const noexcept {
        return borrowed_bytes_;
    }
    // Total pending bytes: what consume_output() takes once both are written.
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return bytes_.size() + borrowed_bytes_.size();
    }
    [[nodiscard]] constexpr websocket_transport_disposition disposition() const noexcept {
        return disposition_;
    }

private:
    friend class websocket_connection;
    constexpr websocket_output_plan(std::string_view bytes_value, std::string_view borrowed_bytes_value,
        websocket_transport_disposition disposition) noexcept
        : bytes_(bytes_value),
          borrowed_bytes_(borrowed_bytes_value),
          disposition_(disposition) {}
    std::string_view bytes_;
    std::string_view borrowed_bytes_;
    websocket_transport_disposition disposition_;
};

enum class websocket_event_kind : std::uint8_t { message,
    ping,
    pong,
    close,
    protocol_error,
    transport_end };

class websocket_ping_event final {
public:
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class websocket_event;
    explicit constexpr websocket_ping_event(std::string_view payload_value) noexcept
        : payload_(payload_value) {}
    std::string_view payload_;
};

class websocket_pong_event final {
public:
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class websocket_event;
    explicit constexpr websocket_pong_event(std::string_view payload_value) noexcept
        : payload_(payload_value) {}
    std::string_view payload_;
};

class websocket_close_event final {
public:
    // RFC 6455 section 7.1.5 uses 1005 when the peer supplied no status code;
    // section 7.4.1 reserves it from ever being emitted on wire.
    [[nodiscard]] constexpr std::uint16_t close_code() const noexcept {
        return close_code_;
    }
    [[nodiscard]] constexpr std::string_view reason() const noexcept {
        return reason_;
    }

private:
    friend class websocket_event;
    constexpr websocket_close_event(std::uint16_t close_code, std::string_view reason) noexcept
        : close_code_(close_code),
          reason_(reason) {}
    std::uint16_t close_code_;
    std::string_view reason_;
};

class websocket_protocol_error_event final {
public:
    [[nodiscard]] constexpr std::uint16_t close_code() const noexcept {
        return close_code_;
    }

private:
    friend class websocket_event;
    explicit constexpr websocket_protocol_error_event(std::uint16_t close_code) noexcept
        : close_code_(close_code) {}
    std::uint16_t close_code_;
};

class websocket_transport_end_event final {
private:
    friend class websocket_event;
    constexpr websocket_transport_end_event() noexcept = default;
};

// One protocol value shared by both public drivers and the sans-I/O core.
// Payload/reason views borrow the driver's input or message assembly storage;
// the next next_event(), feed() or an append to the driver's input buffer may
// invalidate those views.
class websocket_event final {
public:
    [[nodiscard]] websocket_event_kind kind() const noexcept {
        return static_cast<websocket_event_kind>(value_.index());
    }
    [[nodiscard]] const websocket_message* message() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_message>(&value_);
    }
    const websocket_message* message() const&& = delete;
    [[nodiscard]] const websocket_ping_event* ping() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_ping_event>(&value_);
    }
    const websocket_ping_event* ping() const&& = delete;
    [[nodiscard]] const websocket_pong_event* pong() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_pong_event>(&value_);
    }
    const websocket_pong_event* pong() const&& = delete;
    [[nodiscard]] const websocket_close_event* close() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_close_event>(&value_);
    }
    const websocket_close_event* close() const&& = delete;
    [[nodiscard]] const websocket_protocol_error_event* protocol_error() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_protocol_error_event>(&value_);
    }
    const websocket_protocol_error_event* protocol_error() const&& = delete;
    [[nodiscard]] const websocket_transport_end_event* transport_end() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_transport_end_event>(&value_);
    }
    const websocket_transport_end_event* transport_end() const&& = delete;

private:
    friend class websocket_connection;
    using value_type = std::variant<websocket_message, websocket_ping_event, websocket_pong_event,
        websocket_close_event, websocket_protocol_error_event, websocket_transport_end_event>;
    static_assert(static_cast<std::uint8_t>(websocket_event_kind::transport_end) + 1 == std::variant_size_v<value_type>);
    template <typename event_type>
    explicit websocket_event(event_type event) noexcept
        : value_(std::move(event)) {}
    [[nodiscard]] static websocket_event message(websocket_opcode opcode, std::string_view payload_value) noexcept {
        return websocket_event(websocket_message::borrow(opcode, payload_value));
    }
    [[nodiscard]] static websocket_event ping(std::string_view payload_value) noexcept {
        return websocket_event(websocket_ping_event(payload_value));
    }
    [[nodiscard]] static websocket_event pong(std::string_view payload_value) noexcept {
        return websocket_event(websocket_pong_event(payload_value));
    }
    [[nodiscard]] static websocket_event close(std::uint16_t code, std::string_view reason) noexcept {
        return websocket_event(websocket_close_event(code, reason));
    }
    [[nodiscard]] static websocket_event protocol_error(std::uint16_t code) noexcept {
        return websocket_event(websocket_protocol_error_event(code));
    }
    [[nodiscard]] static websocket_event make_transport_end() noexcept {
        return websocket_event(websocket_transport_end_event());
    }
    value_type value_;
};

namespace detail {
class zlib_deflate_stream;
class zlib_inflate_stream;
}  // namespace detail

enum class websocket_inflate_result : std::uint8_t {
    ok,
    error,
    too_large,
};

// RFC 7692 permessage-deflate codecs for directions negotiated without context
// takeover. Such a direction starts every message from an empty dictionary,
// so one owner (for example a worker) can serve any number of connections
// with one codec set instead of a dictionary pair per connection.
//
// Each call handles one whole message and resets the codec it used before
// returning; a codec left by an exception is discarded and recreated by the
// next use: no message, and no failed message, ever influences another. Raw
// deflaters are created on first use for each (compression level, window
// bits) pair; one raw inflater with a 15-bit window serves every receive
// window. Not thread-safe; the address is stable for the borrowers.
class websocket_deflate_codecs final {
public:
    websocket_deflate_codecs() noexcept = default;
    ~websocket_deflate_codecs();
    websocket_deflate_codecs(const websocket_deflate_codecs&) = delete;
    websocket_deflate_codecs& operator=(const websocket_deflate_codecs&) = delete;
    websocket_deflate_codecs(websocket_deflate_codecs&&) = delete;
    websocket_deflate_codecs& operator=(websocket_deflate_codecs&&) = delete;

    // Appends the compressed message payload to `out` without the trailing
    // 0x00 0x00 0xFF 0xFF of the final flush (RFC 7692 section 7.2.1). The
    // level is 0 through 9 and the negotiated window 8 through 15; anything
    // else throws std::invalid_argument. false reports a codec failure;
    // allocation exceptions propagate with their original type.
    [[nodiscard]] bool deflate_message(
        std::string_view payload, std::string& out, int compression_level, int window_bits);

    // Appends the decompressed message to `out`, bounded by `message_limit`
    // across everything this call appends (RFC 7692 section 7.2.2).
    [[nodiscard]] websocket_inflate_result inflate_message(
        std::string_view payload, std::string& out, protocol_byte_limit message_limit);

private:
    static constexpr int min_window_bits = 8;
    static constexpr int max_window_bits = 15;
    static constexpr int compression_levels = 10;

    std::array<detail::zlib_deflate_stream*,
        static_cast<std::size_t>((max_window_bits - min_window_bits + 1) * compression_levels)>
        deflaters_{};
    detail::zlib_inflate_stream* inflater_{nullptr};
};

enum class websocket_feed_status : std::uint8_t { accepted,
    inactive,
    backpressured };

struct websocket_frame_options final {
    // False sends a non-final fragment of a text or binary message.
    bool final_{true};
    // Selects permessage-deflate, when negotiated, for a message's first frame.
    bool compress_{true};
};

// A frame payload websocket_connection::submit_frame() queues by reference
// instead of copying. Construction rejects temporary owning strings, whose
// bytes would expire before the transport writes them.
class websocket_borrowed_payload final {
public:
    explicit constexpr websocket_borrowed_payload(borrowed_text bytes) noexcept
        : bytes_(bytes) {}

    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_.view();
    }

private:
    borrowed_text bytes_;
};

struct websocket_connection_options final {
    protocol_byte_limit message_limit_{protocol_byte_limit::limited(default_max_websocket_message_bytes)};
    // Additional receive-only ceiling, including message aggregation and inflation.
    // The smaller of this limit and message_limit_ applies to input; output is unchanged.
    protocol_byte_limit receive_message_limit_{};
    websocket_compression compression_{(websocket_compression{})};
    websocket_connection_role role_{websocket_connection_role::server};
    websocket_mask_key_generator_type mask_key_generator_{nullptr};
    void* mask_key_context_{nullptr};
    // The private deflater of a direction negotiated with context takeover.
    websocket_deflater_config deflater_{};
    // Borrowed codecs for every permessage-deflate direction negotiated without
    // context takeover; they must outlive the connection and are used only
    // synchronously inside its calls. nullptr gives such a direction a private
    // codec too. Private codecs are created on the direction's first
    // compressed message.
    websocket_deflate_codecs* shared_deflate_{nullptr};
    // Independent bound for feed() bytes awaiting next_event(). A rejected feed
    // consumes nothing; drain events or provide smaller chunks before retrying.
    std::size_t max_buffered_input_bytes_{default_max_websocket_message_bytes + 14};
};

// Sans-I/O RFC 6455 driver for an already upgraded connection. Input arrives
// through feed(), into a buffer the connection owns unless it was constructed
// with the caller's input buffer; with a borrowed buffer the caller may also
// append received bytes directly before calling next_event(). A borrowed
// buffer must outlive the
// connection. The role fixes inbound mask validation and outbound masking,
// including automatic Pong/Close. After the close handshake a server plans
// end_transport once its output is flushed; a client keeps the transport open
// (liveness awaiting_peer_eof, which feed() and next_event() answer by
// discarding input) until notify_transport_eof(), because the server closes
// first (RFC 6455 section 7.1.1). Event views remain valid until the next
// feed(), next_event() or an append to a borrowed input buffer. Output bytes
// remain valid until next_event(), submit_frame(), submit_close(),
// consume_output(), or destruction. Input, EOF and abort do not invalidate an
// in-flight write.
class websocket_connection final {
public:
    explicit websocket_connection(websocket_connection_options options = {});
    explicit websocket_connection(http_io_buffer& input, websocket_connection_options options = {});
    ~websocket_connection();
    websocket_connection(const websocket_connection&) = delete;
    websocket_connection& operator=(const websocket_connection&) = delete;
    websocket_connection(websocket_connection&&) noexcept;
    websocket_connection& operator=(websocket_connection&&) noexcept;

    // Input may borrow the previous event payload; any required snapshot is
    // taken before reclaiming consumed input storage.
    [[nodiscard]] websocket_feed_status feed(std::string_view input);
    template <detail::http_temporary_owning_char_string input_type>
    websocket_feed_status feed(input_type&&) = delete;
    [[nodiscard]] std::optional<websocket_event> next_event() & RUVIA_LIFETIMEBOUND;
    std::optional<websocket_event> next_event() && = delete;
    [[nodiscard]] std::size_t needed_input_bytes() const noexcept;
    [[nodiscard]] websocket_output_plan output_plan() const& noexcept RUVIA_LIFETIMEBOUND;
    websocket_output_plan output_plan() const&& = delete;
    [[nodiscard]] websocket_output_consume_status consume_output(std::size_t bytes) noexcept;
    void commit_transport_end() noexcept;
    // Records eof as the termination unless an earlier cause was recorded.
    void notify_transport_eof() noexcept;
    // `cause` tells why the driver aborts (none records aborted); an earlier
    // termination cause is kept.
    [[nodiscard]] websocket_abort_disposition abort(
        websocket_termination cause = websocket_termination::aborted) noexcept;
    [[nodiscard]] websocket_liveness_mode liveness_mode() const noexcept;
    [[nodiscard]] websocket_termination termination() const noexcept;
    // RFC 6455 wire bytes of whole frames: header, mask key and payload as
    // framed, so after permessage-deflate, data and control frames alike.
    // Received counts every frame next_event() parsed; sent counts the output
    // consume_output() acknowledged. Both saturate.
    [[nodiscard]] std::uint64_t bytes_received() const noexcept;
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept;
    // One frame of a text or binary message, or a Ping or Pong. A data frame is
    // a fragment of its message (RFC 6455 section 5.4): a final frame that
    // continues no open message is a whole message; with final_ false the
    // first frame opens the message with `opcode`, and with RSV1 when
    // compress_ selects permessage-deflate; later frames of the same opcode
    // are its continuation frames, and the final one ends it. Ping, Pong and
    // Close may be submitted between fragments; a data frame of the other
    // opcode reports message_in_progress until the final fragment, and a
    // non-final control frame reports invalid_opcode (section 5.5). A whole
    // message stays uncompressed when deflate does not shrink it; a
    // fragmented compressed message continues one DEFLATE stream across its
    // fragments, each ending at a sync flush (RFC 7692 section 7.2.1), and its
    // first frame fixes compression for all of them. Text is validated across
    // fragment boundaries and the message limit covers the whole message. A
    // rejected frame leaves an open message open. A failed submission
    // preserves queued output and discards any unsent compression history. It
    // can be retried unless resetting the codec also failed, in which case
    // liveness_mode() reports inactive.
    // This form copies the payload into the connection's output before returning.
    [[nodiscard]] websocket_frame_submit_status submit_frame(
        websocket_opcode opcode, std::string_view payload, websocket_frame_options options = {});
    // The same submission queuing an uncompressed, unmasked payload by
    // reference: output_plan() exposes it as borrowed_bytes() after the frame
    // header. Its bytes must stay alive and unchanged until consume_output()
    // has acknowledged them, abort(), or destruction. A masked (client) or
    // compressed frame, or a payload overlapping the pending output, is copied.
    [[nodiscard]] websocket_frame_submit_status submit_frame(
        websocket_opcode opcode, websocket_borrowed_payload payload, websocket_frame_options options = {});
    [[nodiscard]] websocket_close_submit_status submit_close(
        websocket_close_code code, std::string_view reason);

private:
    class impl_type;
    std::unique_ptr<impl_type> impl_;
};

}  // namespace ruvia

#pragma once

namespace ruvia {

inline constexpr std::size_t max_websocket_subprotocols = 64;

// Allocation-free validated RFC 6455 subprotocol list, suitable for server
// route configuration and parsing offered protocol lists. Capacity is a product
// limit of max_websocket_subprotocols distinct protocols across all list elements;
// a handshake offering more is rejected as invalid rather than truncated.
class websocket_subprotocol_set final {
public:
    // Strict: every protocol must be a new token (configuration and the
    // offers a client sends, which RFC 6455 section 4.1 requires to be unique).
    [[nodiscard]] bool append(borrowed_text protocol) noexcept {
        const auto protocol_view = protocol.view();
        if (!is_valid_http_header_name(protocol_view) || size_ == protocols_.size() || contains(protocol_view)) {
            return false;
        }
        protocols_[size_++] = protocol_view;
        return true;
    }

    // Strict list form of append(); empty list elements are ignored.
    [[nodiscard]] bool append_list(borrowed_text value) noexcept {
        return append_elements(value.view(), false);
    }

    // A received Sec-WebSocket-Protocol offer. Uniqueness binds the sending
    // client only (RFC 6455 section 4.1); a server receiving a repeated
    // protocol skips it (section 4.2.1 imposes no recipient check).
    [[nodiscard]] bool append_offered_list(borrowed_text value) noexcept {
        return append_elements(value.view(), true);
    }

    [[nodiscard]] bool contains(std::string_view protocol) const noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            if (protocols_[i] == protocol) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

private:
    [[nodiscard]] bool append_elements(std::string_view value, bool skip_duplicates) noexcept {
        return detail::http_visit_token_list(value, detail::http_field_list_role::recipient,
                   [this, skip_duplicates](std::string_view protocol) noexcept {
                       return (skip_duplicates && contains(protocol)) || append(protocol);
                   }) == detail::http_field_list_parse_status::ok;
    }

    std::array<std::string_view, max_websocket_subprotocols> protocols_{};
    std::size_t size_{0};
};

// Server handshake policy shared by HTTP/1.1 Upgrade and RFC 8441 Extended
// CONNECT.
struct websocket_server_handshake_options final {
    // Server preference order. Every entry must be a nonempty, unique HTTP token.
    borrowed_span<std::string_view> supported_subprotocols_{};
    // Additional response fields, copied before the response plan is returned.
    // Framing, connection, and Sec-websocket fields belong to the handshake
    // itself. Names/values must be valid fields; values cannot start or end
    // with SP/HTAB.
    borrowed_http_headers response_headers_{};
    websocket_deflate_config deflate_{};
};

struct websocket_handshake_validation_options final {
    // RFC 6455 4.2.2 lets a server that only speaks particular subprotocols
    // refuse a client offering none of them, instead of accepting without a
    // Sec-WebSocket-Protocol response field. When set, a request whose offer
    // shares no entry with supported_subprotocols_ fails validation.
    borrowed_span<std::string_view> supported_subprotocols_{};
    bool require_subprotocol_{false};
};

class websocket_handshake_accepted final {
private:
    friend class websocket_handshake_validation_result;

    constexpr websocket_handshake_accepted() noexcept = default;
};

class websocket_handshake_failure final {
public:
    [[nodiscard]] http_protocol_error protocol_error() const noexcept;

    // RFC 6455 requires a server rejecting an unsupported version to advertise
    // every supported version. Apply this after any application error handler.
    void apply_required_response_headers(http_response& response) const;

private:
    friend class websocket_handshake_validation_result;

    enum class kind_type : std::uint8_t { invalid_request,
        unsupported_version,
        unsupported_subprotocol };

    explicit constexpr websocket_handshake_failure(kind_type kind) noexcept
        : kind_(kind) {}

    kind_type kind_;
};

class websocket_handshake_validation_result;
class http2_connection;
class http3_connection;
class http3_request_head;
namespace detail {
class websocket_compression_extension final {
public:
    explicit constexpr websocket_compression_extension(websocket_compression compression) noexcept {
        if (!compression.enabled_) {
            return;
        }
        append("permessage-deflate");
        if (compression.server_no_context_takeover_) {
            append("; server_no_context_takeover");
        }
        if (compression.client_no_context_takeover_) {
            append("; client_no_context_takeover");
        }
        if (compression.server_max_window_bits_) {
            append("; server_max_window_bits=");
            number(*compression.server_max_window_bits_);
        }
        if (compression.client_max_window_bits_) {
            append("; client_max_window_bits=");
            number(*compression.client_max_window_bits_);
        }
    }
    [[nodiscard]] constexpr std::string_view view() const& noexcept {
        return {bytes_.data(), size_};
    }
    std::string_view view() const&& = delete;
    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0;
    }

private:
    constexpr void append(std::string_view text) noexcept {
        for (char ch : text) {
            bytes_[size_++] = ch;
        }
    }
    constexpr void number(int value) noexcept {
        if (value >= 10) {
            bytes_[size_++] = '1';
        }
        bytes_[size_++] = static_cast<char>('0' + value % 10);
    }
    std::array<char, 160> bytes_{};
    std::size_t size_{0};
};
class http2_connection;
class http2_stream_state;
class websocket_request_header_validator;
inline constexpr std::string_view websocket_switching_protocols_head_prefix =
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
[[nodiscard]] websocket_handshake_validation_result validate_http2_websocket_handshake(
    const http2_stream_state&, const http_request&,
    websocket_handshake_validation_options) noexcept;
}  // namespace detail

class websocket_handshake_validation_result final {
public:
    [[nodiscard]] constexpr const websocket_handshake_accepted* accepted() const& noexcept RUVIA_LIFETIMEBOUND {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const websocket_handshake_accepted* accepted() const&& = delete;

    [[nodiscard]] constexpr const websocket_handshake_failure* failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const websocket_handshake_failure* failure() const&& = delete;

private:
    friend class detail::websocket_request_header_validator;
    friend websocket_handshake_validation_result validate_http2_websocket_handshake(
        http2_connection&, std::uint32_t, const http_request&,
        websocket_handshake_validation_options) noexcept;
    friend websocket_handshake_validation_result detail::validate_http2_websocket_handshake(
        const detail::http2_stream_state&, const http_request&,
        websocket_handshake_validation_options) noexcept;
    friend websocket_handshake_validation_result validate_http3_websocket_handshake(
        const http3_request_head&, const http_request&, websocket_handshake_validation_options) noexcept;
    friend websocket_handshake_validation_result validate_websocket_handshake(
        const http_request&, const http1_request_body_plan&, websocket_handshake_validation_options) noexcept;

    using value_type = std::variant<websocket_handshake_accepted, websocket_handshake_failure>;

    explicit constexpr websocket_handshake_validation_result(
        websocket_handshake_accepted accepted) noexcept
        : value_(accepted) {}

    explicit constexpr websocket_handshake_validation_result(
        websocket_handshake_failure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static constexpr websocket_handshake_validation_result make_accepted() noexcept {
        return websocket_handshake_validation_result(websocket_handshake_accepted());
    }

    [[nodiscard]] static constexpr websocket_handshake_validation_result
    make_invalid_request() noexcept {
        return websocket_handshake_validation_result(
            websocket_handshake_failure(websocket_handshake_failure::kind_type::invalid_request));
    }

    [[nodiscard]] static constexpr websocket_handshake_validation_result
    make_unsupported_version() noexcept {
        return websocket_handshake_validation_result(
            websocket_handshake_failure(websocket_handshake_failure::kind_type::unsupported_version));
    }

    [[nodiscard]] static constexpr websocket_handshake_validation_result
    make_unsupported_subprotocol() noexcept {
        return websocket_handshake_validation_result(
            websocket_handshake_failure(websocket_handshake_failure::kind_type::unsupported_subprotocol));
    }

    value_type value_;
};

// The negotiation an RFC 8441 (HTTP/2) or RFC 9220 (HTTP/3) extended CONNECT
// 200 response committed on the wire. The runtime configures its websocket
// connection from this value, never from a separately recomputed
// compression/subprotocol tuple.
class websocket_extended_connect_negotiation final {
public:
    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;
    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

private:
    friend class http2_connection;
    friend class detail::http2_connection;
    friend class http3_connection;
    websocket_extended_connect_negotiation(std::string subprotocol, websocket_compression compression) noexcept
        : subprotocol_(std::move(subprotocol)),
          compression_(compression) {}
    std::string subprotocol_;
    websocket_compression compression_;
};

// Validates an RFC 6455 HTTP/1.1 opening handshake, including the parser-owned
// request-body framing plan. It does not create or write a response.
[[nodiscard]] websocket_handshake_validation_result validate_websocket_handshake(
    const http_request& request, const http1_request_body_plan& body_plan,
    websocket_handshake_validation_options options = {}) noexcept;

// Owned HTTP/1.1 101 response plan. for_each_response_part emits stable views for
// scatter-gather I/O; compression() configures the subsequent websocket driver
// from the exact negotiation encoded in this response.
class websocket_server_handshake final {
public:
    websocket_server_handshake(const websocket_server_handshake&) = delete;
    websocket_server_handshake& operator=(const websocket_server_handshake&) = delete;
    websocket_server_handshake(websocket_server_handshake&&) noexcept = default;
    websocket_server_handshake& operator=(websocket_server_handshake&&) = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] constexpr websocket_compression compression() const noexcept {
        return compression_;
    }

    template <typename visitor_type>
    void for_each_response_part(visitor_type&& visitor) const& {
        visitor(detail::websocket_switching_protocols_head_prefix);
        visitor(std::string_view(accept_.data(), accept_.size()));
        visitor(crlf);
        if (!subprotocol_.empty()) {
            visitor(subprotocol_header_prefix);
            visitor(std::string_view(subprotocol_));
            visitor(crlf);
        }
        if (!compression_extension_.empty()) {
            visitor(extensions_header_prefix);
            visitor(compression_extension_.view());
            visitor(crlf);
        }
        for (const auto& header : response_headers_) {
            visitor(header.name());
            visitor(std::string_view(": "));
            visitor(header.value());
            visitor(crlf);
        }
        visitor(crlf);
    }

    template <typename visitor_type>
    void for_each_response_part(visitor_type&&) const&& = delete;

private:
    friend websocket_server_handshake make_websocket_server_handshake(
        const http_request&, websocket_server_handshake_options);

    inline static constexpr std::string_view subprotocol_header_prefix = "Sec-WebSocket-Protocol: ";
    inline static constexpr std::string_view extensions_header_prefix = "Sec-WebSocket-Extensions: ";
    inline static constexpr std::string_view crlf = "\r\n";

    websocket_server_handshake(std::array<char, 28> accept, std::string subprotocol,
        websocket_compression compression, std::vector<http_header> response_headers_value) noexcept
        : accept_(accept),
          subprotocol_(std::move(subprotocol)),
          compression_(compression),
          compression_extension_(compression),
          response_headers_(std::move(response_headers_value)) {}

    std::array<char, 28> accept_;
    std::string subprotocol_;
    websocket_compression compression_;
    detail::websocket_compression_extension compression_extension_;
    std::vector<http_header> response_headers_;
};

// Call after validate_websocket_handshake() succeeds. The selected subprotocol
// and response fields are copied; the response plan borrows no
// request or server-preference storage.
[[nodiscard]] websocket_server_handshake make_websocket_server_handshake(
    const http_request& request, websocket_server_handshake_options options = {});

struct websocket_client_negotiation_config_view final {
    borrowed_span<std::string_view> subprotocols_{};
    websocket_client_deflate_offer deflate_{};
};
// The one failure shape of a client opening handshake over HTTP/1.1 Upgrade
// (RFC 6455 section 4.1) or Extended CONNECT (RFC 8441 over HTTP/2, RFC 9220
// over HTTP/3).
enum class websocket_client_handshake_error : std::uint8_t {
    // Not 101 over HTTP/1.1, not 2xx over Extended CONNECT.
    response_status,
    // Extended CONNECT: the 2xx HEADERS ended the stream.
    stream_closed,
    // HTTP/1.1: Sec-WebSocket-Accept is missing, repeated or wrong.
    accept,
    // HTTP/1.1: no Upgrade: websocket.
    upgrade,
    // HTTP/1.1: no Connection: Upgrade.
    connection,
    subprotocol,
    extensions,
    // Extended CONNECT: a connection-specific response field (RFC 9113
    // section 8.2.2, RFC 9114 section 4.2).
    forbidden_field,
};
struct websocket_client_negotiation_result_view final {
    // Borrows the supplied response headers.
    std::string_view selected_subprotocol_{};
    websocket_compression compression_{};
};

class websocket_client_negotiation;
namespace detail {
// The server's Sec-WebSocket-Protocol and Sec-WebSocket-Extensions selection
// against the offer; `extended` (RFC 8441) also rejects connection-specific
// fields. The result view borrows `fields`.
[[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_websocket_client_selection(
    const websocket_client_negotiation& negotiation, std::span<const http_header> fields, bool extended);
}  // namespace detail

// HTTP-version-independent RFC 6455/7692 client negotiation owner. Configuration
// is copied once. It supplies request fields to HTTP/1 Upgrade or
// RFC 8441 Extended CONNECT, and validates the server's exact selection; the
// version-specific response validators check the same selection after their
// own status checks.
// All offer views expire with this owner; response views expire with the head.
class websocket_client_negotiation final {
public:
    // Validates the subprotocols and the deflate offer, including its deflater.
    static void validate_configuration(websocket_client_negotiation_config_view config);
    explicit websocket_client_negotiation(websocket_client_negotiation_config_view config);
    websocket_client_negotiation(websocket_client_negotiation&&) noexcept = default;
    websocket_client_negotiation& operator=(websocket_client_negotiation&&) = delete;
    websocket_client_negotiation(const websocket_client_negotiation&) = delete;
    websocket_client_negotiation& operator=(const websocket_client_negotiation&) = delete;
    [[nodiscard]] std::span<const http_header> request_fields() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::span<const http_header>(fields_.data(), field_count_);
    }
    std::span<const http_header> request_fields() const&& = delete;
    // The server's Sec-WebSocket-Protocol and Sec-WebSocket-Extensions selection
    // in `fields` alone, without status or connection-specific field checks.
    [[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
    validate_fields(std::span<const http_header> fields) const;
    template <detail::http_temporary_owning_range<http_header> fields_type>
    std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
    validate_fields(fields_type&&) const = delete;

private:
    friend std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
    detail::validate_websocket_client_selection(
        const websocket_client_negotiation&, std::span<const http_header>, bool);

    websocket_client_deflate_offer deflate_;
    std::array<http_header, 3> fields_{};
    std::size_t field_count_{0};
    // The owned Sec-WebSocket-Protocol request value; empty when none is offered.
    std::string_view offered_subprotocols_{};
};

// RFC 6455 section 4.1 opening handshake over HTTP/1.1 Upgrade. The caller
// owns the base64-encoded 16-byte nonce (`encoded_key`), the negotiation and the
// application headers for the whole exchange; subprotocols and the deflate offer
// are validated by websocket_client_negotiation.

// Rejects application headers the handshake or request writer generates itself
// (Host, framing, Expect, Upgrade/Connection and every Sec-WebSocket-* field)
// and any header the request writer would refuse.
void validate_http1_websocket_client_handshake_configuration(std::span<const http_header_view> headers);

[[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const http_header> headers, std::span<char> head_buffer,
    http_request_field_source source = http_request_field_source::generated);
[[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const http_header_view> headers, std::span<char> head_buffer,
    http_request_field_source source = http_request_field_source::generated);

// Validates the 101 response against the key, then the server's selection
// against the negotiation. The result view borrows the response head.
[[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_http1_websocket_client_response(std::span<const char, 24> encoded_key,
    const websocket_client_negotiation& negotiation, const http1_parsed_client_response_head& response RUVIA_LIFETIMEBOUND);
std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_http1_websocket_client_response(std::span<const char, 24>,
    const websocket_client_negotiation&, const http1_parsed_client_response_head&&) = delete;

// Extended CONNECT over HTTP/2 or HTTP/3: validates the server's 2xx final
// head on a still-open stream, then its selection against the negotiation.
// The result view borrows the response head.
[[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_extended_connect_websocket_client_response(
    const websocket_client_negotiation& negotiation, const http_client_response_head& head, bool stream_open);
std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_extended_connect_websocket_client_response(
    const websocket_client_negotiation&, const http_client_response_head&&, bool) = delete;
}  // namespace ruvia
