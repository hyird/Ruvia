#include "ruvia/http/http_set_cookie.h"

#include <array>
#include <charconv>
#include <limits>
#include <string_view>

#include "ruvia/http/detail/util/ascii_case.h"

#include "cookie/cookie_validation.h"
#include "field/http_date.h"

namespace ruvia {
namespace {

constexpr std::size_t max_received_cookie_bytes = 4096;
constexpr std::size_t max_received_cookie_attribute_bytes = 1024;

std::string_view trim_ows(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

std::pair<std::string_view, std::string_view> split_attribute(std::string_view value) noexcept {
    const auto equals = value.find('=');
    if (equals == std::string_view::npos) {
        return {trim_ows(value), {}};
    }
    return {trim_ows(value.substr(0, equals)), trim_ows(value.substr(equals + 1))};
}

std::optional<std::int64_t> parse_max_age_seconds(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    const bool negative = value.front() == '-';
    const auto digit_offset = negative ? std::size_t{1} : std::size_t{0};
    if (digit_offset == value.size()) {
        return std::nullopt;
    }
    for (const char byte : value.substr(digit_offset)) {
        if (byte < '0' || byte > '9') {
            return std::nullopt;
        }
    }

    std::int64_t seconds = 0;
    const auto [parsed_value, error] =
        std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (error == std::errc::result_out_of_range) {
        return negative ? std::numeric_limits<std::int64_t>::min() : max_cookie_age_seconds;
    }
    if (error != std::errc{} || parsed_value != value.data() + value.size()) {
        return std::nullopt;
    }
    return seconds > max_cookie_age_seconds ? max_cookie_age_seconds : seconds;
}

bool contains_rejected_received_cookie_control(std::string_view value) noexcept {
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte <= 0x08 || (byte >= 0x0a && byte <= 0x1f) || byte == 0x7f) {
            return true;
        }
    }
    return false;
}

bool is_cookie_date_delimiter(unsigned char byte) noexcept {
    return byte == 0x09 || (byte >= 0x20 && byte <= 0x2f) || (byte >= 0x3b && byte <= 0x40) ||
           (byte >= 0x5b && byte <= 0x60) || (byte >= 0x7b && byte <= 0x7e);
}

// Consume only on success. Any remaining suffix starts with a non-digit,
// as required by RFC 6265 section 5.1.1 and verified erratum 4148.
std::optional<int> consume_cookie_date_digits(
    std::string_view& value, std::size_t minimum_digits, std::size_t maximum_digits) noexcept {
    std::size_t digits = 0;
    int result_value = 0;
    while (digits < value.size() && value[digits] >= '0' && value[digits] <= '9') {
        if (digits == maximum_digits) {
            return std::nullopt;
        }
        result_value = result_value * 10 + (value[digits] - '0');
        ++digits;
    }
    if (digits < minimum_digits) {
        return std::nullopt;
    }
    value.remove_prefix(digits);
    return result_value;
}

struct cookie_date_time final {
    int hour_;
    int minute_;
    int second_;
};

std::optional<cookie_date_time> parse_cookie_date_time(std::string_view value) noexcept {
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
    return cookie_date_time{*hour, *minute, *second};
}

std::optional<int> parse_cookie_date_month(std::string_view value) noexcept {
    if (value.size() < 3) {
        return std::nullopt;
    }
    value = value.substr(0, 3);
    constexpr std::array<std::string_view, 12> months{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (std::size_t index = 0; index < months.size(); ++index) {
        if (detail::http_ascii_equals_ignore_case(value, months[index])) {
            return static_cast<int>(index + 1);
        }
    }
    return std::nullopt;
}

std::optional<std::time_t> parse_cookie_date(std::string_view value) noexcept {
    std::optional<cookie_date_time> time;
    std::optional<int> day;
    std::optional<int> month;
    std::optional<int> year;

    std::size_t cursor_value = 0;
    while (cursor_value < value.size()) {
        while (cursor_value < value.size() &&
               is_cookie_date_delimiter(static_cast<unsigned char>(value[cursor_value]))) {
            ++cursor_value;
        }
        const auto begin = cursor_value;
        while (cursor_value < value.size() &&
               !is_cookie_date_delimiter(static_cast<unsigned char>(value[cursor_value]))) {
            ++cursor_value;
        }
        if (begin == cursor_value) {
            continue;
        }
        auto token = value.substr(begin, cursor_value - begin);
        if (!time) {
            time = parse_cookie_date_time(token);
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
            month = parse_cookie_date_month(token);
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
    if (*year < 1601 || *day < 1 || *day > 31 || time->hour_ > 23 || time->minute_ > 59 ||
        time->second_ > 59) {
        return std::nullopt;
    }
    return detail::http_civil_to_time_t(*year, *month, *day, time->hour_, time->minute_, time->second_);
}

}  // namespace

std::optional<http_set_cookie_view> parse_set_cookie(std::string_view value) noexcept {
    const auto first_end = value.find(';');
    const auto cookie_pair = value.substr(0, first_end);
    std::string_view name;
    std::string_view cookie_value;
    if (!(cookie_pair.find('=') != std::string_view::npos)) {
        cookie_value = trim_ows(cookie_pair);
    } else {
        const auto fields_value = split_attribute(cookie_pair);
        name = fields_value.first;
        cookie_value = fields_value.second;
    }
    if ((name.empty() && cookie_value.empty()) || name.size() > max_received_cookie_bytes ||
        cookie_value.size() > max_received_cookie_bytes - name.size() ||
        contains_rejected_received_cookie_control(name) ||
        contains_rejected_received_cookie_control(cookie_value)) {
        return std::nullopt;
    }

    http_set_cookie_view result(name, cookie_value);
    std::size_t offset = first_end == std::string_view::npos ? value.size() : first_end + 1;
    while (offset < value.size()) {
        const auto end = value.find(';', offset);
        const auto [attribute, argument] = split_attribute(value.substr(
            offset, end == std::string_view::npos ? value.size() - offset : end - offset));
        if (argument.size() > max_received_cookie_attribute_bytes) {
            // Ignore this cookie-av and continue with later attributes.
        } else if (detail::http_ascii_equals_ignore_case(attribute, "Path")) {
            if (!detail::is_valid_cookie_attribute(argument)) {
                return std::nullopt;
            }
            result.path_ = argument;
            result.set(http_set_cookie_attribute::path);
        } else if (detail::http_ascii_equals_ignore_case(attribute, "Domain")) {
            auto domain = argument;
            if (!domain.empty() && domain.front() == '.') {
                domain.remove_prefix(1);
            }
            if (!domain.empty() && !detail::is_valid_cookie_domain(domain)) {
                return std::nullopt;
            }
            result.domain_ = domain;
        } else if (detail::http_ascii_equals_ignore_case(attribute, "Expires")) {
            if (const auto expires = parse_cookie_date(argument)) {
                result.expires_ = expires;
            }
        } else if (detail::http_ascii_equals_ignore_case(attribute, "Max-Age")) {
            if (const auto seconds = parse_max_age_seconds(argument)) {
                result.max_age_seconds_ = seconds;
            }
        } else if (detail::http_ascii_equals_ignore_case(attribute, "SameSite")) {
            if (detail::http_ascii_equals_ignore_case(argument, "None")) {
                result.set(http_set_cookie_attribute::same_site_none);
            } else {
                result.clear(http_set_cookie_attribute::same_site_none);
            }
        } else if (detail::http_ascii_equals_ignore_case(attribute, "Secure")) {
            result.set(http_set_cookie_attribute::secure);
        }
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1;
    }
    return result;
}

}  // namespace ruvia
