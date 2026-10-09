#include "ruvia/http/HttpSetCookie.h"

#include <array>
#include <charconv>
#include <limits>
#include <string_view>

#include "ruvia/http/detail/util/AsciiCase.h"

#include "cookie/CookieValidation.h"
#include "field/HttpDate.h"

namespace ruvia {
namespace {

constexpr std::size_t kMaxReceivedCookieBytes = 4096;
constexpr std::size_t kMaxReceivedCookieAttributeBytes = 1024;

std::string_view trimOws(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

std::pair<std::string_view, std::string_view> splitAttribute(std::string_view value) noexcept {
    const auto equals = value.find('=');
    if (equals == std::string_view::npos) {
        return {trimOws(value), {}};
    }
    return {trimOws(value.substr(0, equals)), trimOws(value.substr(equals + 1))};
}

std::optional<std::int64_t> parseMaxAgeSeconds(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    const bool negative = value.front() == '-';
    const auto digitOffset = negative ? std::size_t{1} : std::size_t{0};
    if (digitOffset == value.size()) {
        return std::nullopt;
    }
    for (const char byte : value.substr(digitOffset)) {
        if (byte < '0' || byte > '9') {
            return std::nullopt;
        }
    }

    std::int64_t seconds = 0;
    const auto [parsed, error] =
        std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (error == std::errc::result_out_of_range) {
        return negative ? std::numeric_limits<std::int64_t>::min() : kMaxCookieAgeSeconds;
    }
    if (error != std::errc{} || parsed != value.data() + value.size()) {
        return std::nullopt;
    }
    return seconds > kMaxCookieAgeSeconds ? kMaxCookieAgeSeconds : seconds;
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
    return byte == 0x09 || (byte >= 0x20 && byte <= 0x2f) || (byte >= 0x3b && byte <= 0x40) ||
           (byte >= 0x5b && byte <= 0x60) || (byte >= 0x7b && byte <= 0x7e);
}

// Consume only on success. Any remaining suffix starts with a non-digit,
// as required by RFC 6265 section 5.1.1 and verified erratum 4148.
std::optional<int> consume_cookie_date_digits(
    std::string_view& value, std::size_t minimum_digits, std::size_t maximum_digits) noexcept {
    std::size_t digits = 0;
    int result = 0;
    while (digits < value.size() && value[digits] >= '0' && value[digits] <= '9') {
        if (digits == maximum_digits) {
            return std::nullopt;
        }
        result = result * 10 + (value[digits] - '0');
        ++digits;
    }
    if (digits < minimum_digits) {
        return std::nullopt;
    }
    value.remove_prefix(digits);
    return result;
}

struct CookieDateTime final {
    int hour;
    int minute;
    int second;
};

std::optional<CookieDateTime> parseCookieDateTime(std::string_view value) noexcept {
    const auto hour = consume_cookie_date_digits(value, 1, 2);
    if (!hour || value.empty() || value.front() != ':') {
        return std::nullopt;
    }
    value.remove_prefix(1);
    const auto minute = consume_cookie_date_digits(value, 1, 2);
    if (!minute || value.empty() || value.front() != ':') {
        return std::nullopt;
    }
    value.remove_prefix(1);
    const auto second = consume_cookie_date_digits(value, 1, 2);
    if (!second) {
        return std::nullopt;
    }
    return CookieDateTime{*hour, *minute, *second};
}

std::optional<int> parseCookieDateMonth(std::string_view value) noexcept {
    if (value.size() < 3) {
        return std::nullopt;
    }
    value = value.substr(0, 3);
    constexpr std::array<std::string_view, 12> months{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
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
        if (begin == cursor) {
            continue;
        }
        auto token = value.substr(begin, cursor - begin);
        if (!time) {
            time = parseCookieDateTime(token);
            if (time) {
                continue;
            }
        }
        if (!day) {
            day = consume_cookie_date_digits(token, 1, 2);
            if (day) {
                continue;
            }
        }
        if (!month) {
            month = parseCookieDateMonth(token);
            if (month) {
                continue;
            }
        }
        if (!year) {
            year = consume_cookie_date_digits(token, 2, 4);
        }
    }

    if (!time || !day || !month || !year) {
        return std::nullopt;
    }
    if (*year >= 70 && *year <= 99) {
        *year += 1900;
    } else if (*year <= 69) {
        *year += 2000;
    }
    if (*year < 1601 || *day < 1 || *day > 31 || time->hour > 23 || time->minute > 59 ||
        time->second > 59) {
        return std::nullopt;
    }
    return detail::httpCivilToTimeT(*year, *month, *day, time->hour, time->minute, time->second);
}

}  // namespace

std::optional<HttpSetCookieView> parseSetCookie(std::string_view value) noexcept {
    const auto firstEnd = value.find(';');
    const auto cookiePair = value.substr(0, firstEnd);
    std::string_view name;
    std::string_view cookieValue;
    if (!(cookiePair.find('=') != std::string_view::npos)) {
        cookieValue = trimOws(cookiePair);
    } else {
        const auto fields = splitAttribute(cookiePair);
        name = fields.first;
        cookieValue = fields.second;
    }
    if ((name.empty() && cookieValue.empty()) || name.size() > kMaxReceivedCookieBytes ||
        cookieValue.size() > kMaxReceivedCookieBytes - name.size() ||
        containsRejectedReceivedCookieControl(name) ||
        containsRejectedReceivedCookieControl(cookieValue)) {
        return std::nullopt;
    }

    HttpSetCookieView result(name, cookieValue);
    std::size_t offset = firstEnd == std::string_view::npos ? value.size() : firstEnd + 1;
    while (offset < value.size()) {
        const auto end = value.find(';', offset);
        const auto [attribute, argument] = splitAttribute(value.substr(
            offset, end == std::string_view::npos ? value.size() - offset : end - offset));
        if (argument.size() > kMaxReceivedCookieAttributeBytes) {
            // Ignore this cookie-av and continue with later attributes.
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "Path")) {
            if (!detail::isValidCookieAttribute(argument)) {
                return std::nullopt;
            }
            result.path_ = argument;
            result.set(HttpSetCookieAttribute::kPath);
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "Domain")) {
            auto domain = argument;
            if (!domain.empty() && domain.front() == '.') {
                domain.remove_prefix(1);
            }
            if (!domain.empty() && !detail::isValidCookieDomain(domain)) {
                return std::nullopt;
            }
            result.domain_ = domain;
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "Expires")) {
            if (const auto expires = parseCookieDate(argument)) {
                result.expires_ = expires;
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "Max-Age")) {
            if (const auto seconds = parseMaxAgeSeconds(argument)) {
                result.maxAgeSeconds_ = seconds;
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "SameSite")) {
            if (detail::httpAsciiEqualsIgnoreCase(argument, "None")) {
                result.set(HttpSetCookieAttribute::kSameSiteNone);
            } else {
                result.clear(HttpSetCookieAttribute::kSameSiteNone);
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(attribute, "Secure")) {
            result.set(HttpSetCookieAttribute::kSecure);
        }
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1;
    }
    return result;
}

}  // namespace ruvia
