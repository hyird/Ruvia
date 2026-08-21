#pragma once

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/UrlEncoding.h"
#include "ruvia/http/detail/util/AsciiCase.h"

// Primitives shared by everything that turns a delimited request field list into
// a parsed name/value vector -- the query string, the Cookie header and the form
// body all go through these: how many entries to reserve for untrusted input,
// how to decode a percent-encoded component in place, and the deterministic name
// order a lookup index is built from.

namespace ruvia::detail {

[[nodiscard]] inline std::size_t delimitedFieldCount(std::string_view input, char delimiter) noexcept {
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
        output.push_back(static_cast<char>(detail::httpAsciiToLower(static_cast<unsigned char>(ch))));
    }
}

[[nodiscard]] inline bool assignUrlDecodedOrCopy(std::pmr::string& output, std::string_view input, detail::UrlDecodeMode mode) {
    if (detail::hasUrlEncoding(input, mode)) {
        auto decoded = detail::decodeUrlComponent(input, {.mode = mode, .resource = output.get_allocator().resource()});
        if (decoded.has_value()) {
            output = std::move(*decoded);
            return true;
        }
        return false;
    }
    output.assign(input.data(), input.size());
    return true;
}

[[nodiscard]] inline std::string_view storedStringView(const std::pmr::string& value) noexcept {
    return value;
}

[[nodiscard]] inline std::string_view pairNameAt(const std::pmr::vector<std::pmr::string>& storage, std::size_t index) noexcept {
    return storedStringView(storage[index * 2]);
}

[[nodiscard]] inline std::pmr::vector<std::size_t> sortedPairOrder(const std::pmr::vector<std::pmr::string>& storage, std::pmr::memory_resource* resource) {
    std::pmr::vector<std::size_t> order(resource);
    const auto count = storage.size() / 2;
    order.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        order.push_back(i);
    }
    // The original position is an explicit tie-breaker, so an in-place sort has
    // the same deterministic order as stable_sort without its non-PMR scratch
    // allocation on the request path.
    std::ranges::sort(order, [&storage](std::size_t left, std::size_t right) noexcept {
        const auto leftName = pairNameAt(storage, left);
        const auto rightName = pairNameAt(storage, right);
        if (leftName == rightName) {
            return left < right;
        }
        return leftName < rightName;
    });
    return order;
}

}  // namespace ruvia::detail
