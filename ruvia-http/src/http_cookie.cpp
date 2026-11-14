#include "http_cookie.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "decimal_text.h"
#include "http_date.h"
#include "http_date_parts.h"
#include "http_field_text.h"
#include "http_header.h"
#include "http_request.h"
#include "http_uri_syntax.h"
#include "http_wire_writer.h"

namespace ruvia::detail {

// RFC 6265bis section 5.6 storage limits. The sender rejects anything the
// receiving parser would truncate or drop, so set_cookie can find and replace
// cookies it emitted earlier by re-parsing them.
inline constexpr std::size_t max_cookie_name_value_bytes = 4096;
inline constexpr std::size_t max_cookie_attribute_value_bytes = 1024;

[[nodiscard]] inline bool is_valid_cookie_attribute(std::string_view value) noexcept {
    // RFC 6265bis path-value is *av-octet: ASCII %x20-3A / %x3C-7E.
    // HTTP field values can carry HTAB and obs-text, but emitting either inside
    // Path produces a Set-Cookie value outside the server grammar.
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7e || c == ';') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool is_valid_cookie_domain(std::string_view value) noexcept {
    // RFC 6265bis domain-value is an RFC 1034 subdomain as relaxed by RFC 1123:
    // ASCII LDH labels, each at most 63 octets, with a letter or digit at both
    // ends. The root separators consume the remaining two octets of DNS's
    // 255-octet wire limit, so an unqualified textual name is at most 253.
    return value.empty() || is_valid_dns_domain_name(value);
}

[[nodiscard]] inline std::string_view cookie_priority_token(cookie_priority priority) noexcept {
    switch (priority) {
        case cookie_priority::low:
            return "Low";
        case cookie_priority::medium:
            return "Medium";
        case cookie_priority::high:
            return "High";
    }
    return {};
}

[[nodiscard]] inline std::string_view cookie_same_site_token(cookie_same_site same_site) noexcept {
    switch (same_site) {
        case cookie_same_site::strict:
            return "Strict";
        case cookie_same_site::lax:
            return "Lax";
        case cookie_same_site::none:
            return "None";
    }
    return {};
}

[[nodiscard]] inline bool cookie_attribute_emitted(cookie_attribute_policy policy) {
    switch (policy) {
        case cookie_attribute_policy::omit:
            return false;
        case cookie_attribute_policy::emit:
            return true;
    }
    throw std::invalid_argument("invalid cookie attribute policy");
}

// `secure` and `partitioned` are resolved attribute policies; the plan also
// checks Max-Age before converting it to unsigned storage.
inline void validate_cookie(std::string_view name, std::string_view value, const cookie_options& options, bool secure, bool partitioned) {
    if (!is_valid_http_header_name(name)) {
        throw std::invalid_argument("invalid cookie name");
    }
    if (!::ruvia::is_valid_cookie_value(value)) {
        throw std::invalid_argument("invalid cookie value");
    }
    const auto prefix_size = options.prefix_ ? ::ruvia::http_cookie_prefix_text(*options.prefix_).size() : 0;
    if (name.size() > max_cookie_name_value_bytes - prefix_size || value.size() > max_cookie_name_value_bytes - prefix_size - name.size()) {
        throw std::invalid_argument("cookie name and value exceed 4096 bytes");
    }
    // Receivers trim OWS around Path, so edge spaces would not round-trip and
    // trailing SP would end the Set-Cookie field value in whitespace.
    const std::string_view path = options.path_;
    if (!is_valid_cookie_attribute(path) || path.size() > max_cookie_attribute_value_bytes || http_trim_ows(path).size() != path.size()) {
        throw std::invalid_argument("invalid cookie attribute");
    }
    if (!is_valid_cookie_domain(options.domain_)) {
        throw std::invalid_argument("invalid cookie domain");
    }
    if (options.expires_.has_value()) {
        using namespace std::chrono;
        constexpr auto minimum = duration_cast<seconds>(sys_days{year{1601} / January / 1}.time_since_epoch());
        const auto expires = floor<seconds>(options.expires_->time_since_epoch());
        if (expires < minimum) {
            throw std::invalid_argument("cookie Expires must not precede year 1601");
        }
    }
    if (options.priority_ && cookie_priority_token(*options.priority_).empty()) {
        throw std::invalid_argument("invalid cookie Priority");
    }
    if (options.same_site_ && cookie_same_site_token(*options.same_site_).empty()) {
        throw std::invalid_argument("invalid cookie SameSite");
    }
    if (options.same_site_ == cookie_same_site::none && !secure) {
        throw std::invalid_argument("SameSite=None cookie requires Secure");
    }
    if (options.prefix_ && ::ruvia::http_cookie_prefix_text(*options.prefix_).empty()) {
        throw std::invalid_argument("invalid cookie prefix");
    }
    if (partitioned && !secure) {
        throw std::invalid_argument("partitioned cookie requires Secure");
    }
    // User agents apply __Host-/__Secure- constraints case-insensitively. Mirror
    // that receive-side rule for literal wire names so every cookie emitted by
    // this sender can actually be stored. A typed prefix is canonical and is the
    // outermost wire prefix, so it takes precedence over bytes in `name`.
    const auto prefix = options.prefix_ ? options.prefix_ : ::ruvia::cookie_name_prefix(name);
    if (prefix && !::ruvia::cookie_prefix_requirements_met(*prefix, secure, options.path_, options.domain_)) {
        throw std::invalid_argument(*prefix == cookie_prefix::host ? "__Host- cookie requires Secure, Path=/, and no Domain" : "__Secure- cookie requires Secure");
    }
}

}  // namespace ruvia::detail

namespace ruvia {

set_cookie_plan::set_cookie_plan(std::string_view name, std::string_view value, const cookie_options& options) :
    name_(name),
    value_(value),
    path_(options.path_),
    domain_(options.domain_),
    prefix_text_(options.prefix_ ? http_cookie_prefix_text(*options.prefix_) : std::string_view{}),
    priority_text_(options.priority_ ? detail::cookie_priority_token(*options.priority_) : std::string_view{}),
    same_site_text_(options.same_site_ ? detail::cookie_same_site_token(*options.same_site_) : std::string_view{}),
    http_only_(detail::cookie_attribute_emitted(options.http_only_)),
    secure_(detail::cookie_attribute_emitted(options.secure_)),
    partitioned_(detail::cookie_attribute_emitted(options.partitioned_)) {
    // max-age-av starts with a non-zero digit (RFC 6265 section 4.1.1);
    // an immediate expiry is written as the past deletion Expires instead.
    const bool expire_now = options.max_age_.has_value() && options.max_age_->count() == 0;
    if (expire_now || options.expires_.has_value()) {
        const auto expires = expire_now ? cookie_deletion_expires : *options.expires_;
        const auto seconds = std::chrono::floor<std::chrono::seconds>(expires.time_since_epoch()).count();
        if (!std::in_range<std::time_t>(seconds)) {
            throw std::invalid_argument("cookie Expires is not representable");
        }
        const auto date = format_http_date(static_cast<std::time_t>(seconds));
        if (!date) {
            throw std::invalid_argument("cookie Expires is not representable");
        }
        expires_size_ = date->size();
        std::memcpy(expires_buffer_.data(), date->data(), expires_size_);
    }
    if (options.max_age_.has_value() && options.max_age_->count() != 0) {
        if (options.max_age_->count() < 0) {
            throw std::invalid_argument("cookie Max-Age must not be negative");
        }
        max_age_value_ = static_cast<std::uint64_t>(options.max_age_->count());
    }

    const auto add_size = [this](std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - size_) {
            throw std::length_error("Set-Cookie value is too large");
        }
        size_ += amount;
    };

    // Compute the complete wire length before validating the borrowed views.
    // This keeps a hostile oversized view from reaching a grammar scan and
    // prevents the later uninitialized response-header write from receiving a
    // wrapped length.
    add_size(prefix_text_.size());
    add_size(name_.size());
    add_size(1);
    add_size(value_.size());
    if (!path_.empty()) {
        add_size(std::string_view("; Path=").size());
        add_size(path_.size());
    }
    if (!domain_.empty()) {
        add_size(std::string_view("; Domain=").size());
        add_size(domain_.size());
    }
    if (max_age_value_ != 0) {
        add_size(std::string_view("; Max-Age=").size());
        add_size(http_unsigned_decimal_size(max_age_value_));
    }
    if (expires_size_ != 0) {
        add_size(std::string_view("; Expires=").size());
        add_size(expires_size_);
    }
    if (http_only_) {
        add_size(std::string_view("; HttpOnly").size());
    }
    if (secure_) {
        add_size(std::string_view("; Secure").size());
    }
    if (!same_site_text_.empty()) {
        add_size(std::string_view("; SameSite=").size());
        add_size(same_site_text_.size());
    }
    if (!priority_text_.empty()) {
        add_size(std::string_view("; Priority=").size());
        add_size(priority_text_.size());
    }
    if (partitioned_) {
        add_size(std::string_view("; Partitioned").size());
    }

    detail::validate_cookie(name, value, options, secure_, partitioned_);
}

char* write_cookie_request_pair(char* out, std::string_view name, std::string_view value) {
    const std::string_view written(out, cookie_request_pair_size(name, value));
    const bool name_overlaps = detail::http_text_overlaps(name, written);
    const bool value_overlaps = detail::http_text_overlaps(value, written);
    if (name_overlaps || value_overlaps) {
        // Snapshot only bytes that writing the pair could overwrite.
        const auto name_bytes = name_overlaps ? name.size() : 0;
        const auto value_bytes = value_overlaps ? value.size() : 0;
        std::array<char, 256> stack_snapshot;
        if (value_bytes <= stack_snapshot.size() && name_bytes <= stack_snapshot.size() - value_bytes) {
            if (name_overlaps) {
                std::memcpy(stack_snapshot.data(), name.data(), name_bytes);
            }
            if (value_overlaps) {
                std::memcpy(stack_snapshot.data() + name_bytes, value.data(), value_bytes);
            }
            return write_cookie_request_pair(out, name_overlaps ? std::string_view(stack_snapshot.data(), name_bytes) : name, value_overlaps ? std::string_view(stack_snapshot.data() + name_bytes, value_bytes) : value);
        }
        std::string snapshot;
        snapshot.reserve(name_bytes + value_bytes);
        if (name_overlaps) {
            snapshot.append(name);
        }
        if (value_overlaps) {
            snapshot.append(value);
        }
        return write_cookie_request_pair(out, name_overlaps ? std::string_view(snapshot.data(), name_bytes) : name, value_overlaps ? std::string_view(snapshot.data() + name_bytes, value_bytes) : value);
    }
    detail::http_raw_wire_sink sink(out);
    sink.append(name);
    sink.append('=');
    sink.append(value);
    return sink.position();
}

void set_cookie_plan::write(char* cursor_value) const {
    (void)write_fields(cursor_value);
}

set_cookie_plan::written_fields set_cookie_plan::write_fields(char* cursor_value) const noexcept {
    written_fields fields;
    detail::http_raw_wire_sink sink(cursor_value);
    const auto* const name_begin = sink.position();
    sink.append(prefix_text_);
    sink.append(name_);
    fields.wire_name_ = {name_begin, static_cast<std::size_t>(sink.position() - name_begin)};
    sink.append('=');
    sink.append(value_);
    if (!path_.empty()) {
        sink.append("; Path=");
        fields.path_ = {sink.position(), path_.size()};
        sink.append(path_);
    }
    if (!domain_.empty()) {
        sink.append("; Domain=");
        fields.domain_ = {sink.position(), domain_.size()};
        sink.append(domain_);
    }
    if (max_age_value_ != 0) {
        // The constructor sized this slice with http_unsigned_decimal_size().
        sink.append("; Max-Age=");
        sink.append_unsigned(max_age_value_);
    }
    if (expires_size_ != 0) {
        sink.append("; Expires=");
        sink.append(std::string_view(expires_buffer_.data(), expires_size_));
    }
    if (http_only_) {
        sink.append("; HttpOnly");
    }
    if (secure_) {
        sink.append("; Secure");
    }
    if (!same_site_text_.empty()) {
        sink.append("; SameSite=");
        sink.append(same_site_text_);
    }
    if (!priority_text_.empty()) {
        sink.append("; Priority=");
        sink.append(priority_text_);
    }
    if (partitioned_) {
        sink.append("; Partitioned");
    }
    return fields;
}

bool is_valid_cookie_value(std::string_view value) noexcept {
    // cookie-value = *cookie-octet / ( DQUOTE *cookie-octet DQUOTE ); the quotes
    // are part of the value (RFC 6265 section 4.1.1).
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte >= 0x7f || c == '"' || c == ',' || c == ';' || c == '\\') {
            return false;
        }
    }
    return true;
}

std::optional<cookie_prefix> cookie_name_prefix(std::string_view name) noexcept {
    if (http_ascii_starts_with_ignore_case(name, "__Host-")) {
        return cookie_prefix::host;
    }
    if (http_ascii_starts_with_ignore_case(name, "__Secure-")) {
        return cookie_prefix::secure;
    }
    return std::nullopt;
}

bool cookie_prefix_requirements_met(cookie_prefix prefix, bool secure, std::string_view path, std::string_view domain) noexcept {
    switch (prefix) {
        case cookie_prefix::secure:
            return secure;
        case cookie_prefix::host:
            return secure && path == "/" && domain.empty();
    }
    return false;
}

std::string_view http_cookie_prefix_text(cookie_prefix prefix) noexcept {
    switch (prefix) {
        case cookie_prefix::secure:
            return "__Secure-";
        case cookie_prefix::host:
            return "__Host-";
    }
    return {};
}

namespace {

std::pair<std::string_view, std::string_view> split_attribute(std::string_view value) noexcept {
    const auto equals = value.find('=');
    if (equals == std::string_view::npos) {
        return {http_trim_ows(value), {}};
    }
    return {http_trim_ows(value.substr(0, equals)), http_trim_ows(value.substr(equals + 1))};
}

std::optional<std::int64_t> parse_max_age_seconds(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }

    std::int64_t seconds = 0;
    const auto [parsed_value, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
    // Even overflow must consume the complete attribute before saturation.
    if (parsed_value != value.data() + value.size()) {
        return std::nullopt;
    }
    if (error == std::errc::result_out_of_range) {
        return value.front() == '-' ? std::numeric_limits<std::int64_t>::min() : max_cookie_age_seconds;
    }
    if (error != std::errc{}) {
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
    return byte == 0x09 || (byte >= 0x20 && byte <= 0x2f) || (byte >= 0x3b && byte <= 0x40) || (byte >= 0x5b && byte <= 0x60) || (byte >= 0x7b && byte <= 0x7e);
}

// Consume only on success. Any remaining suffix starts with a non-digit,
// as required by RFC 6265 section 5.1.1 and verified erratum 4148.
std::optional<int> consume_cookie_date_digits(std::string_view& value, std::size_t minimum_digits, std::size_t maximum_digits) noexcept {
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
    for (std::size_t index = 0; index < detail::http_month_names.size(); ++index) {
        if (http_ascii_equals_ignore_case(value, detail::http_month_names[index])) {
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
        while (cursor_value < value.size() && is_cookie_date_delimiter(static_cast<unsigned char>(value[cursor_value]))) {
            ++cursor_value;
        }
        const auto begin = cursor_value;
        while (cursor_value < value.size() && !is_cookie_date_delimiter(static_cast<unsigned char>(value[cursor_value]))) {
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
    if (*year < 1601 || *day < 1 || *day > 31 || time->hour_ > 23 || time->minute_ > 59 || time->second_ > 59) {
        return std::nullopt;
    }
    return detail::http_civil_to_time_t(*year, *month, *day, time->hour_, time->minute_, time->second_);
}

}  // namespace

std::optional<http_set_cookie_view> parse_set_cookie(std::string_view value) noexcept {
    const auto first_end = value.find(';');
    const auto cookie_pair = value.substr(0, first_end);
    // RFC 6265 section 5.2 steps 2 and 5: a pair without '=' or with an empty
    // name makes the user agent ignore the whole set-cookie-string.
    const auto equals = cookie_pair.find('=');
    if (equals == std::string_view::npos) {
        return std::nullopt;
    }
    const auto name = http_trim_ows(cookie_pair.substr(0, equals));
    const auto cookie_value = http_trim_ows(cookie_pair.substr(equals + 1));
    if (name.empty() || name.size() > detail::max_cookie_name_value_bytes || cookie_value.size() > detail::max_cookie_name_value_bytes - name.size() || contains_rejected_received_cookie_control(name) || contains_rejected_received_cookie_control(cookie_value)) {
        return std::nullopt;
    }

    http_set_cookie_view result(name, cookie_value);
    std::size_t offset = first_end == std::string_view::npos ? value.size() : first_end + 1;
    while (offset < value.size()) {
        const auto end = value.find(';', offset);
        const auto [attribute, argument] = split_attribute(value.substr(offset, end == std::string_view::npos ? value.size() - offset : end - offset));
        if (contains_rejected_received_cookie_control(attribute) || contains_rejected_received_cookie_control(argument)) {
            return std::nullopt;
        }
        if (argument.size() <= detail::max_cookie_attribute_value_bytes) {
            switch (attribute.size()) {
                case 4U:
                    if (http_ascii_equals_ignore_case(attribute, "Path")) {
                        // RFC 6265 section 5.2.4: a value that is empty or does not start
                        // with '/' selects the default-path; the last Path attribute wins.
                        result.path_ = argument.empty() || argument.front() != '/' ? std::string_view{} : argument;
                    }
                    break;
                case 6U:
                    if (http_ascii_equals_ignore_case(attribute, "Domain")) {
                        // RFC 6265 section 5.2.3: an empty value ignores this cookie-av; a
                        // leading '.' is dropped, so "Domain=." yields a host-only cookie.
                        // Domain-match against the request host is the storage step's job.
                        if (!argument.empty()) {
                            result.domain_ = argument.front() == '.' ? argument.substr(1) : argument;
                        }
                    } else if (http_ascii_equals_ignore_case(attribute, "Secure")) {
                        result.set(http_set_cookie_attribute::secure);
                    }
                    break;
                case 7U:
                    if (http_ascii_equals_ignore_case(attribute, "Expires")) {
                        if (const auto expires = parse_cookie_date(argument)) {
                            result.expires_ = expires;
                        }
                    } else if (http_ascii_equals_ignore_case(attribute, "Max-Age")) {
                        if (const auto seconds = parse_max_age_seconds(argument)) {
                            result.max_age_seconds_ = seconds;
                        }
                    }
                    break;
                case 8U:
                    if (http_ascii_equals_ignore_case(attribute, "SameSite")) {
                        if (http_ascii_equals_ignore_case(argument, "Strict")) {
                            result.same_site_ = cookie_same_site::strict;
                        } else if (http_ascii_equals_ignore_case(argument, "Lax")) {
                            result.same_site_ = cookie_same_site::lax;
                        } else if (http_ascii_equals_ignore_case(argument, "None")) {
                            result.same_site_ = cookie_same_site::none;
                        } else {
                            result.same_site_.reset();
                        }
                    } else if (http_ascii_equals_ignore_case(attribute, "HttpOnly")) {
                        result.set(http_set_cookie_attribute::http_only);
                    }
                    break;
                case 11U:
                    if (http_ascii_equals_ignore_case(attribute, "Partitioned")) {
                        result.set(http_set_cookie_attribute::partitioned);
                    }
                    break;
                default:
                    break;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1;
    }
    return result;
}

}  // namespace ruvia
