#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "http1_connection_plan.h"
#include "http1_server_request.h"
#include "http_header.h"
#include "http_message.h"
#include "http_response.h"
#include "http_response_stream.h"
#include "http_status.h"

namespace ruvia {

// An HTTP/1 response head this endpoint cannot send: an interim, final,
// CONNECT or CONNECT-UDP head that breaks a message rule (RFC 9110, RFC 9112)
// or the head size bound. It reports an application or runtime bug, never a
// peer error, so a server answers it like any other handler failure.
class http1_response_head_error final : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

inline constexpr std::size_t response_head_stack_bytes = 512;
inline constexpr std::size_t response_head_retained_heap_bytes = std::size_t{4} * 1024;

// Reusable scratch storage for serialized HTTP response heads. Keeps small heads
// inline and retains only bounded heap capacity across requests.
class http_response_head_buffer final {
    struct stack_state_type final {
        std::size_t used_{0};
    };
    struct heap_state_type final {};

public:
    http_response_head_buffer() noexcept = default;

    http_response_head_buffer(const http_response_head_buffer& other);
    http_response_head_buffer(http_response_head_buffer&& other) noexcept;
    http_response_head_buffer& operator=(const http_response_head_buffer& other);
    http_response_head_buffer& operator=(http_response_head_buffer&& other) noexcept;
    void reset() noexcept;
    // value may alias storage, including a written range returned by stack_cursor().
    void append(std::string_view value);
    void append(char value);
    void append_unsigned(std::uint64_t value);
    // Reserve beyond the current size without spilling a fitting inline range.
    void reserve_additional(std::size_t size);
    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view view() const&& = delete;
    [[nodiscard]] bool can_append_on_stack(std::size_t size) const noexcept;

    template <typename writer_type>
        requires std::is_nothrow_invocable_r_v<void, writer_type&, char*>
    void append_generated(std::size_t size, writer_type&& writer) {
        if (char* cursor_value = stack_cursor(size); cursor_value != nullptr) {
            writer(cursor_value);
            commit_stack(cursor_value + size);
            return;
        }
        reserve_additional(size);
        const auto previous_size = heap_.size();
        heap_.resize(previous_size + size);
        writer(heap_.data() + previous_size);
    }

    // Returns uncommitted inline scratch at the current end if bound fits.
    // This is a capacity check, not a reservation; the buffer must stay alive
    // and unmoved, and intervening mutations may change the logical end.
    [[nodiscard]] char* stack_cursor(std::size_t bound) & noexcept RUVIA_LIFETIMEBOUND {
        auto* const stack_state = std::get_if<stack_state_type>(&state_);
        if (stack_state == nullptr || bound > stack_.size() - stack_state->used_) {
            return nullptr;
        }
        return stack_.data() + stack_state->used_;
    }
    [[nodiscard]] char* stack_cursor(std::size_t) && = delete;

    // Sets the inline length to end, including the array's one-past endpoint.
    // This may shorten existing content. A spilled buffer throws logic_error;
    // a null or foreign endpoint throws invalid_argument. Rejection preserves
    // the committed content. No pointer subtraction is performed on a foreign
    // array, even when an endpoint was taken from another live buffer.
    void commit_stack(const char* end) {
        auto* const stack_state = std::get_if<stack_state_type>(&state_);
        if (stack_state == nullptr) {
            throw std::logic_error("HTTP response head no longer uses inline storage");
        }
        const auto begin_address = reinterpret_cast<std::uintptr_t>(stack_.data());
        const auto end_address = reinterpret_cast<std::uintptr_t>(end);
        const auto offset = end_address - begin_address;
        if (end == nullptr || end_address < begin_address || offset > stack_.size()) {
            throw std::invalid_argument("HTTP response head endpoint is outside its inline storage");
        }
        stack_state->used_ = static_cast<std::size_t>(offset);
    }

private:
    void spill_to_heap(std::size_t min_capacity);
    void copy_committed_state(const http_response_head_buffer& other) noexcept;
    std::array<char, response_head_stack_bytes> stack_{};
    std::string heap_;
    std::variant<stack_state_type, heap_state_type> state_{stack_state_type{}};
};

// Who measures the length a response head declares: the response writer, from
// a complete buffered representation, or the response's own fields for a
// stream whose content follows the head.
enum class http1_response_content_source : std::uint8_t {
    buffered,
    streamed,
};

// Final HTTP/1 head framing is one exclusive protocol value. The body plan
// keeps method/status content semantics attached to the exact wire framing
// selected by the HTTP/1 planner:
// - known_length from a buffered source: the response writer length-delimits
//   the complete representation with a canonical Content-Length.
// - known_length from a streamed source: the response's own Content-Length
//   delimits the stream (RFC 9112 §6.3); the runtime writes raw body bytes,
//   rejects a write past the declared length and closes the connection when
//   the stream ends short of it (RFC 9112 §8).
// - chunked: the response writer owns canonical Transfer-Encoding: chunked and
//   the runtime emits the matching chunk frames. Application headers cannot
//   redefine framing.
// - close_delimited: the response has no declared message-body length. When
//   content is allowed, the runtime closes the connection to delimit it;
//   application Content-Length and Transfer-Encoding fields therefore cannot
//   survive into the wire head.
class http1_response_head_plan final {
public:
    [[nodiscard]] constexpr const http1_message_framing& framing() const& noexcept RUVIA_LIFETIMEBOUND {
        return framing_;
    }
    const http1_message_framing& framing() const&& = delete;

    [[nodiscard]] constexpr http1_response_content_source content_source() const noexcept {
        return content_source_;
    }

    [[nodiscard]] constexpr http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    [[nodiscard]] constexpr http_protocol_version protocol_version() const noexcept {
        return protocol_version_;
    }

    // Whether a buffered response writes payload bytes after this head: its
    // method/status permit a body and the representation is non-empty.
    [[nodiscard]] constexpr bool send_body() const noexcept {
        return detail::http_buffered_response_sends_body(body_plan_, framing_.content_length());
    }

private:
    friend http1_response_head_plan plan_http1_buffered_response_head(http_buffered_response_write_plan, http1_request_connection_plan) noexcept;
    friend constexpr http1_response_head_plan plan_http1_response_stream_head(http_response_body_plan, http1_request_connection_plan, http1_message_framing) noexcept;

    constexpr http1_response_head_plan(http_response_body_plan body_plan, http_protocol_version protocol_version, http1_message_framing framing, http1_response_content_source content_source) noexcept :
        body_plan_(body_plan),
        framing_(framing),
        protocol_version_(protocol_version),
        content_source_(content_source) {}

    http_response_body_plan body_plan_;
    http1_message_framing framing_;
    http_protocol_version protocol_version_;
    http1_response_content_source content_source_;
};

// The head of a complete buffered response, length-delimited by `write_plan`.
[[nodiscard]] inline http1_response_head_plan plan_http1_buffered_response_head(http_buffered_response_write_plan write_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_response_head_plan(write_plan.body_plan(), connection_plan.protocol_version(), http1_message_framing::known_length(write_plan.content_length()), http1_response_content_source::buffered);
}

// The head of a response stream delimited by `framing`: chunked,
// close_delimited, or the known_length the response itself declares.
[[nodiscard]] constexpr http1_response_head_plan plan_http1_response_stream_head(http_response_body_plan body_plan, http1_request_connection_plan connection_plan, http1_message_framing framing) noexcept {
    return http1_response_head_plan(body_plan, connection_plan.protocol_version(), framing, http1_response_content_source::streamed);
}

// Serialize a finalized HTTP/1 response head into caller-owned reusable scratch.
// The plan must describe the same response representation.
// Canonical Content-Length is measured and emitted from the same decimal token.
// An explicit Date is preserved without consulting the clock; otherwise a
// cached current Date is generated when a usable clock is available. A head
// that breaks a message rule or the head size bound throws
// http1_response_head_error. The plan owns framing: an application
// Transfer-Encoding is accepted only as exactly `chunked` (OWS and ASCII case
// aside) and is replaced by the plan's framing; any other value throws.
void append_http1_response_head(const http_response& response, http_response_head_buffer& head, const http1_response_head_plan& plan);

// Append only field lines; chunk delimiters belong to the caller. Inputs may
// alias output. Allocation failure leaves the existing output bytes unchanged.
void append_http1_response_trailers(std::string& output, const http_response_trailer_section& trailers);

// Connection lifecycle after the encoded interim response. RFC 9112 requires
// a sender of Connection: close to begin closing after the response containing
// that option, even though doing so leaves the request without a final response.
enum class http1_interim_connection_disposition : std::uint8_t {
    unchanged,
    close_after_interim_response,
};

class http1_interim_response_buffer_too_small final {
public:
    [[nodiscard]] constexpr std::size_t required_head_bytes() const noexcept {
        return required_head_bytes_;
    }

private:
    friend std::variant<class prepared_http1_interim_response, http1_interim_response_buffer_too_small> prepare_http1_interim_response_head(const http_interim_response_head&, std::span<char> head_buffer RUVIA_LIFETIMEBOUND);

    explicit constexpr http1_interim_response_buffer_too_small(std::size_t required_head_bytes) noexcept :
        required_head_bytes_(required_head_bytes) {}

    std::size_t required_head_bytes_;
};

// A transactionally encoded HTTP/1.1 interim head. The byte view points into
// the caller's output buffer. When Connection: close is present, the owner must
// initiate connection closure as soon as this interim head has been written.
class prepared_http1_interim_response final {
public:
    [[nodiscard]] constexpr std::string_view head() const noexcept {
        return head_;
    }

    [[nodiscard]] constexpr http1_interim_connection_disposition connection_disposition() const noexcept {
        return connection_disposition_;
    }

private:
    friend std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small> prepare_http1_interim_response_head(const http_interim_response_head&, std::span<char> head_buffer RUVIA_LIFETIMEBOUND);

    constexpr prepared_http1_interim_response(std::string_view head, http1_interim_connection_disposition connection_disposition) noexcept :
        head_(head),
        connection_disposition_(connection_disposition) {}

    std::string_view head_;
    http1_interim_connection_disposition connection_disposition_;
};

// Allocation-free HTTP/1.1 interim-head writer. It validates the complete
// borrowed field set against max_http_header_fields/max_http_header_bytes
// before touching output, throwing http1_response_head_error for an unsendable head.
// No Server or Date field is injected: the encoded fields are exactly those
// represented by http_interim_response_head.
[[nodiscard]] std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small> prepare_http1_interim_response_head(const http_interim_response_head& response, std::span<char> head_buffer RUVIA_LIFETIMEBOUND);

class http1_response_stream_plan final {
public:
    // The framing a body-carrying stream uses unless the response declares its
    // own length at head commit: chunked for HTTP/1.1, close_delimited for
    // HTTP/1.0.
    [[nodiscard]] http1_framing_kind framing() const noexcept {
        return framing_;
    }

    [[nodiscard]] http1_request_connection_plan request_connection_plan() const noexcept {
        return request_connection_plan_;
    }

    [[nodiscard]] http1_close_policy close_policy() const noexcept {
        return close_policy_;
    }

    [[nodiscard]] http_known_method request_method() const noexcept {
        return request_method_;
    }

private:
    friend http1_response_stream_plan plan_http1_response_stream(const http1_server_request_parser&, http1_close_policy, http1_request_body_consumption) noexcept;

    http1_response_stream_plan(http1_framing_kind framing, http1_request_connection_plan request_connection_plan, http1_close_policy close_policy, http_known_method request_method) noexcept :
        framing_(framing),
        request_connection_plan_(request_connection_plan),
        close_policy_(close_policy),
        request_method_(request_method) {}

    http1_framing_kind framing_;
    http1_request_connection_plan request_connection_plan_;
    http1_close_policy close_policy_{http1_close_policy::close_after_response};
    http_known_method request_method_{http_known_method::unknown};
};

// Pure HTTP/1 response-stream planning. The runtime contributes only its typed
// product policy (for example, a per-connection request limit) and the request
// body's consumption state; HTTP retains the request close policy and candidate
// framing until the response status is known. Commit can therefore distinguish
// a body-allowed HTTP/1.0 stream, which requires close delimiting, from a
// body-suppressed response that is already self-delimited. A route that consumed
// the complete request body before streaming passes `complete`; the parser's
// body plan still describes the original request framing, so planning for an
// unread body passes `body_plan_.consumption_if_unread()`.
[[nodiscard]] inline http1_response_stream_plan plan_http1_response_stream(const http1_server_request_parser& parsed_value, http1_close_policy close_policy, http1_request_body_consumption consumption) noexcept {
    const auto framing = parsed_value.request().protocol_version() == http_protocol_version::http11 ? http1_framing_kind::chunked : http1_framing_kind::close_delimited;
    return http1_response_stream_plan(framing, apply_request_body_consumption(parsed_value.connection_plan(), consumption), close_policy, parsed_value.request().known_method());
}

enum class http1_connection_close_field_policy : std::uint8_t { close_only,
    preserve_upgrade };

inline void http1_mark_connection_close(http_response& response, http1_connection_close_field_policy field_policy = http1_connection_close_field_policy::close_only) {
    // A runtime close verdict dominates keep-alive. Collapse repeated fields
    // after the socket lifecycle is decided, while preserving the Upgrade
    // option required by any retained Upgrade field and any custom hop-by-hop
    // field names declared in Connection (RFC 9110 section 7.6.1).
    const std::string_view base = field_policy == http1_connection_close_field_policy::preserve_upgrade ? "close, Upgrade" : "close";
    std::string merged;
    for (const auto& header : response.headers()) {
        if (!http_ascii_equals_ignore_case(header.name(), "Connection")) {
            continue;
        }
        detail::http_visit_comma_separated_quoted(header.value(), [&merged, base](std::string_view option) {
            if (!http_ascii_equals_ignore_case(option, "close") && !http_ascii_equals_ignore_case(option, "keep-alive") && !http_ascii_equals_ignore_case(option, "Upgrade")) {
                if (merged.empty()) {
                    merged.assign(base);
                }
                merged.append(", ");
                merged.append(option);
            }
            return true;
        });
    }
    if (merged.empty()) {
        response.header("Connection", base);
    } else {
        response.header("Connection", std::move(merged));
    }
}

class prepared_http1_response_stream;

// Commit response-side HTTP/1 persistence after the runtime has folded in
// request-body completion and server policy. This is the sole protocol mutation:
// it honors an application-provided Connection: close and emits the version-
// appropriate Connection field. Success retains the exact request version for
// head serialization; a head this endpoint cannot send (for example a 426
// without an Upgrade protocol, or a Transfer-Encoding other than exactly
// `chunked`) throws http1_response_head_error before the response changes.
[[nodiscard]] http1_request_connection_plan http1_commit_final_response(http_response& response, http1_request_connection_plan plan);

// Commit-time result. This object combines request/version/runtime constraints with
// the response method/status and Connection options, then binds the exact header
// bytes to the authoritative connection plan. The response is normalized to
// the commit plan as prepare_http_response_stream_head() leaves it.
class prepared_http1_response_stream final {
public:
    [[nodiscard]] http_response& response() & noexcept RUVIA_LIFETIMEBOUND {
        return response_;
    }
    [[nodiscard]] http_response& response() && = delete;

    [[nodiscard]] const http_response& response() const& noexcept RUVIA_LIFETIMEBOUND {
        return response_;
    }
    [[nodiscard]] const http_response& response() const&& = delete;

    [[nodiscard]] const http1_response_head_plan& response_head_plan() const& noexcept RUVIA_LIFETIMEBOUND {
        return response_head_plan_;
    }
    [[nodiscard]] const http1_response_head_plan& response_head_plan() const&& = delete;

    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const& noexcept RUVIA_LIFETIMEBOUND {
        return commit_plan_;
    }
    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const&& = delete;

    [[nodiscard]] http1_request_connection_plan connection_plan() const noexcept {
        return connection_plan_;
    }

private:
    friend prepared_http1_response_stream prepare_http1_response_stream_head(http_response, const http1_response_stream_plan&, http_response_trailer_intent);

    prepared_http1_response_stream(http_response response, http_response_stream_commit_plan commit_plan, http1_response_head_plan response_head_plan, http1_request_connection_plan connection_plan) noexcept :
        response_(std::move(response)),
        commit_plan_(commit_plan),
        response_head_plan_(response_head_plan),
        connection_plan_(connection_plan) {}

    http_response response_;
    http_response_stream_commit_plan commit_plan_;
    http1_response_head_plan response_head_plan_;
    http1_request_connection_plan connection_plan_;
};

// The response's own valid Content-Length, without a Transfer-Encoding hint
// (which overrides it, RFC 9112 §6.3), selects known_length framing for a
// body-carrying response on either HTTP/1 version; otherwise the plan's chunked
// or close-delimited framing applies and Content-Length is dropped.
[[nodiscard]] prepared_http1_response_stream prepare_http1_response_stream_head(http_response response, const http1_response_stream_plan& plan, http_response_trailer_intent trailer_intent = http_response_trailer_intent::none);

// The successful head ends HTTP framing and transfers the connection to a
// duplex byte stream. Payload belongs to that stream, not to an HTTP response.
// A 2xx head with content or framing fields throws http1_response_head_error.
[[nodiscard]] http1_response_head_plan prepare_http1_connect_response_head(const http_response& response, http_protocol_version version);

// RFC 9110 §7.8: whether an HTTP/1.1 `request` asks to switch protocols -- an
// Upgrade field offering at least one protocol, sent with the Upgrade
// connection option. Any other request's Upgrade field is ignored, as is a
// CONNECT's: its 2xx establishes a tunnel instead.
[[nodiscard]] bool http1_request_offers_upgrade(const http_request& request) noexcept;

// Makes `response` -- a head without content or framing fields -- the 101
// (Switching Protocols) head after which an HTTP/1.1 connection carries the
// protocol it names (RFC 9110 §7.8, §15.2.2): the response's own Upgrade
// field, else the first protocol `request` offered, with the Upgrade
// connection option added when missing. A request that offers no upgrade, a
// protocol it did not offer, content or framing fields throw
// http1_response_head_error before `response` changes.
[[nodiscard]] http1_response_head_plan prepare_http1_upgrade_response_head(http_response& response, const http_request& request);

}  // namespace ruvia
