#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "borrowed_text.h"
#include "decode_step.h"
#include "http1_connection_plan.h"
#include "http_protocol.h"
#include "http_request.h"
#include "http_request_content_plan.h"
#include "http_request_expectation.h"

namespace ruvia {

class http1_server_request_parser;

// Immutable framing contract produced only by the HTTP/1 parser and consumed
// by any runtime driver. A caller cannot synthesize a plan that bypasses wire
// validation. A request is never close-delimited (RFC 9112 section 6.3).
class http1_request_body_plan final {
public:
    [[nodiscard]] constexpr const http1_message_framing& framing() const& noexcept RUVIA_LIFETIMEBOUND {
        return framing_;
    }
    const http1_message_framing& framing() const&& = delete;

    // The consumption a response must record when the runtime never read the
    // request body: incomplete only if there is anything to read.
    [[nodiscard]] constexpr http1_request_body_consumption consumption_if_unread() const noexcept {
        return framing_.requires_consumption() ? http1_request_body_consumption::incomplete : http1_request_body_consumption::complete;
    }

    [[nodiscard]] http_request_expectations expectations() const noexcept {
        return expectations_;
    }

    [[nodiscard]] http_server_expectation_plan expectation_plan(http_unsupported_expectation_policy unsupported_policy) const noexcept {
        return expectations_.server_plan(framing_.requires_consumption() ? http_request_content_indication::will_follow : http_request_content_indication::no_content, unsupported_policy);
    }

private:
    friend class http1_server_request_parser;

    constexpr http1_request_body_plan(http1_message_framing framing, http_request_expectations expectations) noexcept :
        framing_(framing),
        expectations_(expectations) {}

    http1_message_framing framing_;
    http_request_expectations expectations_;
};

// Cleartext listeners can silently discard inputs whose request-line failure
// (invalid_request_line or unsupported_http_version) shows the peer was not
// speaking HTTP (for example, TLS bytes on an HTTP port). Input whose first
// line (after ignored empty lines; the available bytes while no CRLF has
// arrived) has a whitespace-separated word naming an HTTP version ("HTTP/" in
// any case, or token "/" DIGITs "." DIGITs) is HTTP and is not dropped.
// See RFC 9112 sections 2.2 and 3. Otherwise input is dropped once its first
// line is CRLF-terminated, or at once when its first byte cannot begin a
// method token. Any other parse error is answered.
[[nodiscard]] bool should_drop_invalid_cleartext_http1_input(std::string_view buffer, http_parse_error error) noexcept;

using http1_server_request_parse_failure = decode_failure<http_parse_error>;

// One parse() outcome: the head is incomplete, complete (consumed_bytes() is
// then its exact size through CRLF CRLF, zero otherwise), or rejected.
using http1_server_request_parse_result = decode_step<decode_need_input, decode_complete, http1_server_request_parse_failure>;

// Sans-I/O HTTP/1 request-head parser for one request at a time. The parser
// owns the request, body plan and connection plan it derives. The request's
// descriptor storage is kept across parse() attempts on one head and released
// by reset().
class http1_server_request_parser final {
public:
    http1_server_request_parser() noexcept;

    // Parses the head at the start of `buffer`. After need_input, call again
    // with the same bytes followed by more input: bytes already classified are
    // not rescanned. A complete or rejected head ends the parse; a parser that
    // is not active throws std::logic_error until reset().
    // 分配失败以 std::bad_alloc 传出且不发布完成状态；释放资源后可用同一 buffer 重试，但部分元数据可能残留。
    [[nodiscard]] http1_server_request_parse_result parse(std::string_view buffer);

    template <detail::http_temporary_owning_char_string buffer_type>
    http1_server_request_parse_result parse(buffer_type&&) = delete;

    [[nodiscard]] decoder_state state() const noexcept {
        return state_;
    }
    // Starts the next request: releases the request's descriptor storage and
    // restores the default plans.
    void reset() noexcept;

    // The request, body plan and connection plan parse() derived from its
    // input. A failure keeps the accepted request version and a connection
    // plan that requires close; an incomplete head keeps an empty request.
    [[nodiscard]] const http_request& request() const& noexcept RUVIA_LIFETIMEBOUND {
        return request_;
    }
    const http_request& request() const&& = delete;
    [[nodiscard]] const http1_request_body_plan& body_plan() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_plan_;
    }
    const http1_request_body_plan& body_plan() const&& = delete;
    [[nodiscard]] const http1_request_connection_plan& connection_plan() const& noexcept RUVIA_LIFETIMEBOUND {
        return connection_plan_;
    }
    const http1_request_connection_plan& connection_plan() const&& = delete;

    // Each extraction moves one result out and leaves the default value
    // behind; the parser then serves only reset() or destruction. Taking the
    // request releases its descriptor storage.
    [[nodiscard]] http_request take_request() && noexcept;
    [[nodiscard]] http1_request_body_plan take_body_plan() && noexcept;
    [[nodiscard]] http1_request_connection_plan take_connection_plan() && noexcept;

private:
    [[nodiscard]] http1_server_request_parse_result fail(http_parse_error error) noexcept;

    http_request request_;
    http1_request_body_plan body_plan_{http1_message_framing::no_content(), http_request_expectations{}};
    http1_request_connection_plan connection_plan_{http1_request_connection_plan::http11_close()};
    // Resumable scan progress: leading empty-line bytes already skipped, the
    // end of the request-line once accepted, and the input size the last
    // incomplete parse classified. Each input byte is thereby classified once
    // across repeated parse() calls.
    std::size_t leading_bytes_{0};
    std::size_t request_line_end_{0};
    std::size_t scanned_bytes_{0};
    decoder_state state_{decoder_state::active};
};

}  // namespace ruvia
