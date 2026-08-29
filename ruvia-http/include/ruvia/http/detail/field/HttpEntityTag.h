#pragma once

#include <string_view>

#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia::detail {

[[nodiscard]] inline std::string_view httpTrimWeakEtagPrefix(std::string_view value) noexcept {
    if (value.size() >= 2 && value[0] == 'W' && value[1] == '/') {
        value.remove_prefix(2);
    }
    return value;
}

template <HttpTemporaryOwningCharString Value>
std::string_view httpTrimWeakEtagPrefix(Value&&) = delete;

[[nodiscard]] inline bool httpIsWeakEtag(std::string_view value) noexcept {
    return value.size() >= 2 && value[0] == 'W' && value[1] == '/';
}

[[nodiscard]] inline bool httpStrongEtagEquals(
    std::string_view left, std::string_view right) noexcept {
    return !httpIsWeakEtag(left) && !httpIsWeakEtag(right) && left == right;
}

[[nodiscard]] inline bool httpWeakEtagEquals(
    std::string_view left, std::string_view right) noexcept {
    return httpTrimWeakEtagPrefix(left) == httpTrimWeakEtagPrefix(right);
}

struct HttpEtagListMatchResult final {
    bool valid;
    bool matched;
};

[[nodiscard]] inline HttpEtagListMatchResult httpParseEtagListMatches(
    std::string_view values, std::string_view expected, bool strong) noexcept {
    bool matched = false;
    std::size_t offset = 0;

    while (offset < values.size()) {
        while (offset < values.size() &&
               (values[offset] == ' ' || values[offset] == '\t' || values[offset] == ',')) {
            ++offset;
        }
        if (offset == values.size()) {
            break;
        }

        const std::size_t begin = offset;
        if (values.substr(offset).starts_with("W/")) {
            offset += 2;
        }
        if (offset == values.size() || values[offset] != '"') {
            return {false, false};
        }
        ++offset;
        while (offset < values.size() && values[offset] != '"') {
            const auto byte = static_cast<unsigned char>(values[offset]);
            if (byte != 0x21 && !(byte >= 0x23 && byte <= 0x7e) && byte < 0x80) {
                return {false, false};
            }
            ++offset;
        }
        if (offset == values.size()) {
            return {false, false};
        }
        ++offset;
        const auto entityTag = values.substr(begin, offset - begin);
        matched = matched || (strong ? httpStrongEtagEquals(entityTag, expected)
                                     : httpWeakEtagEquals(entityTag, expected));

        while (offset < values.size() && (values[offset] == ' ' || values[offset] == '\t')) {
            ++offset;
        }
        if (offset < values.size() && values[offset] != ',') {
            return {false, false};
        }
    }
    return {true, matched};
}

[[nodiscard]] inline bool httpEtagListMatches(
    std::string_view values, std::string_view expected, bool strong) noexcept {
    const auto result = httpParseEtagListMatches(values, expected, strong);
    return result.valid && result.matched;
}

}  // namespace ruvia::detail
