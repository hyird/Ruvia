#pragma once

#include <array>
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

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"

namespace ruvia {

// An HTTP/1 response head this endpoint cannot send: an interim, final,
// CONNECT or CONNECT-UDP head that breaks a message rule (RFC 9110, RFC 9112)
// or the head size bound. It reports an application or runtime bug, never a
// peer error, so a server answers it like any other handler failure.
class http1_response_head_error final : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

}  // namespace ruvia

namespace ruvia {

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

}  // namespace ruvia

namespace ruvia {

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

    explicit constexpr http1_buffered_response_head(std::uint64_t content_length) noexcept
        : content_length_(content_length) {}

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

// Final HTTP/1 head framing is an exclusive protocol value, not the old
// "suppress automatic Content-Length" boolean. The body plan keeps method/status
// content semantics attached to the exact wire-framing alternative selected by
// the HTTP/1 planner.
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

    [[nodiscard]] constexpr const http1_close_delimited_response_stream_head* close_delimited_stream()
        const& noexcept {
        return std::get_if<http1_close_delimited_response_stream_head>(&framing_);
    }
    [[nodiscard]] constexpr const http1_close_delimited_response_stream_head* close_delimited_stream()
        const&& = delete;

    [[nodiscard]] constexpr http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    [[nodiscard]] constexpr http_protocol_version protocol_version() const noexcept {
        return protocol_version_;
    }

private:
    friend http1_buffered_response_plan get_http1_buffered_response_plan(
        http_buffered_response_write_plan, http1_request_connection_plan) noexcept;
    friend constexpr http1_response_head_plan http1_chunked_response_stream_head_plan(
        http_response_body_plan, http1_request_connection_plan) noexcept;
    friend constexpr http1_response_head_plan http1_close_delimited_response_stream_head_plan(
        http_response_body_plan, http1_request_connection_plan) noexcept;

    using framing_type = std::variant<http1_buffered_response_head,
        http1_chunked_response_stream_head, http1_close_delimited_response_stream_head>;

    [[nodiscard]] static constexpr framing_type buffered_framing(std::uint64_t content_length) noexcept {
        return framing_type(http1_buffered_response_head(content_length));
    }

    [[nodiscard]] static constexpr framing_type chunked_stream_framing() noexcept {
        return framing_type(http1_chunked_response_stream_head());
    }

    [[nodiscard]] static constexpr framing_type close_delimited_stream_framing() noexcept {
        return framing_type(http1_close_delimited_response_stream_head());
    }

    constexpr http1_response_head_plan(http_response_body_plan body_plan,
        http_protocol_version protocol_version, framing_type framing) noexcept
        : body_plan_(body_plan),
          protocol_version_(protocol_version),
          framing_(framing) {}

    http_response_body_plan body_plan_;
    http_protocol_version protocol_version_;
    framing_type framing_;
};

[[nodiscard]] constexpr http1_response_head_plan http1_chunked_response_stream_head_plan(
    http_response_body_plan body_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_response_head_plan(
        body_plan, connection_plan.protocol_version(), http1_response_head_plan::chunked_stream_framing());
}

[[nodiscard]] constexpr http1_response_head_plan http1_close_delimited_response_stream_head_plan(
    http_response_body_plan body_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_response_head_plan(body_plan, connection_plan.protocol_version(),
        http1_response_head_plan::close_delimited_stream_framing());
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
    friend http1_buffered_response_plan get_http1_buffered_response_plan(
        http_buffered_response_write_plan, http1_request_connection_plan) noexcept;

    explicit constexpr http1_buffered_response_plan(http1_response_head_plan head_plan) noexcept
        : head_plan_(head_plan) {}

    http1_response_head_plan head_plan_;
};

static_assert(std::is_trivially_copyable_v<http1_buffered_response_plan>);
static_assert(sizeof(http1_buffered_response_plan) == sizeof(http1_response_head_plan));

[[nodiscard]] inline http1_buffered_response_plan get_http1_buffered_response_plan(
    http_buffered_response_write_plan write_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_buffered_response_plan(
        http1_response_head_plan(write_plan.body_plan(), connection_plan.protocol_version(),
            http1_response_head_plan::buffered_framing(write_plan.content_length())));
}

}  // namespace ruvia

// Stable, non-detail entry points for HTTP response serialization plans. The
// plan implementations remain shared with the HTTP/1 and HTTP/2 protocol code.

namespace ruvia {

namespace detail {

[[nodiscard]] bool is_forbidden_response_trailer_name(std::string_view name) noexcept;
[[nodiscard]] bool is_forbidden_http_binary_response_field(std::string_view name) noexcept;

}  // namespace detail

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
    friend http_response_trailer_section validate_http_response_trailers(
        std::span<const http_header_view>);
    explicit http_response_trailer_section(std::span<const http_header_view> fields_value) noexcept
        : fields_(fields_value) {}

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
void append_http1_response_head(
    const http_response& response, http_response_head_buffer& head,
    const http1_response_head_plan& plan);

// Append only field lines; chunk delimiters belong to the caller. Inputs may
// alias output. Allocation failure leaves the existing output bytes unchanged.
void append_http1_response_trailers(
    std::string& output, const http_response_trailer_section& trailers);

// Validate a complete response trailer section and return a borrowed proof for
// synchronous protocol submission. The input storage must outlive its use.
[[nodiscard]] http_response_trailer_section validate_http_response_trailers(
    std::span<const http_header_view> trailers);

template <detail::http_temporary_owning_range<http_header_view> headers_type>
http_response_trailer_section validate_http_response_trailers(headers_type&&) = delete;
http_response_trailer_section validate_http_response_trailers(
    std::initializer_list<http_header_view>) = delete;

}  // namespace ruvia
