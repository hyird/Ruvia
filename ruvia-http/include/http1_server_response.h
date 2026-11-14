#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "http1_chunked_body.h"
#include "http1_server_request.h"
#include "http_header.h"
#include "http_message.h"
#include "http_request.h"
#include "http_response.h"
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

class http1_response_head_plan;
class http1_buffered_response_plan;

// A complete buffered representation is length-delimited by the response writer.
class http1_buffered_response_head final {
public:
    [[nodiscard]] constexpr std::uint64_t content_length() const noexcept {
        return content_length_;
    }

private:
    friend class http1_response_head_plan;

    explicit constexpr http1_buffered_response_head(std::uint64_t content_length) noexcept :
        content_length_(content_length) {}

    std::uint64_t content_length_{0};
};

// The response writer owns canonical Transfer-Encoding: chunked and the runtime
// emits the matching chunk frames. Application headers cannot redefine framing.
class http1_chunked_response_stream_head final {
private:
    friend class http1_response_head_plan;

    constexpr http1_chunked_response_stream_head() noexcept = default;
};

// The response has no declared message-body length. When content is allowed, the
// runtime closes the connection to delimit it; application Content-Length and
// Transfer-Encoding fields therefore cannot survive into the wire head.
class http1_close_delimited_response_stream_head final {
private:
    friend class http1_response_head_plan;

    constexpr http1_close_delimited_response_stream_head() noexcept = default;
};

// The response's own Content-Length delimits the stream (RFC 9112 §6.3): the
// runtime writes raw body bytes, rejects a write past the declared length and
// closes the connection when the stream ends short of it (RFC 9112 §8).
class http1_length_delimited_response_stream_head final {
public:
    [[nodiscard]] constexpr std::uint64_t content_length() const noexcept {
        return content_length_;
    }

private:
    friend class http1_response_head_plan;

    explicit constexpr http1_length_delimited_response_stream_head(std::uint64_t content_length) noexcept :
        content_length_(content_length) {}

    std::uint64_t content_length_{0};
};

// Final HTTP/1 head framing is one exclusive protocol value. The body plan
// keeps method/status content semantics attached to the exact wire-framing
// alternative selected by the HTTP/1 planner.
class http1_response_head_plan final {
public:
    [[nodiscard]] constexpr const http1_buffered_response_head* buffered() const& noexcept {
        return std::get_if<http1_buffered_response_head>(&framing_);
    }
    [[nodiscard]] constexpr const http1_buffered_response_head* buffered() const&& = delete;

    [[nodiscard]] constexpr const http1_chunked_response_stream_head* chunked_stream() const& noexcept {
        return std::get_if<http1_chunked_response_stream_head>(&framing_);
    }
    [[nodiscard]] constexpr const http1_chunked_response_stream_head* chunked_stream() const&& = delete;

    [[nodiscard]] constexpr const http1_close_delimited_response_stream_head* close_delimited_stream() const& noexcept {
        return std::get_if<http1_close_delimited_response_stream_head>(&framing_);
    }
    [[nodiscard]] constexpr const http1_close_delimited_response_stream_head* close_delimited_stream() const&& = delete;

    [[nodiscard]] constexpr const http1_length_delimited_response_stream_head* length_delimited_stream() const& noexcept {
        return std::get_if<http1_length_delimited_response_stream_head>(&framing_);
    }
    [[nodiscard]] constexpr const http1_length_delimited_response_stream_head* length_delimited_stream() const&& = delete;

    [[nodiscard]] constexpr http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    [[nodiscard]] constexpr http_protocol_version protocol_version() const noexcept {
        return protocol_version_;
    }

private:
    friend http1_buffered_response_plan get_http1_buffered_response_plan(http_buffered_response_write_plan, http1_request_connection_plan) noexcept;
    friend constexpr http1_response_head_plan http1_chunked_response_stream_head_plan(http_response_body_plan, http1_request_connection_plan) noexcept;
    friend constexpr http1_response_head_plan http1_close_delimited_response_stream_head_plan(http_response_body_plan, http1_request_connection_plan) noexcept;
    friend constexpr http1_response_head_plan http1_length_delimited_response_stream_head_plan(http_response_body_plan, http1_request_connection_plan, std::uint64_t) noexcept;

    using framing_type = std::variant<http1_buffered_response_head, http1_chunked_response_stream_head, http1_close_delimited_response_stream_head, http1_length_delimited_response_stream_head>;

    [[nodiscard]] static constexpr framing_type buffered_framing(std::uint64_t content_length) noexcept {
        return framing_type(http1_buffered_response_head(content_length));
    }

    [[nodiscard]] static constexpr framing_type chunked_stream_framing() noexcept {
        return framing_type(http1_chunked_response_stream_head());
    }

    [[nodiscard]] static constexpr framing_type close_delimited_stream_framing() noexcept {
        return framing_type(http1_close_delimited_response_stream_head());
    }

    [[nodiscard]] static constexpr framing_type length_delimited_stream_framing(std::uint64_t content_length) noexcept {
        return framing_type(http1_length_delimited_response_stream_head(content_length));
    }

    constexpr http1_response_head_plan(http_response_body_plan body_plan, http_protocol_version protocol_version, framing_type framing) noexcept :
        body_plan_(body_plan),
        protocol_version_(protocol_version),
        framing_(framing) {}

    http_response_body_plan body_plan_;
    http_protocol_version protocol_version_;
    framing_type framing_;
};

[[nodiscard]] constexpr http1_response_head_plan http1_chunked_response_stream_head_plan(http_response_body_plan body_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_response_head_plan(body_plan, connection_plan.protocol_version(), http1_response_head_plan::chunked_stream_framing());
}

[[nodiscard]] constexpr http1_response_head_plan http1_close_delimited_response_stream_head_plan(http_response_body_plan body_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_response_head_plan(body_plan, connection_plan.protocol_version(), http1_response_head_plan::close_delimited_stream_framing());
}

[[nodiscard]] constexpr http1_response_head_plan http1_length_delimited_response_stream_head_plan(http_response_body_plan body_plan, http1_request_connection_plan connection_plan, std::uint64_t content_length) noexcept {
    return http1_response_head_plan(body_plan, connection_plan.protocol_version(), http1_response_head_plan::length_delimited_stream_framing(content_length));
}

// The runtime writes one inseparable HTTP/1 buffered response contract. The
// embedded head owns method/status/body semantics and representation length
// exactly once; direct fact access avoids restoring a parallel write-plan copy.
class http1_buffered_response_plan final {
public:
    [[nodiscard]] constexpr http_response_body_plan body_plan() const noexcept {
        return head_plan_.body_plan();
    }

    [[nodiscard]] http_status_code response_status() const noexcept {
        return body_plan().response_status();
    }

    [[nodiscard]] constexpr std::uint64_t content_length() const noexcept {
        return head_plan_.buffered()->content_length();
    }

    [[nodiscard]] bool send_body() const noexcept {
        return detail::http_buffered_response_sends_body(body_plan(), content_length());
    }

    [[nodiscard]] constexpr const http1_response_head_plan& head_plan() const& noexcept {
        return head_plan_;
    }
    [[nodiscard]] constexpr const http1_response_head_plan& head_plan() const&& = delete;

private:
    friend http1_buffered_response_plan get_http1_buffered_response_plan(http_buffered_response_write_plan, http1_request_connection_plan) noexcept;

    explicit constexpr http1_buffered_response_plan(http1_response_head_plan head_plan) noexcept :
        head_plan_(head_plan) {}

    http1_response_head_plan head_plan_;
};

static_assert(std::is_trivially_copyable_v<http1_buffered_response_plan>);
static_assert(sizeof(http1_buffered_response_plan) == sizeof(http1_response_head_plan));

[[nodiscard]] inline http1_buffered_response_plan get_http1_buffered_response_plan(http_buffered_response_write_plan write_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_buffered_response_plan(http1_response_head_plan(write_plan.body_plan(), connection_plan.protocol_version(), http1_response_head_plan::buffered_framing(write_plan.content_length())));
}

class http_response_trailer_section;

// Borrowed proof that the complete terminal section passed the shared response-
// trailer rules. The source span must outlive its synchronous consumption.
class http_response_trailer_section final {
public:
    [[nodiscard]] std::span<const http_header_view> fields() const noexcept {
        return fields_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return fields_.empty();
    }

private:
    friend http_response_trailer_section validate_http_response_trailers(std::span<const http_header_view>);
    explicit http_response_trailer_section(std::span<const http_header_view> fields_value) noexcept :
        fields_(fields_value) {}

    std::span<const http_header_view> fields_;
};

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

// Validate a complete response trailer section and return a borrowed proof for
// synchronous protocol submission. The input storage must outlive its use.
[[nodiscard]] http_response_trailer_section validate_http_response_trailers(std::span<const http_header_view> trailers);

template <detail::http_temporary_owning_range<http_header_view> headers_type>
http_response_trailer_section validate_http_response_trailers(headers_type&&) = delete;
http_response_trailer_section validate_http_response_trailers(std::initializer_list<http_header_view>) = delete;

// Immutable, bodyless response-head view for 1xx progress messages that precede
// a final response. 101 is deliberately excluded: switching protocols transfers
// connection ownership and therefore requires its dedicated protocol driver.
// Header elements and their strings are borrowed and must remain stable through
// the synchronous sans-I/O submit call.
class http_interim_response_head final {
public:
    explicit http_interim_response_head(http_status_code status_code, borrowed_http_headers headers = {});

    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_code_;
    }

    [[nodiscard]] constexpr std::span<const http_header_view> headers() const noexcept {
        return headers_;
    }

private:
    http_status_code status_code_;
    std::span<const http_header_view> headers_;
};

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
    friend std::variant<class prepared_http1_interim_response, http1_interim_response_buffer_too_small> prepare_http1_interim_response_head(const http_interim_response_head&, std::span<char>);

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
    friend std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small> prepare_http1_interim_response_head(const http_interim_response_head&, std::span<char>);

    constexpr prepared_http1_interim_response(std::string_view head, http1_interim_connection_disposition connection_disposition) noexcept :
        head_(head),
        connection_disposition_(connection_disposition) {}

    std::string_view head_;
    http1_interim_connection_disposition connection_disposition_;
};

// Allocation-free HTTP/1.1 interim-head writer. It validates the complete
// borrowed field set and exact 64-field/64-KiB bounds before touching the
// output buffer, throwing http1_response_head_error for a head it cannot send.
// No Server or Date field is injected: the encoded fields are exactly those
// represented by http_interim_response_head.
[[nodiscard]] std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small> prepare_http1_interim_response_head(const http_interim_response_head& response, std::span<char> head_buffer);

enum class http_response_stream_framing : std::uint8_t {
    http1_chunked,
    // RFC 9112 6.1: a server MUST NOT send Transfer-Encoding to a client that did
    // not indicate HTTP/1.1. An HTTP/1.0 stream therefore carries no chunk framing;
    // the body is delimited by the connection close (RFC 9112 6.3), so it also
    // announces Connection: close and the session shuts the socket afterwards.
    http1_close_delimited,
    // RFC 9112 6.3: a valid Content-Length the response itself declares, with
    // no Transfer-Encoding, delimits the stream on HTTP/1.1 and HTTP/1.0 alike,
    // so the connection stays reusable. Selected at head commit, never planned.
    http1_length_delimited,
    http2_frames,
    // RFC 9114 §4.1: HTTP/3 delimits content with DATA frames and carries a
    // trailer section in a trailing HEADERS frame, exactly like HTTP/2.
    http3_frames
};
// A trailer section is terminal message metadata, not an independently queued
// side channel. The caller declares whether this head commit is reserving a
// terminal trailer section. Trailers are optional metadata a recipient may
// discard (RFC 9110 6.5), so framing that cannot carry them (HTTP/1.0
// close-delimited, known length, or a suppressed body) drops them; only a
// status that can never carry content rejects the intent before the head.
enum class http_response_trailer_intent : std::uint8_t { none,
    present };

// Whether a validated trailer section commits the response to sending trailers.
// An empty section is not "no decision": it is the decision to send none, which
// the head must state before any body byte goes out.
[[nodiscard]] inline http_response_trailer_intent response_trailer_intent(const http_response_trailer_section& section) noexcept {
    return section.empty() ? http_response_trailer_intent::none : http_response_trailer_intent::present;
}

// Authoritative phase immediately after the initial response head is submitted.
// Body-suppressed responses (HEAD / 304) complete at the initial head
// (message_ended) and discard trailers.
enum class http_response_stream_head_disposition : std::uint8_t { body_open,
    message_ended };

class http_response_stream_commit_plan final {
public:
    [[nodiscard]] http_status_code response_status() const noexcept {
        return body_plan_.response_status();
    }

    [[nodiscard]] http_response_stream_framing framing() const noexcept {
        return framing_;
    }

    [[nodiscard]] http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    // HTTP/1.1 requires chunked framing for trailers (RFC 9112 7.1.2).
    // HTTP/2 and HTTP/3 carry them in a trailing field section. A body-suppressed
    // response (HEAD / 304) ends at its initial head and discards trailers.
    [[nodiscard]] bool trailers_available() const noexcept {
        return !body_plan_.body_suppressed() && (framing_ == http_response_stream_framing::http1_chunked || framing_ == http_response_stream_framing::http2_frames || framing_ == http_response_stream_framing::http3_frames);
    }

    [[nodiscard]] http_response_stream_head_disposition head_disposition() const noexcept {
        return body_plan_.body_suppressed() ? http_response_stream_head_disposition::message_ended : http_response_stream_head_disposition::body_open;
    }

    // A status that can never carry content (1xx/204/205/304) has no message
    // body to terminate with a trailer section; asking for one is a handler
    // error. Every other unavailable framing discards the trailers instead.
    [[nodiscard]] bool status_permits_trailers() const noexcept {
        return body_plan_.status_allows_body();
    }

    [[nodiscard]] bool trailer_intent_allowed() const noexcept {
        return !trailer_intent_present_ || status_permits_trailers();
    }

private:
    friend http_response_stream_commit_plan plan_http_response_stream_commit(http_response_stream_framing, http_known_method, http_status_code, http_response_trailer_intent) noexcept;

    http_response_stream_commit_plan(http_response_stream_framing framing, http_response_body_plan body_plan, bool trailer_intent_present) noexcept :
        framing_(framing),
        body_plan_(body_plan),
        trailer_intent_present_(trailer_intent_present) {}

    http_response_stream_framing framing_{http_response_stream_framing::http1_chunked};
    http_response_body_plan body_plan_;
    bool trailer_intent_present_{false};
};

[[nodiscard]] inline http_response_stream_commit_plan plan_http_response_stream_commit(http_response_stream_framing framing, http_known_method request_method, http_status_code response_status, http_response_trailer_intent trailer_intent) noexcept {
    const auto body_plan = plan_http_response_body(request_method, response_status);
    return http_response_stream_commit_plan(framing, body_plan, trailer_intent == http_response_trailer_intent::present);
}

// Preparation normalizes the initial response metadata to the supplied commit
// plan. Mutating the response afterward can invalidate that relationship; this
// object is not a permanent proof over a mutable response.
class http_response_stream_head final {
public:
    [[nodiscard]] http_response& response() & noexcept {
        return response_;
    }
    [[nodiscard]] http_response& response() && = delete;

    [[nodiscard]] const http_response& response() const& noexcept {
        return response_;
    }
    [[nodiscard]] const http_response& response() const&& = delete;

    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const& noexcept {
        return commit_plan_;
    }
    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const&& = delete;

private:
    friend http_response_stream_head prepare_http_response_stream_head(http_response, http_response_stream_commit_plan);

    http_response_stream_head(http_response response, http_response_stream_commit_plan commit_plan) :
        response_(std::move(response)),
        commit_plan_(commit_plan) {}

    http_response response_;
    http_response_stream_commit_plan commit_plan_;
};

// An application Transfer-Encoding must be exactly the `chunked` coding (OWS
// and ASCII case aside): it is a hint the framework's writer replaces with its
// own framing. Any other value throws http1_response_head_error for an HTTP/1
// framing and std::invalid_argument for HTTP/2 or HTTP/3 before the response
// changes.
[[nodiscard]] http_response_stream_head prepare_http_response_stream_head(http_response response, http_response_stream_commit_plan commit_plan);
class http1_response_stream_plan final {
public:
    [[nodiscard]] http_response_stream_framing framing() const noexcept {
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
    friend http1_response_stream_plan http1_plan_response_stream(const http1_server_request_parse_state&, http1_close_policy, http1_request_body_consumption) noexcept;

    http1_response_stream_plan(http_response_stream_framing framing, http1_request_connection_plan request_connection_plan, http1_close_policy close_policy, http_known_method request_method) noexcept :
        framing_(framing),
        request_connection_plan_(request_connection_plan),
        close_policy_(close_policy),
        request_method_(request_method) {}

    http_response_stream_framing framing_;
    http1_request_connection_plan request_connection_plan_;
    http1_close_policy close_policy_{http1_close_policy::close_after_response};
    http_known_method request_method_{http_known_method::unknown};
};

// Pure HTTP/1 response-stream planning. The runtime contributes only its typed
// product policy (for example, a per-connection request limit) and the request
// body's consumption state; HTTP retains the request disposition and candidate
// framing until the response status is known. Commit can therefore distinguish
// a body-allowed HTTP/1.0 stream, which requires close delimiting, from a
// body-suppressed response that is already self-delimited. A route that consumed
// the complete request body before streaming passes `complete`; the parser's
// body plan still describes the original request framing, so planning for an
// unread body passes `body_plan_.consumption_if_unread()`.
[[nodiscard]] inline http1_response_stream_plan http1_plan_response_stream(const http1_server_request_parse_state& parsed_value, http1_close_policy close_policy, http1_request_body_consumption consumption) noexcept {
    const auto framing = parsed_value.request().protocol_version() == http_protocol_version::http11 ? http_response_stream_framing::http1_chunked : http_response_stream_framing::http1_close_delimited;
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
        response.header_stable_view("Connection", base);
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
// bytes to the authoritative connection disposition.
class prepared_http1_response_stream final {
public:
    [[nodiscard]] http_response& response() & noexcept {
        return head_.response();
    }
    [[nodiscard]] http_response& response() && = delete;

    [[nodiscard]] const http_response& response() const& noexcept {
        return head_.response();
    }
    [[nodiscard]] const http_response& response() const&& = delete;

    [[nodiscard]] const http1_response_head_plan& response_head_plan() const& noexcept {
        return response_head_plan_;
    }
    [[nodiscard]] const http1_response_head_plan& response_head_plan() const&& = delete;

    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const& noexcept {
        return head_.commit_plan();
    }
    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const&& = delete;

    [[nodiscard]] http1_request_connection_plan connection_plan() const noexcept {
        return connection_plan_;
    }

private:
    friend prepared_http1_response_stream prepare_http1_response_stream_head(http_response, const http1_response_stream_plan&, http_response_trailer_intent);

    prepared_http1_response_stream(http_response_stream_head head, http1_response_head_plan response_head_plan, http1_request_connection_plan connection_plan) noexcept :
        head_(std::move(head)),
        response_head_plan_(response_head_plan),
        connection_plan_(connection_plan) {}

    http_response_stream_head head_;
    http1_response_head_plan response_head_plan_;
    http1_request_connection_plan connection_plan_;
};

// The response's own valid Content-Length, without a Transfer-Encoding hint
// (which overrides it, RFC 9112 §6.3), selects http1_length_delimited for a
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
