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

#include "borrowed_text.h"
#include "http1_server_response.h"
#include "http_header.h"
#include "http_message.h"
#include "http_request.h"

namespace ruvia {

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
    constexpr http_origin_view(http_scheme scheme, std::string_view host, std::uint16_t port) noexcept :
        host_(host),
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

    explicit constexpr http_client_request_bytes_view(std::string_view value) noexcept :
        value_(value) {}

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

    [[nodiscard]] static constexpr http_client_request_content_view bytes(std::string_view value) noexcept {
        return http_client_request_content_view(http_client_request_bytes_view(value));
    }

    [[nodiscard]] static http_client_request_content_view bytes(std::span<const std::byte> value) noexcept {
        return bytes(std::string_view(reinterpret_cast<const char*>(value.data()), value.size()));
    }

    template <typename traits_type, typename allocator_type>
    static http_client_request_content_view bytes(std::basic_string<char, traits_type, allocator_type>&&) = delete;

    template <typename traits_type, typename allocator_type>
    static http_client_request_content_view bytes(const std::basic_string<char, traits_type, allocator_type>&&) = delete;

    [[nodiscard]] constexpr const http_client_request_without_content* without_content() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_client_request_without_content>(&content_);
    }
    const http_client_request_without_content* without_content() const&& = delete;

    [[nodiscard]] constexpr const http_client_request_bytes_view* borrowed_bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_client_request_bytes_view>(&content_);
    }
    const http_client_request_bytes_view* borrowed_bytes() const&& = delete;

private:
    using content_type = std::variant<http_client_request_without_content, http_client_request_bytes_view>;

    explicit constexpr http_client_request_content_view(http_client_request_without_content content) noexcept :
        content_(content) {}

    explicit constexpr http_client_request_content_view(http_client_request_bytes_view content) noexcept :
        content_(content) {}

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

    http1_client_exchange_state(http_known_method method, http1_close_policy close_policy, detail::http1_client_initial_content_state content_state, std::string offered_upgrade_protocols) noexcept :
        offered_upgrade_protocols_(std::move(offered_upgrade_protocols)),
        method_(method),
        close_policy_(close_policy),
        content_state_(content_state) {}

    std::string offered_upgrade_protocols_;
    http_known_method method_{http_known_method::unknown};
    http1_close_policy close_policy_{http1_close_policy::allow_reuse};
    detail::http1_client_initial_content_state content_state_{detail::http1_client_initial_content_state::complete};
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
    http1_client_streaming_request_content(std::optional<std::uint64_t> length, bool gated) noexcept :
        length_(length),
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

    explicit constexpr http1_client_immediate_request_content(std::string_view bytes_value) noexcept :
        bytes_(bytes_value) {}

    std::string_view bytes_;
};

class http1_client_continue_gated_request_content final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend struct detail::http1_client_request_prepare_result_access;

    explicit constexpr http1_client_continue_gated_request_content(std::string_view bytes_value) noexcept :
        bytes_(bytes_value) {}

    std::string_view bytes_;
};

// Immutable outbound content contract. Continue-gated content means the writer
// generated Expect: 100-continue and the I/O owner must send the head separately;
// it may release those bytes after 100 Continue or according to its finite wait
// policy. Immediate content includes explicit empty content (Content-Length: 0).
// Payload exists only on the two alternatives that actually send content.
class http1_client_request_content_plan final {
public:
    [[nodiscard]] constexpr const http1_client_request_without_content* without_content() const& noexcept {
        return std::get_if<http1_client_request_without_content>(&content_);
    }
    const http1_client_request_without_content* without_content() const&& = delete;

    [[nodiscard]] constexpr const http1_client_immediate_request_content* immediate() const& noexcept {
        return std::get_if<http1_client_immediate_request_content>(&content_);
    }
    const http1_client_immediate_request_content* immediate() const&& = delete;

    [[nodiscard]] constexpr const http1_client_continue_gated_request_content* continue_gated() const& noexcept {
        return std::get_if<http1_client_continue_gated_request_content>(&content_);
    }
    const http1_client_continue_gated_request_content* continue_gated() const&& = delete;

    [[nodiscard]] constexpr const http1_client_streaming_request_content* streaming() const& noexcept {
        return std::get_if<http1_client_streaming_request_content>(&content_);
    }
    const http1_client_streaming_request_content* streaming() const&& = delete;

private:
    friend struct detail::http1_client_request_prepare_result_access;

    explicit http1_client_request_content_plan(http1_client_streaming_request_content content) noexcept :
        content_(content) {}
    using content_type = std::variant<http1_client_request_without_content, http1_client_immediate_request_content, http1_client_continue_gated_request_content, http1_client_streaming_request_content>;

    explicit constexpr http1_client_request_content_plan(http1_client_request_without_content content) noexcept :
        content_(content) {}

    explicit constexpr http1_client_request_content_plan(http1_client_immediate_request_content content) noexcept :
        content_(content) {}

    explicit constexpr http1_client_request_content_plan(http1_client_continue_gated_request_content content) noexcept :
        content_(content) {}

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

[[nodiscard]] std::string_view http1_client_request_prepare_error_message(http1_client_request_prepare_error error) noexcept;

class http1_client_request_buffer_too_small final {
public:
    [[nodiscard]] constexpr std::size_t required_head_bytes() const noexcept {
        return required_head_bytes_;
    }

private:
    friend struct detail::http1_client_request_prepare_result_access;

    explicit constexpr http1_client_request_buffer_too_small(std::size_t required_head_bytes) noexcept :
        required_head_bytes_(required_head_bytes) {}

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

    prepared_http1_client_request(std::string_view head, http1_client_request_content_plan content_plan, http1_client_exchange_state exchange_state) noexcept :
        head_(head),
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

    [[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> prepare(const http_origin_view& origin, const http_client_request_view& request, std::span<char> head_buffer, http1_client_request_wire_policy policy = {}) const;

    // RFC 9298 HTTP/1.1 GET Upgrade. Capsule-Protocol is validated; Upgrade and
    // Connection are driver-owned, and Host remains owned by the writer.
    // RFC 9931 section 6.3 forbids sending UDP payload before the server's
    // successful protocol-upgrade response in HTTP/1.x.
    [[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> prepare_connect_udp(const http_origin_view& origin, borrowed_text target, std::span<const http_header_view> headers, std::span<char> head_buffer, http1_client_request_wire_policy policy = {}) const;

    [[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> prepare_streaming(const http_origin_view& origin, const http1_client_request_head_view& request, std::span<char> head_buffer, http1_client_request_wire_policy policy = {}) const;

    // When forwarding untrusted TCP payload, RFC 9931 section 8 requires the
    // driver to wait for the successful tunnel plan before sending that payload,
    // unless close_after_response makes the request emit Connection: close.
    [[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> prepare_connect(const http_origin_view& tunnel_origin, std::span<const http_header_view> headers, std::span<char> head_buffer, http1_client_request_wire_policy policy = {}) const;

    [[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> prepare_connect(borrowed_text authority, std::span<const http_header_view> headers, std::span<char> head_buffer, http1_client_request_wire_policy policy = {}) const;
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
    [[nodiscard]] std::variant<std::string_view, http1_request_content_write_error> plan_finish(std::span<char> buffer, std::span<const http_header_view> trailers = {}) noexcept;
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
