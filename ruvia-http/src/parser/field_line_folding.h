#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

enum class field_line_folding_policy { mime,
    http };

[[nodiscard]] inline bool is_field_line_whitespace(char byte) noexcept {
    return byte == ' ' || byte == '\t';
}

// A fold always starts with CRLF followed by SP/HTAB. Leave bare CR/LF and
// ordinary field separators visible to the protocol's field validator.
[[nodiscard]] inline std::size_t find_field_line_fold(
    std::string_view input, std::size_t start = 0) noexcept {
    auto position = input.find("\r\n", start);
    while (position != std::string_view::npos) {
        if (position + 2 < input.size() && is_field_line_whitespace(input[position + 2])) {
            return position;
        }
        position = input.find("\r\n", position + 2);
    }
    return std::string_view::npos;
}

// first_fold comes from find_field_line_fold. The caller owns storage through
// every use of the resulting views; input must not borrow that storage.
// MIME removes only CRLF. HTTP replaces the complete obs-fold, including its
// adjacent OWS/RWS, with SP before field values are interpreted (RFC 9112 5.2).
inline void copy_unfolded_field_lines(std::string_view input, std::size_t first_fold,
    field_line_folding_policy policy, std::pmr::string& storage) {
    storage.clear();
    storage.reserve(input.size());
    std::size_t segment_begin = 0;
    for (auto position = first_fold; position != std::string_view::npos;) {
        auto segment_end = position;
        auto next_segment = position + 2;
        if (policy == field_line_folding_policy::http) {
            while (segment_end > segment_begin && is_field_line_whitespace(input[segment_end - 1])) {
                --segment_end;
            }
            while (next_segment < input.size() && is_field_line_whitespace(input[next_segment])) {
                ++next_segment;
            }
        }
        storage.append(input.substr(segment_begin, segment_end - segment_begin));
        if (policy == field_line_folding_policy::http) {
            storage.push_back(' ');
        }
        segment_begin = next_segment;
        position = find_field_line_fold(input, next_segment);
    }
    storage.append(input.substr(segment_begin));
}

}  // namespace ruvia::detail
