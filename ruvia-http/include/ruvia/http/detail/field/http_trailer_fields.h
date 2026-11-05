#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/field/http_field_list_grammar.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"

namespace ruvia::detail {

// Shared request/response restrictions (RFC 9110 section 6.5.1): fields that
// control framing, routing, authentication or representation interpretation.
// Direction-specific permissions are composed below and by response policy.
[[nodiscard]] inline bool is_forbidden_common_trailer_name(
    std::string_view name, request_header_kind kind) noexcept {
    switch (kind) {
        case request_header_kind::host:
        case request_header_kind::content_length:
        case request_header_kind::transfer_encoding:
        case request_header_kind::connection:
        case request_header_kind::content_encoding:
        case request_header_kind::content_type:
        case request_header_kind::cookie:
        case request_header_kind::expect:
        case request_header_kind::if_match:
        case request_header_kind::if_modified_since:
        case request_header_kind::if_none_match:
        case request_header_kind::if_range:
        case request_header_kind::if_unmodified_since:
        case request_header_kind::range:
        case request_header_kind::upgrade:
        case request_header_kind::authorization:
        case request_header_kind::te:
        case request_header_kind::trailer:
        case request_header_kind::proxy_authorization:
            return true;
        case request_header_kind::other:
        case request_header_kind::accept:
        case request_header_kind::accept_encoding:
        case request_header_kind::access_control_request_headers:
        case request_header_kind::access_control_request_method:
        case request_header_kind::origin:
        case request_header_kind::user_agent:
        case request_header_kind::sec_websocket_key:
        case request_header_kind::sec_websocket_protocol:
        case request_header_kind::sec_websocket_version:
        case request_header_kind::forwarded:
        case request_header_kind::x_forwarded_for:
        case request_header_kind::x_forwarded_proto:
        case request_header_kind::sec_websocket_extensions:
            break;
    }

    switch (name.size()) {
        case 10:
            return http_ascii_equals_ignore_case(name, "Keep-Alive") ||
                   http_ascii_equals_ignore_case(name, "Set-Cookie");
        case 12:
            return http_ascii_equals_ignore_case(name, "Max-Forwards");
        case 13:
            return http_ascii_equals_ignore_case(name, "Cache-Control") ||
                   http_ascii_equals_ignore_case(name, "Content-Range");
        case 16:
            return http_ascii_equals_ignore_case(name, "Proxy-Connection");
        case 18:
            return http_ascii_equals_ignore_case(name, "Proxy-Authenticate");
        default:
            return false;
    }
}

// Trailer advertisements and actual request sections use the same policy.
// Accept-Ranges is permitted in response trailers, not request trailers.
[[nodiscard]] inline bool is_forbidden_http_request_trailer_name(std::string_view name) noexcept {
    const auto kind = classify_request_header(name);
    if (is_forbidden_common_trailer_name(name, kind)) {
        return true;
    }
    return kind == request_header_kind::access_control_request_headers ||
           kind == request_header_kind::access_control_request_method ||
           kind == request_header_kind::origin ||
           http_ascii_equals_ignore_case(name, "Accept-Ranges");
}

template <typename forbidden_name_type>
[[nodiscard]] inline bool is_valid_http_trailer_field_value(
    std::string_view value, http_field_list_role role, forbidden_name_type&& forbidden_name) noexcept {
    // RFC 9110 section 5.6.1.1: senders must not generate empty list elements,
    // but `#field-name` itself can represent an empty list.
    const auto list_role = http_trim_ows(value).empty() ? http_field_list_role::recipient : role;
    return http_visit_token_list(value, list_role, [&forbidden_name](std::string_view name) noexcept {
        return !forbidden_name(name);
    }) == http_field_list_parse_status::ok;
}

[[nodiscard]] inline bool is_valid_http_request_trailer_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    return is_valid_http_trailer_field_value(value, role,
        [](std::string_view name) noexcept { return is_forbidden_http_request_trailer_name(name); });
}

enum class http_trailer_block_status : std::uint8_t {
    ok,
    invalid_field,
    section_too_large,
    visitor_stopped,
};

// The single HTTP/1 received trailer-block field parser for both message
// directions. `block` holds the field lines between the last chunk and the
// empty line that ends the trailer section; a final CRLF after the last field
// is accepted for standalone validation. Values are exposed with field-line OWS
// trimmed.
// `obs_fold` decides whether a line starting with OWS continues the previous
// field (RFC 9112 section 5.2). Rejected, such a line is malformed. Accepted,
// the field is validated and charged as its HTTP unfolding (each obs-fold and
// adjacent OWS is one SP), and the visitor receives the received value span
// with its folds in place; the consumer unfolds it before interpretation.
// `discarded_name` is the direction's sender trailer policy. The sender rule of
// RFC 9110 section 6.5.1 does not make a received field invalid: a recipient
// that removes the chunked coding may selectively discard received trailer
// fields (RFC 9112 section 7.1.2), so a field the sender was not permitted to
// generate is skipped without reaching the visitor. Syntax errors and the size
// budget still fail the whole section. `budget` is the section total before this
// block, so a caller can extend a retained section; it is advanced for every
// received field, discarded or not.
// The visitor receives views borrowing `block` and returns false to stop.
template <typename discarded_name_type, typename visitor_type>
[[nodiscard]] http_trailer_block_status visit_http1_trailer_block(std::string_view block,
    http1_trailer_obs_fold obs_fold, discarded_name_type&& discarded_name, http_field_section_budget& budget,
    visitor_type&& visitor) {
    if (block.size() > max_http_header_bytes) {
        return http_trailer_block_status::section_too_large;
    }
    const auto line_at = [block](std::size_t begin, std::size_t end) noexcept {
        return end == std::string_view::npos ? block.substr(begin) : block.substr(begin, end - begin);
    };
    std::size_t cursor_value = 0;
    while (cursor_value < block.size()) {
        auto line_end = block.find("\r\n", cursor_value);
        const auto line = line_at(cursor_value, line_end);
        // Finding the separator also proves that the complete name is a token.
        const auto colon = http_token_prefix_size(line);
        if (colon == 0 || colon == line.size() || line[colon] != ':') {
            return http_trailer_block_status::invalid_field;
        }
        const auto name = line.substr(0, colon);
        // The unfolded value spans the first to the last non-empty piece, each
        // fold between them counting as one SP.
        auto piece = http_trim_ows(line.substr(colon + 1));
        auto value = piece;
        std::size_t value_bytes = 0;
        std::size_t pending_separators = 0;
        for (;;) {
            if (!is_valid_http_field_value_bytes(piece)) {
                return http_trailer_block_status::invalid_field;
            }
            if (!piece.empty()) {
                if (value_bytes == 0) {
                    value = piece;
                } else {
                    value_bytes += pending_separators;
                    value = std::string_view(value.data(), static_cast<std::size_t>(piece.data() - value.data()) + piece.size());
                }
                value_bytes += piece.size();
                pending_separators = 0;
            }
            if (obs_fold == http1_trailer_obs_fold::reject || line_end == std::string_view::npos ||
                line_end + 2 >= block.size() || !is_http_ows_char(block[line_end + 2])) {
                break;
            }
            ++pending_separators;
            const auto piece_begin = line_end + 2;
            line_end = block.find("\r\n", piece_begin);
            piece = http_trim_ows(line_at(piece_begin, line_end));
        }
        if (!budget.add_field(name.size(), value_bytes)) {
            return http_trailer_block_status::section_too_large;
        }
        if (!discarded_name(name) && !visitor(name, value)) {
            return http_trailer_block_status::visitor_stopped;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        cursor_value = line_end + 2;
    }
    return http_trailer_block_status::ok;
}

}  // namespace ruvia::detail
