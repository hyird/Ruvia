#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "decode_step.h"
#include "http1_client_request.h"
#include "http1_connection_plan.h"
#include "http_client_response_head.h"
#include "http_content_coding.h"
#include "http_message.h"
#include "http_status.h"

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

// Result of notifying the exchange that the external runtime finished writing
// every byte in the prepared request content plan. This event is required before
// a content-bearing request can accept 101 Switching Protocols.
enum class http1_client_request_content_completion_status : std::uint8_t {
    completed,
    already_complete,
    exchange_terminal,
};

// Immutable RFC 9110/9112 response framing and lifecycle contract. A head is
// exactly one of: an informational response (the exchange awaits another
// head), a final response delimited by its HTTP/1 framing, a CONNECT tunnel,
// or a protocol upgrade. The framing mirrors message-length precedence and
// content semantics directly: no_content is a final response that cannot
// carry content (HEAD, 204, 304), while a zero known_length still has framing.
class http1_client_response_plan final {
public:
    http1_client_response_plan(const http1_client_response_plan&) noexcept = default;
    http1_client_response_plan& operator=(const http1_client_response_plan&) = delete;
    http1_client_response_plan(http1_client_response_plan&&) noexcept = default;
    http1_client_response_plan& operator=(http1_client_response_plan&&) = delete;

    [[nodiscard]] constexpr bool informational() const noexcept {
        return std::holds_alternative<informational_type>(state_);
    }

    // The framing of a final response; null for the other alternatives.
    [[nodiscard]] constexpr const http1_message_framing* framing() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_message_framing>(&state_);
    }
    const http1_message_framing* framing() const&& = delete;

    [[nodiscard]] constexpr bool connect_tunnel() const noexcept {
        return std::holds_alternative<connect_tunnel_type>(state_);
    }

    [[nodiscard]] constexpr bool protocol_upgrade() const noexcept {
        return std::holds_alternative<protocol_upgrade_type>(state_);
    }

    // Connection lifecycle after a self-delimited response has been consumed.
    // For an informational response, reuse means the same exchange can await
    // its final response; it does not make the connection poolable before that
    // final. A close-delimited response always closes, and a tunnel or upgrade
    // never returns the connection to HTTP: all three report
    // close_after_response.
    [[nodiscard]] constexpr http1_close_policy close_policy() const noexcept {
        return close_policy_;
    }

    // A 205 response with an ordinary framing. RFC 9110 section 15.3.6 forbids
    // only generating its content, so content the framing delimits is consumed
    // and discarded (http1_body_decoder::discard_content()), and the
    // connection stays reusable.
    [[nodiscard]] constexpr bool discards_content() const noexcept {
        return discards_content_;
    }

    // Only exchange_complete cancels pending request content. Receiving an early
    // final response without that signal leaves the external writer responsible
    // for finishing the request before the connection can be reused (RFC 9110
    // section 7.5), even though this response-head parser is already terminal.
    [[nodiscard]] constexpr std::optional<http_client_request_content_signal> request_content_signal() const noexcept {
        return request_content_signal_;
    }

private:
    friend struct detail::http1_client_response_plan_access;

    struct informational_type final {};
    struct connect_tunnel_type final {};
    struct protocol_upgrade_type final {};
    using state_type = std::variant<informational_type, http1_message_framing, connect_tunnel_type, protocol_upgrade_type>;

    constexpr http1_client_response_plan(state_type state_value, http1_close_policy close_policy, std::optional<http_client_request_content_signal> request_content_signal, bool discards_content = false) noexcept :
        state_(state_value),
        request_content_signal_(request_content_signal),
        close_policy_(close_policy),
        discards_content_(discards_content) {}

    state_type state_;
    std::optional<http_client_request_content_signal> request_content_signal_;
    http1_close_policy close_policy_;
    bool discards_content_{false};
};

static_assert(std::is_nothrow_move_constructible_v<http1_client_response_plan>);

// Protocol failures use typed values whose representation does not allocate.
// Resource exhaustion can throw while unfolding fields, storing temporary
// descriptors, or materializing an owning response. Exchange termination is
// NOT a parse error: parsing after the exchange completed or failed throws
// std::logic_error until reset().
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

[[nodiscard]] std::string_view http1_client_response_parse_error_message(http1_client_response_parse_error error) noexcept;

// One complete response head. The response owns status/header storage. The
// step that carries it reports the exact boundary after CRLF CRLF as
// consumed_bytes(), so a sans-I/O driver can feed the remaining bytes to the
// body/tunnel/upgrade path directly.
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

private:
    friend struct detail::http1_client_response_parse_result_access;

    http1_parsed_client_response_head(http_client_response_head head, http1_client_response_plan plan) noexcept :
        head_(std::move(head)),
        plan_(std::move(plan)) {}

    http_client_response_head head_;
    http1_client_response_plan plan_;
};

static_assert(std::is_nothrow_move_constructible_v<http1_parsed_client_response_head>);

using http1_client_response_parse_failure = decode_failure<http1_client_response_parse_error>;

// One parse() outcome: the head is incomplete, complete (consumed_bytes() is
// then its exact size, zero otherwise), or rejected. The parsed head is
// move-only: extract it through the step that holds it.
using http1_client_response_parse_result = decode_step<decode_need_input, http1_parsed_client_response_head, http1_client_response_parse_failure>;

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

    // After need_input, append to the same logical prefix; after a parsed head,
    // remove consumed_bytes() before supplying the following response.
    // Allocation failures propagate without completing or failing the exchange;
    // after reclaiming resources, retry with the same response prefix.
    [[nodiscard]] http1_client_response_parse_result parse(std::string_view buffer) &;
    http1_client_response_parse_result parse(std::string_view) && = delete;

    // Active while a response head is awaited (informational heads keep it
    // active); finished once the final response, tunnel or upgrade head was
    // parsed; failed after a rejected head.
    [[nodiscard]] decoder_state state() const noexcept {
        return state_;
    }
    // Starts the response side of another prepared request.
    void reset(http1_client_exchange_state exchange_state) noexcept;

private:
    [[nodiscard]] static detail::http1_client_request_content_phase initial_request_content_phase(const http1_client_exchange_state& state_value) noexcept;

    http1_client_exchange_state exchange_state_;
    decoder_state state_{decoder_state::active};
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
