#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/http1_client_exchange_state.h"
#include "ruvia/http/http_client_response_head.h"
#include "ruvia/http/http_transfer_coding.h"

namespace ruvia {

namespace detail {

struct http1_client_response_parse_result_access;
struct http1_client_response_plan_access;

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
    [[nodiscard]] const http_transfer_codings& transfer_codings() const noexcept {
        return transfer_codings_;
    }

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
    [[nodiscard]] const http_transfer_codings& transfer_codings() const noexcept {
        return transfer_codings_;
    }

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
        const& noexcept {
        return std::get_if<http1_client_informational_response>(&state_);
    }
    const http1_client_informational_response* informational() const&& = delete;

    [[nodiscard]] constexpr const http1_client_response_without_content* without_content()
        const& noexcept {
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
    [[nodiscard]] constexpr const http1_client_known_length_response* known_length() const& noexcept {
        return std::get_if<http1_client_known_length_response>(&state_);
    }
    const http1_client_known_length_response* known_length() const&& = delete;

    [[nodiscard]] constexpr const http1_client_chunked_response* chunked() const& noexcept {
        return std::get_if<http1_client_chunked_response>(&state_);
    }
    const http1_client_chunked_response* chunked() const&& = delete;

    [[nodiscard]] constexpr const http1_client_close_delimited_response* close_delimited()
        const& noexcept {
        return std::get_if<http1_client_close_delimited_response>(&state_);
    }
    const http1_client_close_delimited_response* close_delimited() const&& = delete;

    [[nodiscard]] constexpr const http1_client_connect_tunnel* connect_tunnel() const& noexcept {
        return std::get_if<http1_client_connect_tunnel>(&state_);
    }
    const http1_client_connect_tunnel* connect_tunnel() const&& = delete;

    [[nodiscard]] constexpr const http1_client_protocol_upgrade* protocol_upgrade() const& noexcept {
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

// One complete response head. The response owns status/header storage through
// PMR; consumed_bytes() is the exact boundary after CRLF CRLF, so a sans-I/O
// driver can feed the remaining bytes to the body/tunnel/upgrade path directly.
class http1_parsed_client_response_head final {
public:
    http1_parsed_client_response_head(const http1_parsed_client_response_head&) = delete;
    http1_parsed_client_response_head& operator=(const http1_parsed_client_response_head&) = delete;
    http1_parsed_client_response_head(http1_parsed_client_response_head&&) noexcept = default;
    http1_parsed_client_response_head& operator=(http1_parsed_client_response_head&&) = delete;

    [[nodiscard]] const http_client_response_head& head() const& noexcept {
        return head_;
    }
    [[nodiscard]] const http_client_response_head& head() const&& = delete;

    [[nodiscard]] http_client_response_head take_head() && noexcept {
        return std::move(head_);
    }

    [[nodiscard]] const http1_client_response_plan& plan() const& noexcept {
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

    [[nodiscard]] const http1_client_response_need_more* need_more() const& noexcept {
        return std::get_if<http1_client_response_need_more>(&state_);
    }
    const http1_client_response_need_more* need_more() const&& = delete;

    [[nodiscard]] http1_parsed_client_response_head* parsed() & noexcept {
        return std::get_if<http1_parsed_client_response_head>(&state_);
    }

    [[nodiscard]] const http1_parsed_client_response_head* parsed() const& noexcept {
        return std::get_if<http1_parsed_client_response_head>(&state_);
    }
    http1_parsed_client_response_head* parsed() && = delete;
    const http1_parsed_client_response_head* parsed() const&& = delete;

    [[nodiscard]] const http1_client_response_parse_error* failure() const& noexcept {
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
// bounded inline storage and spill to the supplied PMR as needed. Response
// obs-fold is replaced with spaces before field validation; its temporary
// storage uses the same PMR.
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
// HEAD/304 Content-Length representation metadata still requires valid,
// consistent lengths; ignored 1xx/204/CONNECT framing values are not interpreted.
// Any number of informational heads may precede the final response; each head
// remains subject to the header limits and the supplied memory resource.
class http1_client_response_parser final {
public:
    explicit http1_client_response_parser(
        http1_client_exchange_state exchange_state, std::pmr::memory_resource* resource = nullptr) noexcept;

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
    [[nodiscard]] static constexpr detail::http1_client_request_content_phase
    initial_request_content_phase(const http1_client_exchange_state& state_value) noexcept {
        switch (detail::http1_client_exchange_state_access::content_state(state_value)) {
            case detail::http1_client_initial_content_state::complete:
                return detail::http1_client_request_content_phase::content_complete;
            case detail::http1_client_initial_content_state::pending:
                return detail::http1_client_request_content_phase::content_pending;
            case detail::http1_client_initial_content_state::awaiting_continue:
                return detail::http1_client_request_content_phase::awaiting_continue;
        }
        return detail::http1_client_request_content_phase::content_complete;
    }

    enum class phase_type : std::uint8_t {
        await_response,
        complete,
        failed,
    };

    http1_client_exchange_state exchange_state_;
    std::pmr::memory_resource* resource_;
    phase_type phase_{phase_type::await_response};
    detail::http1_client_request_content_phase request_content_phase_;
    bool interim_faulty_framing_{false};
    std::size_t header_scan_offset_{0};
};

inline http1_client_response_parser::http1_client_response_parser(
    http1_client_exchange_state exchange_state, std::pmr::memory_resource* resource) noexcept
    : exchange_state_(std::move(exchange_state)),
      resource_(resource),
      request_content_phase_(initial_request_content_phase(exchange_state_)) {}

}  // namespace ruvia
