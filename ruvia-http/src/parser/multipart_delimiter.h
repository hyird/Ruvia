#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <variant>

#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/multipart_parser.h"

// Locating a multipart delimiter line in a byte run (RFC 2046 section 5.1.1):
// the CRLF--boundary sequence that separates parts, its closing form, and the
// "need more input" state a partial trailing match reports. Scanning only; part
// headers and the boundary parameter itself are parsed elsewhere.

namespace ruvia::detail {

enum class http_multipart_delimiter_kind : std::uint8_t {
    no_match,
    invalid,
    need_input,
    part,
    close,
};

// Delimiter scanning distinguishes absence, a malformed delimiter line, an
// input-boundary ambiguity, a regular part delimiter, and the terminal close
// delimiter. Candidate outcomes expose their offset; complete delimiter lines
// expose their line length.
struct http_multipart_delimiter_result final {
    http_multipart_delimiter_kind kind_{http_multipart_delimiter_kind::no_match};
    std::size_t offset_{0};
    std::size_t line_bytes_{0};

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_no_match() noexcept {
        return {.kind_ = http_multipart_delimiter_kind::no_match};
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_invalid() noexcept {
        return {.kind_ = http_multipart_delimiter_kind::invalid};
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_need_input(
        std::size_t offset = 0) noexcept {
        return {.kind_ = http_multipart_delimiter_kind::need_input, .offset_ = offset};
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_part(
        std::size_t offset, std::size_t line_bytes) noexcept {
        return {.kind_ = http_multipart_delimiter_kind::part,
            .offset_ = offset,
            .line_bytes_ = line_bytes};
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_close(
        std::size_t offset, std::size_t line_bytes) noexcept {
        return {.kind_ = http_multipart_delimiter_kind::close,
            .offset_ = offset,
            .line_bytes_ = line_bytes};
    }

    [[nodiscard]] constexpr http_multipart_delimiter_result rebased(std::size_t base) const noexcept {
        if (kind_ == http_multipart_delimiter_kind::need_input ||
            kind_ == http_multipart_delimiter_kind::part ||
            kind_ == http_multipart_delimiter_kind::close) {
            return {.kind_ = kind_, .offset_ = base + offset_, .line_bytes_ = line_bytes_};
        }
        return *this;
    }
};

[[nodiscard]] inline bool http_multipart_marker_prefix_matches(
    std::string_view input, std::string_view boundary) noexcept {
    const auto expected_size = boundary.size() + 2;
    const auto compared = std::min(input.size(), expected_size);
    for (std::size_t index = 0; index < compared; ++index) {
        const char expected = index < 2 ? '-' : boundary[index - 2];
        if (input[index] != expected) {
            return false;
        }
    }
    return true;
}

// Matches one delimiter line beginning at its leading "--". RFC 2046
// transport-padding is accepted after a regular delimiter or after the closing
// "--". A closing delimiter ending exactly at the current buffer boundary is
// complete only when the I/O owner has signalled end-of-input.
[[nodiscard]] inline http_multipart_delimiter_result http_match_multipart_delimiter_line(
    std::string_view input, const multipart_boundary& boundary, bool input_finished,
    std::size_t& padding_cursor) noexcept {
    const auto value = boundary.value();
    const auto marker_size = value.size() + 2;
    if (!http_multipart_marker_prefix_matches(input, value)) {
        return http_multipart_delimiter_result::make_no_match();
    }
    if (input.size() < marker_size) {
        return input_finished ? http_multipart_delimiter_result::make_no_match()
                              : http_multipart_delimiter_result::make_need_input();
    }

    std::size_t cursor_value = marker_size;
    bool close = false;
    if (cursor_value < input.size() && input[cursor_value] == '-') {
        if (cursor_value + 1 >= input.size()) {
            return input_finished ? http_multipart_delimiter_result::make_no_match()
                                  : http_multipart_delimiter_result::make_need_input();
        }
        if (input[cursor_value + 1] != '-') {
            return http_multipart_delimiter_result::make_invalid();
        }
        close = true;
        cursor_value += 2;
    }

    cursor_value = std::max(cursor_value, padding_cursor);
    // transport-padding is *LWSP-char (RFC 2046 section 5.1.1): SP / HTAB.
    while (cursor_value < input.size() && is_http_ows_char(input[cursor_value])) {
        ++cursor_value;
    }
    padding_cursor = cursor_value;
    if (cursor_value == input.size()) {
        if (close && input_finished) {
            return http_multipart_delimiter_result::make_close(0, cursor_value);
        }
        return input_finished ? http_multipart_delimiter_result::make_no_match()
                              : http_multipart_delimiter_result::make_need_input();
    }
    if (input[cursor_value] != '\r') {
        return http_multipart_delimiter_result::make_invalid();
    }
    if (cursor_value + 1 >= input.size()) {
        return input_finished ? http_multipart_delimiter_result::make_no_match()
                              : http_multipart_delimiter_result::make_need_input();
    }
    if (input[cursor_value + 1] != '\n') {
        return http_multipart_delimiter_result::make_invalid();
    }
    return close ? http_multipart_delimiter_result::make_close(0, cursor_value + 2)
                 : http_multipart_delimiter_result::make_part(0, cursor_value + 2);
}

// Cursors belong to one retained input prefix and must be reset after consume.
// Searches keep at most the three-byte delimiter prefix overlap; a candidate's
// transport padding resumes at its last inspected byte.
[[nodiscard]] inline http_multipart_delimiter_result http_find_multipart_delimiter_from_cursor(
    std::string_view input, const multipart_boundary& boundary, bool input_finished,
    bool check_leading_dash, std::size_t rebase_crlf_offset,
    std::size_t& search_cursor, std::size_t& padding_cursor) noexcept {
    if (check_leading_dash && search_cursor == 0 && !input.empty() && input.front() == '-') {
        auto match = http_match_multipart_delimiter_line(input, boundary, input_finished, padding_cursor);
        if (match.kind_ != http_multipart_delimiter_kind::no_match) {
            return match;
        }
        padding_cursor = 0;
    }
    for (auto prefix = input.find("\r\n--", search_cursor); prefix != std::string_view::npos;
        prefix = input.find("\r\n--", prefix + 1)) {
        auto match = http_match_multipart_delimiter_line(
            input.substr(prefix + 2), boundary, input_finished, padding_cursor);
        if (match.kind_ == http_multipart_delimiter_kind::no_match) {
            padding_cursor = 0;
            continue;
        }
        search_cursor = prefix;
        return match.rebased(prefix + rebase_crlf_offset);
    }
    search_cursor = input.size() > 3 ? input.size() - 3 : 0;
    return http_multipart_delimiter_result::make_no_match();
}

[[nodiscard]] inline http_multipart_delimiter_result http_find_initial_multipart_delimiter(
    std::string_view input, const multipart_boundary& boundary, bool input_finished,
    std::size_t& search_cursor, std::size_t& padding_cursor) noexcept {
    auto match = http_find_multipart_delimiter_from_cursor(
        input, boundary, input_finished, true, 2, search_cursor, padding_cursor);
    if (match.kind_ != http_multipart_delimiter_kind::no_match) {
        return match;
    }
    // The CRLF belongs to the preamble, but the first marker byte may still
    // become a delimiter. Keep it out of the preamble quota until the next
    // byte resolves the candidate or EOF makes the prefix incomplete.
    if (!input_finished && input.ends_with("\r\n-")) {
        return http_multipart_delimiter_result::make_need_input(input.size() - 1);
    }
    return match;
}

// RFC 2046 section 5.1.1: body-part = MIME-part-headers [CRLF *OCTET], so a part
// may end right after its header block and the next delimiter's CRLF is the one
// that closed the headers. Body lines cannot start with the dash-boundary, so at
// the body start a delimiter line at offset 0 is a delimiter, not content.
[[nodiscard]] inline http_multipart_delimiter_result http_find_multipart_body_delimiter(
    std::string_view input, const multipart_boundary& boundary, bool input_finished, bool at_body_start,
    std::size_t& search_cursor, std::size_t& padding_cursor) noexcept {
    return http_find_multipart_delimiter_from_cursor(
        input, boundary, input_finished, at_body_start, 0, search_cursor, padding_cursor);
}

}  // namespace ruvia::detail
