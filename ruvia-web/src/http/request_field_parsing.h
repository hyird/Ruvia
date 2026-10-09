#pragma once

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/request_fields.h"

// Primitives shared by everything that turns a delimited request field list into
// a parsed name/value vector -- the query string, the Cookie header and the form
// body all go through these: how many entries to reserve for untrusted input,
// how to decode a percent-encoded component in place, and the deterministic name
// order a lookup index is built from.

namespace ruvia::detail {

[[nodiscard]] inline std::size_t delimited_field_count(
    std::string_view input, char delimiter) noexcept {
    if (input.empty()) {
        return 0;
    }

    std::size_t count = 1;
    for (const char c : input) {
        if (c == delimiter) {
            ++count;
        }
    }
    return count;
}

// Cap on the up-front reservation for a parsed name/value vector. delimited_field_count
// counts every delimiter, including the empty segments that the parser then skips
// (visit_url_encoded_pairs / http_visit_semicolon_parameters), so an untrusted input of
// only delimiters -- e.g. a 16 MiB body of '&' at the buffered-body limit -- would
// reserve millions of heavy field objects while producing none, amplifying a small
// body into a huge allocation. Bound the reservation: growth past it is amortized
// O(1), so a legitimate large input is unaffected while the attacker-controlled
// over-reservation is capped.
inline constexpr std::size_t max_parsed_field_reserve = 4096;

[[nodiscard]] inline std::size_t bounded_field_reserve(std::size_t count) noexcept {
    return count < max_parsed_field_reserve ? count : max_parsed_field_reserve;
}

inline void append_lower_ascii(std::pmr::string& output, std::string_view input) {
    for (const char ch : input) {
        output.push_back(
            static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch))));
    }
}

[[nodiscard]] inline std::string_view stored_string_view(const std::pmr::string& value) noexcept {
    return value;
}

// Unencoded components borrow `input`. Encoded components are owned in
// `storage` so later lookups do not re-decode. Failure is malformed percent
// encoding, not an absent value. The caller must reserve enough storage before
// publishing views so appending strings cannot relocate their inline buffers.
[[nodiscard]] inline std::optional<std::string_view> borrow_or_decode(
    std::pmr::vector<std::pmr::string>& storage, std::string_view input,
    url_decode_mode mode) {
    if (!ruvia::has_url_encoding(input, mode)) {
        return input;
    }
    auto decoded = ruvia::decode_url_component(
        input, {.mode_ = mode, .resource_ = storage.get_allocator().resource()});
    if (!decoded) {
        return std::nullopt;
    }
    return stored_string_view(storage.emplace_back(std::move(*decoded)));
}

[[nodiscard]] inline std::pmr::vector<std::size_t> sorted_field_order(
    const request_name_value_list& fields_value, std::pmr::memory_resource* resource) {
    std::pmr::vector<std::size_t> order(resource);
    order.reserve(fields_value.size());
    for (std::size_t i = 0; i < fields_value.size(); ++i) {
        order.push_back(i);
    }
    std::ranges::sort(order, [&fields_value](std::size_t left, std::size_t right) noexcept {
        const auto left_name = fields_value[left].name();
        const auto right_name = fields_value[right].name();
        if (left_name == right_name) {
            return left < right;
        }
        return left_name < right_name;
    });
    return order;
}

}  // namespace ruvia::detail
