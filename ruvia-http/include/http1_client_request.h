#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "http1_connection_plan.h"
#include "http_authority.h"
#include "http_header.h"
#include "http_message.h"
#include "http_request_content_plan.h"
#include "http_request_head_view.h"
#include "ip_address.h"

namespace ruvia {

// Outbound HTTP client protocol models.
//
// OWNERSHIP: these are transport-free HTTP values. http_origin_view and
// http_request_head_view borrow their string storage; http_client_response_head owns
// parsed header storage. Response content remains owned by the
// external sans-I/O driver that follows the framing plan. Socket/TLS
// configuration, pools, redirect limits, and timeouts also belong there.

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
// CONNECT always uses authority-form.
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

// The HTTP/1-only wire choices of request preparation. The request's
// expectation and field source travel in its http_request_head_view. How long
// an I/O runtime waits before releasing a continue-gated body remains runtime
// policy, not an HTTP message-model setting.
struct http1_client_request_wire_policy final {
    http1_close_policy close_policy_{http1_close_policy::allow_reuse};
    http1_request_target_form target_form_{http1_request_target_form::origin};
};

enum class http1_client_request_prepare_error : std::uint8_t {
    invalid_method,
    invalid_target,
    // A CONNECT whose authority_ is absent or is not host:port with a nonzero
    // port (RFC 9110 section 9.3.6).
    invalid_connect_authority,
    // protocol_ names a request HTTP/1.1 cannot carry: Extended CONNECT, an
    // Upgrade other than connect-udp, or a protocol on another method.
    unsupported_protocol,
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
    // Content will follow on TRACE, or any content plan on CONNECT or on a
    // protocol upgrade request.
    content_forbidden_for_method,
    options_content_type_required,
    header_too_large,
    invalid_close_policy,
    invalid_expectation,
    // An unknown target form, or absolute-form on CONNECT.
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
// caller-provided output buffer. `content()` is none, known length with the
// request's borrowed bytes (explicit empty content included), or streaming
// (chunked when no length); borrowed bytes must remain alive and unchanged
// until sent. continue_gated() means the writer generated Expect:
// 100-continue: the I/O owner sends the head separately and may release the
// content after 100 Continue or according to its finite wait policy.
// take_exchange_state() moves out the independent response-side protocol facts;
// head() and content() remain readable for the request write or an explicit retry.
class prepared_http1_client_request final {
public:
    [[nodiscard]] constexpr std::string_view head() const& noexcept {
        return head_;
    }
    [[nodiscard]] constexpr std::string_view head() const&& = delete;

    [[nodiscard]] constexpr const http_request_content_plan& content() const& noexcept {
        return content_;
    }
    [[nodiscard]] constexpr const http_request_content_plan& content() const&& = delete;

    [[nodiscard]] constexpr bool continue_gated() const noexcept {
        return continue_gated_;
    }

    [[nodiscard]] http1_client_exchange_state take_exchange_state() && noexcept {
        return std::move(exchange_state_);
    }

private:
    friend struct detail::http1_client_request_prepare_result_access;

    prepared_http1_client_request(std::string_view head, http_request_content_plan content, bool continue_gated, http1_client_exchange_state exchange_state) noexcept :
        head_(head),
        content_(content),
        exchange_state_(std::move(exchange_state)),
        continue_gated_(continue_gated) {}

    std::string_view head_;
    http_request_content_plan content_;
    http1_client_exchange_state exchange_state_;
    bool continue_gated_;
};

// The one outcome of preparing an HTTP/1 request head.
using http1_client_request_prepare_result = std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error>;

// Prepares one HTTP/1.1 request head for a direct origin or, in absolute-form,
// a forward proxy. Ordinary requests are allocation-free;
// an Upgrade request owns only its offered protocol value for later 101
// validation. It validates the complete request before touching the caller's
// buffer, generates Host and exact Content-Length, and returns separate
// head/content views for writev-style I/O or Expect: 100-continue gating.
// Expect is writer-owned (request.expectation_) so the emitted field and the
// resulting content gate cannot disagree.
// Generated port and Content-Length tokens are shared by measurement and writing.
// Metadata text and the header descriptor array may overlap head_buffer:
// successful preparation consumes them before overwriting the emitted head.
// Borrowed content must not overlap those written bytes; it may use the unused
// tail of head_buffer and remains unchanged until the runtime sends it.
// Explicit empty content emits Content-Length: 0 without triggering TRACE's
// content prohibition or generated OPTIONS' Content-Type requirement.
// The head's kind() selects the request line:
// - regular: `origin` supplies the scheme and authority (Host and, in
//   absolute-form, the target prefix); request.scheme_ and
//   request.authority_ are not read.
// - connect: authority-form to request.authority_, which needs a nonzero
//   port; `origin` is not written (RFC 9112 section 3.2.3). When forwarding
//   untrusted TCP payload, RFC 9931 section 8 requires the driver to wait for
//   the successful tunnel plan before sending that payload, unless
//   close_after_response makes the request emit Connection: close.
// - protocol_upgrade with protocol_ connect-udp: the RFC 9298 section 3.2
//   GET Upgrade to `origin`. Capsule-Protocol is validated; Upgrade and
//   Connection are writer-owned. RFC 9931 section 6.3 forbids sending UDP
//   payload before the server's successful protocol-upgrade response.
// - extended_connect has no HTTP/1.1 form and is unsupported_protocol.
[[nodiscard]] http1_client_request_prepare_result prepare_http1_client_request(const http_origin_view& origin, const http_request_head_view& request, std::span<char> head_buffer, http1_client_request_wire_policy policy = {});

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

// Bound to a successfully prepared streaming request's immutable framing facts
// (precondition: prepared.content().streaming_content() != nullptr).
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
    explicit http1_request_content_writer(const prepared_http1_client_request& prepared) noexcept;
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
