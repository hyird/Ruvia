#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_status.h"

namespace ruvia::detail {

inline constexpr std::string_view http10_status_line_prefix = "HTTP/1.0 ";
inline constexpr std::string_view http11_status_line_prefix = "HTTP/1.1 ";
// A head can be measured before its protocol version is chosen for emission.
static_assert(http10_status_line_prefix.size() == http11_status_line_prefix.size());

[[nodiscard]] constexpr std::size_t http1_status_line_size(std::string_view reason_phrase) noexcept {
    return http11_status_line_prefix.size() + http_status_code_token_size + 1 + reason_phrase.size() + 2;
}

// The single HTTP/1 status-line emitter (RFC 9112 section 4):
// HTTP-version SP status-code SP [ reason-phrase ] CRLF. Its size is
// http1_status_line_size(reason_phrase).
template <typename sink_type>
constexpr void emit_http1_status_line(sink_type& sink, http_protocol_version version,
    http_status_code status, std::string_view reason_phrase) {
    sink.append(version == http_protocol_version::http10 ? http10_status_line_prefix : http11_status_line_prefix);
    const auto status_token = http_status_code_token(status);
    sink.append(http_status_code_token_view(status_token));
    // RFC 9112 requires this SP even when the optional reason phrase is empty.
    sink.append(' ');
    sink.append(reason_phrase);
    sink.append(std::string_view("\r\n"));
}

}  // namespace ruvia::detail
