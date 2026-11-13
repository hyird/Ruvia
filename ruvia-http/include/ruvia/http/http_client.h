#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response_server.h"
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

namespace ruvia {

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

}  // namespace ruvia

namespace ruvia {

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

struct http1_client_request_prepare_result_access;

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

}  // namespace ruvia

namespace ruvia {
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
}  // namespace ruvia

namespace ruvia {

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
}  // namespace ruvia

namespace ruvia {

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

}  // namespace ruvia
