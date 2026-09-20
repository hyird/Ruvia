#pragma once

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/UrlEncoding.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/web/RequestFields.h"

// Primitives shared by everything that turns a delimited request field list into
// a parsed name/value vector -- the query string, the Cookie header and the form
// body all go through these: how many entries to reserve for untrusted input,
// how to decode a percent-encoded component in place, and the deterministic name
// order a lookup index is built from.

namespace ruvia::detail {

[[nodiscard]] inline std::size_t delimitedFieldCount(
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

// Cap on the up-front reservation for a parsed name/value vector. delimitedFieldCount
// counts every delimiter, including the empty segments that the parser then skips
// (visitUrlEncodedPairs / httpVisitSemicolonParameters), so an untrusted input of
// only delimiters -- e.g. a 16 MiB body of '&' at the buffered-body limit -- would
// reserve millions of heavy field objects while producing none, amplifying a small
// body into a huge allocation. Bound the reservation: growth past it is amortized
// O(1), so a legitimate large input is unaffected while the attacker-controlled
// over-reservation is capped.
inline constexpr std::size_t kMaxParsedFieldReserve = 4096;

[[nodiscard]] inline std::size_t boundedFieldReserve(std::size_t count) noexcept {
    return count < kMaxParsedFieldReserve ? count : kMaxParsedFieldReserve;
}

inline void appendLowerAscii(std::pmr::string& output, std::string_view input) {
    for (const char ch : input) {
        output.push_back(
            static_cast<char>(detail::httpAsciiToLower(static_cast<unsigned char>(ch))));
    }
}

[[nodiscard]] inline std::string_view storedStringView(const std::pmr::string& value) noexcept {
    return value;
}

// Unencoded components borrow `input`. Encoded components are owned in
// `storage` so later lookups do not re-decode. Failure is malformed percent
// encoding, not an absent value.
[[nodiscard]] inline std::optional<std::string_view> borrowOrDecode(
    std::pmr::vector<std::pmr::string>& storage, std::string_view input,
    detail::UrlDecodeMode mode) {
    if (!detail::hasUrlEncoding(input, mode)) {
        return input;
    }
    auto decoded = detail::decodeUrlComponent(
        input, {.mode = mode, .resource = storage.get_allocator().resource()});
    if (!decoded) {
        return std::nullopt;
    }
    return storedStringView(storage.emplace_back(std::move(*decoded)));
}

[[nodiscard]] inline std::pmr::vector<std::size_t> sortedFieldOrder(
    const RequestNameValueList& fields, std::pmr::memory_resource* resource) {
    std::pmr::vector<std::size_t> order(resource);
    order.reserve(fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i) {
        order.push_back(i);
    }
    std::ranges::sort(order, [&fields](std::size_t left, std::size_t right) noexcept {
        const auto leftName = fields[left].name();
        const auto rightName = fields[right].name();
        if (leftName == rightName) {
            return left < right;
        }
        return leftName < rightName;
    });
    return order;
}

}  // namespace ruvia::detail
