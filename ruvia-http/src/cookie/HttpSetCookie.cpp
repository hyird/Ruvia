#include "ruvia/http/HttpSetCookie.h"

#include <array>
#include <charconv>
#include <limits>

#include "ruvia/http/detail/cookie/CookieValidation.h"
#include "ruvia/http/detail/field/HttpDate.h"
#include "ruvia/http/detail/util/AsciiCase.h"

namespace ruvia {
namespace {

constexpr std::size_t kMaxReceivedCookieBytes = 4096;
constexpr std::size_t kMaxReceivedCookieAttributeBytes = 1024;

std::string_view trimOws(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

std::pair<std::string_view, std::string_view> splitAttribute(std::string_view value) noexcept {
    const auto equals = value.find('=');
    if (equals == std::string_view::npos) return {trimOws(value), {}};
    return {trimOws(value.substr(0, equals)), trimOws(value.substr(equals + 1))};
}

std::optional<std::int64_t> parseMaxAgeSeconds(std::string_view value) noexcept {
    if (value.empty()) return std::nullopt;
    const bool negative = value.front() == '-';
    const auto digitOffset = negative ? std::size_t{1} : std::size_t{0};
    if (digitOffset == value.size()) return std::nullopt;
    for (const char byte : value.substr(digitOffset)) {
        if (byte < '0' || byte > '9') return std::nullopt;
    }

    std::int64_t seconds = 0;
    const auto [parsed, error] = std::from_chars(
        value.data(), value.data() + value.size(), seconds);
    if (error == std::errc::result_out_of_range) {
        return negative
            ? std::numeric_limits<std::int64_t>::min()
            : detail::kMaxCookieAgeSeconds;
    }
    if (error != std::errc{} || parsed != value.data() + value.size()) return std::nullopt;
    return seconds > detail::kMaxCookieAgeSeconds
        ? detail::kMaxCookieAgeSeconds
        : seconds;
}

bool containsRejectedReceivedCookieControl(std::string_view value) noexcept {
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte <= 0x08 || (byte >= 0x0a && byte <= 0x1f) || byte == 0x7f) {
            return true;
        }
    }
    return false;
}

bool isCookieDateDelimiter(unsigned char byte) noexcept {
    return byte == 0x09 || (byte >= 0x20 && byte <= 0x2f) ||
        (byte >= 0x3b && byte <= 0x40) ||
        (byte >= 0x5b && byte <= 0x60) ||
        (byte >= 0x7b && byte <= 0x7e);
}

std::optional<int> parseCookieDateDigits(
    std::string_view value,
    std::size_t minimumDigits,
    std::size_t maximumDigits) noexcept {
    if (value.size() < minimumDigits || value.size() > maximumDigits) return std::nullopt;
    int result = 0;
    for (const char character : value) {
        if (character < '0' || character > '9') return std::nullopt;
        result = result * 10 + (character - '0');
    }
    return result;
}

struct CookieDateTime final {
    int hour;
    int minute;
    int second;
};

std::optional<CookieDateTime> parseCookieDateTime(std::string_view value) noexcept {
    const auto firstColon = value.find(':');
    if (firstColon == std::string_view::npos) return std::nullopt;
    const auto secondColon = value.find(':', firstColon + 1);
    if (secondColon == std::string_view::npos ||
        value.find(':', secondColon + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto hour = parseCookieDateDigits(value.substr(0, firstColon), 1, 2);
    const auto minute = parseCookieDateDigits(
        value.substr(firstColon + 1, secondColon - firstColon - 1), 1, 2);
    const auto second = parseCookieDateDigits(value.substr(secondColon + 1), 1, 2);
    if (!hour || !minute || !second) return std::nullopt;
    return CookieDateTime{*hour, *minute, *second};
}

std::optional<int> parseCookieDateMonth(std::string_view value) noexcept {
    constexpr std::array<std::string_view, 12> months{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (std::size_t index = 0; index < months.size(); ++index) {
        if (detail::httpAsciiEqualsIgnoreCase(value, months[index])) {
            return static_cast<int>(index + 1);
        }
    }
    return std::nullopt;
}

std::optional<std::time_t> parseCookieDate(std::string_view value) noexcept {
    std::optional<CookieDateTime> time;
    std::optional<int> day;
    std::optional<int> month;
    std::optional<int> year;

    std::size_t cursor = 0;
    while (cursor < value.size()) {
        while (cursor < value.size() &&
            isCookieDateDelimiter(static_cast<unsigned char>(value[cursor]))) {
            ++cursor;
        }
        const auto begin = cursor;
        while (cursor < value.size() &&
            !isCookieDateDelimiter(static_cast<unsigned char>(value[cursor]))) {
            ++cursor;
        }
        if (begin == cursor) continue;
        const auto token = value.substr(begin, cursor - begin);
        if (!time) {
            time = parseCookieDateTime(token);
            if (time) continue;
        }
        if (!day) {
            day = parseCookieDateDigits(token, 1, 2);
            if (day) continue;
        }
        if (!month) {
            month = parseCookieDateMonth(token);
            if (month) continue;
        }
        if (!year) year = parseCookieDateDigits(token, 2, 4);
    }

    if (!time || !day || !month || !year) return std::nullopt;
    if (*year >= 70 && *year <= 99) *year += 1900;
    else if (*year <= 69) *year += 2000;
    if (*year < 1601 || *day < 1 || *day > 31 ||
        time->hour > 23 || time->minute > 59 || time->second > 59) {
        return std::nullopt;
    }
    return detail::httpCivilToTimeT(
        *year, *month, *day, time->hour, time->minute, time->second);
}

}  // namespace

std::optional<HttpSetCookieView> parseSetCookie(std::string_view value) noexcept {
    const auto firstEnd = value.find(';');
    const auto cookiePair = value.substr(0, firstEnd);
    std::string_view name;
    std::string_view cookieValue;
    if (cookiePair.find('=') == std::string_view::npos) {
        cookieValue = trimOws(cookiePair);
    } else {
        const auto fields = splitAttribute(cookiePair);
        name = fields.first;
        cookieValue = fields.second;
    }
    if ((name.empty() && cookieValue.empty()) ||
        name.size() > kMaxReceivedCookieBytes ||
        cookieValue.size() > kMaxReceivedCookieBytes - name.size() ||
        containsRejectedReceivedCookieControl(name) ||
        containsRejectedReceivedCookieControl(cookieValue)) {
        return std::nullopt;
    }

    HttpSetCookieView result{
        .name = name,
        .value = cookieValue,
        .path = {},
        .domain = {},
        .expires = std::nullopt,
        .maxAgeSeconds = std::nullopt,
        .secure = false,
        .hasPathAttribute = false,
        .sameSiteNone = false,
    };
    std::size_t offset = firstEnd == std::string_view::npos ? value.size() : firstEnd + 1;
    while (offset < value.size()) {
        const auto end = value.find(';', offset);
        const auto [attribute, argument] = splitAttribute(value.substr(offset, end == std::string_view::npos ? value.size() - offset : end - offset));
        if (argument.size() > kMaxReceivedCookieAttributeBytes) {
            // Ignore this cookie-av and continue with later attributes.
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "Path")) {
            result.path = argument;
            result.hasPathAttribute = true;
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "Domain")) {
            auto domain = argument;
            if (!domain.empty() && domain.front() == '.') domain.remove_prefix(1);
            if (!domain.empty() && !detail::isValidCookieDomain(domain)) return std::nullopt;
            result.domain = domain;
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "Expires")) {
            if (const auto expires = parseCookieDate(argument)) {
                result.expires = expires;
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "Max-Age")) {
            if (const auto seconds = parseMaxAgeSeconds(argument)) {
                result.maxAgeSeconds = seconds;
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "SameSite")) {
            result.sameSiteNone = detail::httpAsciiEqualsIgnoreCase(argument, "None");
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "Secure")) {
            result.secure = true;
        }
        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return result;
}

}  // namespace ruvia
