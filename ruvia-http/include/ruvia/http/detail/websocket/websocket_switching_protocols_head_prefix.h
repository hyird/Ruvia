#pragma once

#include <array>
#include <string_view>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/response/http1_status_line.h"
#include "ruvia/http/http_status.h"

namespace ruvia::detail {

// The fixed bytes of the HTTP/1.1 101 opening-handshake response (RFC 6455
// section 4.2.2) through the Sec-WebSocket-Accept field name; the accept key
// and its CRLF follow.
inline constexpr auto websocket_switching_protocols_head_prefix = [] {
    constexpr auto reason = http_reason_phrase(http_status::switching_protocols);
    constexpr std::string_view upgrade_field = "Upgrade: websocket\r\n";
    constexpr std::string_view field_separator = ": ";
    constexpr std::string_view crlf = "\r\n";
    constexpr std::string_view accept_field_name = "Sec-WebSocket-Accept: ";
    std::array<char, http1_status_line_size(reason) + upgrade_field.size() + http_connection_field_name.size() +
                         field_separator.size() + http_connection_upgrade_option.size() + crlf.size() +
                         accept_field_name.size()>
        result_value{};
    struct sink_type final {
        char* out_;

        constexpr void append(std::string_view part) noexcept {
            for (const char value : part) {
                *out_++ = value;
            }
        }

        constexpr void append(char value) noexcept {
            *out_++ = value;
        }
    } sink{result_value.data()};
    emit_http1_status_line(sink, http_status::switching_protocols, reason);
    sink.append(upgrade_field);
    sink.append(http_connection_field_name);
    sink.append(field_separator);
    sink.append(http_connection_upgrade_option);
    sink.append(crlf);
    sink.append(accept_field_name);
    return result_value;
}();

}  // namespace ruvia::detail
