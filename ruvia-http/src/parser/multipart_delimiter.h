#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>

#include "ruvia/http/multipart_parser.h"

// Locating a multipart delimiter line in a byte run (RFC 2046 section 5.1.1):
// the CRLF--boundary sequence that separates parts, its closing form, and the
// "need more input" state a partial trailing match reports. Scanning only; part
// headers and the boundary parameter itself are parsed elsewhere.

namespace ruvia::detail {

class http_multipart_delimiter_result;

[[nodiscard]] inline http_multipart_delimiter_result http_match_multipart_delimiter_line(
    std::string_view input, const multipart_boundary& boundary, bool input_finished,
    std::size_t* padding_cursor = nullptr) noexcept;
[[nodiscard]] inline http_multipart_delimiter_result http_find_initial_multipart_delimiter(
    std::string_view input, const multipart_boundary& boundary, bool input_finished,
    std::size_t* search_cursor = nullptr, std::size_t* padding_cursor = nullptr) noexcept;
[[nodiscard]] inline http_multipart_delimiter_result http_find_multipart_body_delimiter(
    std::string_view input, const multipart_boundary& boundary, bool input_finished,
    std::size_t* search_cursor = nullptr, std::size_t* padding_cursor = nullptr) noexcept;

class http_multipart_delimiter_no_match final {
private:
    friend class http_multipart_delimiter_result;
    constexpr http_multipart_delimiter_no_match() noexcept = default;
};

class http_multipart_delimiter_need_input final {
public:
    // Initial search: offset of the leading "--". Body search: offset of the
    // CRLF that belongs to the delimiter rather than the preceding part.
    [[nodiscard]] constexpr std::size_t offset() const noexcept {
        return offset_;
    }

private:
    friend class http_multipart_delimiter_result;

    explicit constexpr http_multipart_delimiter_need_input(std::size_t offset) noexcept
        : offset_(offset) {}

    std::size_t offset_;
};

class http_multipart_part_delimiter final {
public:
    [[nodiscard]] constexpr std::size_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::size_t line_bytes() const noexcept {
        return line_bytes_;
    }

private:
    friend class http_multipart_delimiter_result;

    constexpr http_multipart_part_delimiter(std::size_t offset, std::size_t line_bytes) noexcept
        : offset_(offset),
          line_bytes_(line_bytes) {}

    std::size_t offset_;
    std::size_t line_bytes_;
};

class http_multipart_close_delimiter final {
public:
    [[nodiscard]] constexpr std::size_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::size_t line_bytes() const noexcept {
        return line_bytes_;
    }

private:
    friend class http_multipart_delimiter_result;

    constexpr http_multipart_close_delimiter(std::size_t offset, std::size_t line_bytes) noexcept
        : offset_(offset),
          line_bytes_(line_bytes) {}

    std::size_t offset_;
    std::size_t line_bytes_;
};

// Delimiter scanning distinguishes absence, an input-boundary ambiguity, a
// regular part delimiter, and the terminal close delimiter. Only outcomes
// that found a candidate expose its offset; only complete delimiter lines
// expose their line length.
class http_multipart_delimiter_result final {
public:
    [[nodiscard]] constexpr const http_multipart_delimiter_no_match* no_match() const& noexcept {
        return std::get_if<http_multipart_delimiter_no_match>(&value_);
    }
    const http_multipart_delimiter_no_match* no_match() const&& = delete;

    [[nodiscard]] constexpr const http_multipart_delimiter_need_input* need_input() const& noexcept {
        return std::get_if<http_multipart_delimiter_need_input>(&value_);
    }
    const http_multipart_delimiter_need_input* need_input() const&& = delete;

    [[nodiscard]] constexpr const http_multipart_part_delimiter* part() const& noexcept {
        return std::get_if<http_multipart_part_delimiter>(&value_);
    }
    const http_multipart_part_delimiter* part() const&& = delete;

    [[nodiscard]] constexpr const http_multipart_close_delimiter* close() const& noexcept {
        return std::get_if<http_multipart_close_delimiter>(&value_);
    }
    const http_multipart_close_delimiter* close() const&& = delete;

private:
    friend http_multipart_delimiter_result http_match_multipart_delimiter_line(
        std::string_view, const multipart_boundary&, bool, std::size_t*) noexcept;
    friend http_multipart_delimiter_result http_find_initial_multipart_delimiter(
        std::string_view, const multipart_boundary&, bool, std::size_t*, std::size_t*) noexcept;
    friend http_multipart_delimiter_result http_find_multipart_body_delimiter(
        std::string_view, const multipart_boundary&, bool, std::size_t*, std::size_t*) noexcept;

    using value_type = std::variant<http_multipart_delimiter_no_match, http_multipart_delimiter_need_input,
        http_multipart_part_delimiter, http_multipart_close_delimiter>;

    template <typename result_type>
    explicit constexpr http_multipart_delimiter_result(result_type result_value) noexcept
        : value_(result_value) {}

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_no_match() noexcept {
        return http_multipart_delimiter_result(http_multipart_delimiter_no_match());
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_need_input(
        std::size_t offset = 0) noexcept {
        return http_multipart_delimiter_result(http_multipart_delimiter_need_input(offset));
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_part(
        std::size_t offset, std::size_t line_bytes) noexcept {
        return http_multipart_delimiter_result(http_multipart_part_delimiter(offset, line_bytes));
    }

    [[nodiscard]] static constexpr http_multipart_delimiter_result make_close(
        std::size_t offset, std::size_t line_bytes) noexcept {
        return http_multipart_delimiter_result(http_multipart_close_delimiter(offset, line_bytes));
    }

    [[nodiscard]] constexpr http_multipart_delimiter_result rebased(std::size_t base) const noexcept {
        if (const auto* need_input = this->need_input()) {
            return make_need_input(base + need_input->offset());
        }
        if (const auto* part = this->part()) {
            return make_part(base + part->offset(), part->line_bytes());
        }
        if (const auto* close = this->close()) {
            return make_close(base + close->offset(), close->line_bytes());
        }
        return make_no_match();
    }

    value_type value_;
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
    std::size_t* padding_cursor) noexcept {
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
            return http_multipart_delimiter_result::make_no_match();
        }
        close = true;
        cursor_value += 2;
    }

    if (padding_cursor != nullptr) {
        cursor_value = std::max(cursor_value, *padding_cursor);
    }
    while (cursor_value < input.size() && (input[cursor_value] == ' ' || input[cursor_value] == '\t')) {
        ++cursor_value;
    }
    if (padding_cursor != nullptr) {
        *padding_cursor = cursor_value;
    }
    if (cursor_value == input.size()) {
        if (close && input_finished) {
            return http_multipart_delimiter_result::make_close(0, cursor_value);
        }
        return input_finished ? http_multipart_delimiter_result::make_no_match()
                              : http_multipart_delimiter_result::make_need_input();
    }
    if (input[cursor_value] != '\r') {
        return http_multipart_delimiter_result::make_no_match();
    }
    if (cursor_value + 1 >= input.size()) {
        return input_finished ? http_multipart_delimiter_result::make_no_match()
                              : http_multipart_delimiter_result::make_need_input();
    }
    if (input[cursor_value + 1] != '\n') {
        return http_multipart_delimiter_result::make_no_match();
    }
    return close ? http_multipart_delimiter_result::make_close(0, cursor_value + 2)
                 : http_multipart_delimiter_result::make_part(0, cursor_value + 2);
}

// Cursors belong to one retained input prefix and must be reset after consume.
// Searches keep at most the three-byte delimiter prefix overlap; a candidate's
// transport padding resumes at its last inspected byte.
[[nodiscard]] inline http_multipart_delimiter_result http_find_initial_multipart_delimiter(
    std::string_view input, const multipart_boundary& boundary, bool input_finished,
    std::size_t* search_cursor, std::size_t* padding_cursor) noexcept {
    auto cursor_value = search_cursor == nullptr ? 0 : *search_cursor;
    if (cursor_value == 0 && !input.empty() && input.front() == '-') {
        auto match = http_match_multipart_delimiter_line(input, boundary, input_finished, padding_cursor);
        if (match.no_match() == nullptr) {
            return match;
        }
        if (padding_cursor != nullptr) {
            *padding_cursor = 0;
        }
    }
    for (auto prefix = input.find("\r\n--", cursor_value); prefix != std::string_view::npos;
        prefix = input.find("\r\n--", prefix + 1)) {
        auto match = http_match_multipart_delimiter_line(
            input.substr(prefix + 2), boundary, input_finished, padding_cursor);
        if (match.no_match() != nullptr) {
            if (padding_cursor != nullptr) {
                *padding_cursor = 0;
            }
            continue;
        }
        if (search_cursor != nullptr) {
            *search_cursor = prefix;
        }
        return match.rebased(prefix + 2);
    }
    if (search_cursor != nullptr) {
        *search_cursor = input.size() > 3 ? input.size() - 3 : 0;
    }
    // The CRLF belongs to the preamble, but the first marker byte may still
    // become a delimiter. Keep it out of the preamble quota until the next
    // byte resolves the candidate or EOF makes the prefix incomplete.
    if (!input_finished && input.ends_with("\r\n-")) {
        return http_multipart_delimiter_result::make_need_input(input.size() - 1);
    }
    return http_multipart_delimiter_result::make_no_match();
}

[[nodiscard]] inline http_multipart_delimiter_result http_find_multipart_body_delimiter(
    std::string_view input, const multipart_boundary& boundary, bool input_finished,
    std::size_t* search_cursor, std::size_t* padding_cursor) noexcept {
    const auto cursor_value = search_cursor == nullptr ? 0 : *search_cursor;
    for (auto prefix = input.find("\r\n--", cursor_value); prefix != std::string_view::npos;
        prefix = input.find("\r\n--", prefix + 1)) {
        auto match = http_match_multipart_delimiter_line(
            input.substr(prefix + 2), boundary, input_finished, padding_cursor);
        if (match.no_match() != nullptr) {
            if (padding_cursor != nullptr) {
                *padding_cursor = 0;
            }
            continue;
        }
        if (search_cursor != nullptr) {
            *search_cursor = prefix;
        }
        return match.rebased(prefix);
    }
    if (search_cursor != nullptr) {
        *search_cursor = input.size() > 3 ? input.size() - 3 : 0;
    }
    return http_multipart_delimiter_result::make_no_match();
}

}  // namespace ruvia::detail
