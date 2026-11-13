#include "ruvia/web/auth/jwt.h"

#ifdef RUVIA_ENABLE_JWT

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <forward_list>
#include <limits>
#include <optional>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_base64.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_utf8.h"
#include "ruvia/web/model.h"

#include "auth/constant_time.h"
#include "auth/cookie_signature.h"
#include "http/inline_http_header_views.h"

namespace ruvia {

namespace detail {
struct jwt_payload_access final {
    [[nodiscard]] static jwt_claim claim(std::string name, std::string value) noexcept {
        return jwt_claim(std::move(name), std::move(value));
    }
    [[nodiscard]] static jwt_numeric_date numeric_date(std::string_view text) {
        return jwt_numeric_date(text);
    }

    [[nodiscard]] static bool decode_jwt_payload_claim(jwt_payload& payload_value, std::string_view key,
        std::string_view& value, std::uint8_t& seen_reserved_mask);
    static jwt_payload decode_payload_json(std::string_view json);
};

namespace {

using clock_type = std::chrono::system_clock;
using clock_ratio_type = std::ratio_divide<std::chrono::seconds::period, clock_type::period>;
static_assert(clock_ratio_type::den == 1 && clock_ratio_type::num > 1,
    "JWT date arithmetic requires integral subsecond clock ticks");
static_assert(std::numeric_limits<clock_type::rep>::is_integer &&
              std::numeric_limits<clock_type::rep>::is_signed &&
              std::numeric_limits<clock_type::rep>::digits <= 63);
static_assert(std::numeric_limits<std::chrono::seconds::rep>::is_integer &&
              std::numeric_limits<std::chrono::seconds::rep>::is_signed &&
              std::numeric_limits<std::chrono::seconds::rep>::digits <= 63);
static_assert(std::numeric_limits<std::size_t>::digits <= 64);

constexpr auto ticks_per_second = static_cast<std::uint64_t>(clock_ratio_type::num);
constexpr int tick_decimal_places = [] {
    auto ticks = ticks_per_second;
    int places = 0;
    while (ticks > 1 && ticks % 10 == 0) {
        ticks /= 10;
        ++places;
    }
    return ticks == 1 ? places : -1;
}();
static_assert(tick_decimal_places > 0 && tick_decimal_places <= 18,
    "JWT date arithmetic requires decimal subsecond clock ticks");

[[nodiscard]] constexpr bool is_digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

[[nodiscard]] constexpr std::uint64_t unsigned_magnitude(std::int64_t value) noexcept {
    // Negating the signed minimum is never an intermediate operation.
    return value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1 : static_cast<std::uint64_t>(value);
}

struct decimal_parts final {
    std::string_view text_{};
    std::string_view exponent_{};
    std::size_t integer_end_{};
    std::size_t first_nonzero_{std::string_view::npos};
    std::size_t last_nonzero_{};
    bool negative_{};
    bool exponent_negative_{};
};

// Decomposes a JSON number whose RFC 8259 grammar was already validated by scan_json_number_token.
[[nodiscard]] decimal_parts decompose_validated_decimal(std::string_view text) noexcept {
    decimal_parts parts{.text_ = text};
    std::size_t position = 0;
    if (position < text.size() && text[position] == '-') {
        parts.negative_ = true;
        ++position;
    }
    const auto consume_mantissa_digits = [&] {
        while (position < text.size() && is_digit(text[position])) {
            if (text[position] != '0') {
                if (parts.first_nonzero_ == std::string_view::npos) {
                    parts.first_nonzero_ = position;
                }
                parts.last_nonzero_ = position;
            }
            ++position;
        }
    };
    consume_mantissa_digits();
    parts.integer_end_ = position;
    if (position < text.size() && text[position] == '.') {
        ++position;
        consume_mantissa_digits();
    }
    if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
            parts.exponent_negative_ = text[position] == '-';
            ++position;
        }
        while (position < text.size() && text[position] == '0') {
            ++position;
        }
        parts.exponent_ = text.substr(position);
    }
    return parts;
}

// Orders outside [-64, 64] cannot approach either a <=20-digit seconds
// threshold or a host tick (at most 18 decimal places). Only the final order
// is clipped: a large exponent can cancel a large mantissa displacement.
[[nodiscard]] int clipped_unsigned(std::string_view digits) noexcept {
    if (digits.size() > 2) {
        return 64;
    }
    int value = 0;
    for (const char digit : digits) {
        value = value * 10 + digit - '0';
    }
    return std::min(value, 64);
}

[[nodiscard]] int clipped_difference(std::string_view left, std::string_view right) noexcept {
    const bool negative = left.size() < right.size() || (left.size() == right.size() && left < right);
    if (negative) {
        std::swap(left, right);
    }
    if (left.size() > 2 && left.size() - right.size() > 1) {
        return negative ? -64 : 64;
    }
    int value = 0;
    int place = 1;
    int borrow = 0;
    // Decimal subtraction avoids even a size_t overflow when the exponent
    // and the mantissa displacement straddle the largest machine integer.
    while (!left.empty()) {
        int digit = left.back() - '0' - borrow;
        left.remove_suffix(1);
        if (!right.empty()) {
            digit -= right.back() - '0';
            right.remove_suffix(1);
        }
        borrow = digit < 0 ? 1 : 0;
        if (borrow != 0) {
            digit += 10;
        }
        value = std::min(64, value + digit * place);
        place = std::min(64, place * 10);
    }
    return negative ? -value : value;
}

struct decimal_view final {
    std::string_view digits_;
    std::size_t digit_count_{};
    int order_{};
    bool negative_{};
};

[[nodiscard]] decimal_view normalize_decimal(const decimal_parts& parts) noexcept {
    if (parts.first_nonzero_ == std::string_view::npos) {
        return {};  // Every spelling of signed zero has the same mathematical value.
    }
    const bool shift_negative = parts.first_nonzero_ > parts.integer_end_;
    const auto shift = shift_negative ? parts.first_nonzero_ - parts.integer_end_ - 1 : parts.integer_end_ - parts.first_nonzero_;
    const int clipped_shift = static_cast<int>(std::min<std::size_t>(shift, 64));
    int order;
    if (parts.exponent_.empty()) {
        order = shift_negative ? -clipped_shift : clipped_shift;
    } else if (shift_negative == parts.exponent_negative_) {
        order = std::min(64, clipped_shift + clipped_unsigned(parts.exponent_));
        if (shift_negative) {
            order = -order;
        }
    } else {
        std::array<char, http_max_unsigned_decimal_size> buffer;
        const auto shift_digits = http_format_unsigned(buffer, static_cast<std::uint64_t>(shift));
        order = clipped_difference(parts.exponent_, shift_digits);
        if (parts.exponent_negative_) {
            order = -order;
        }
    }
    const auto digits = parts.text_.substr(parts.first_nonzero_, parts.last_nonzero_ - parts.first_nonzero_ + 1);
    const bool contains_point = parts.first_nonzero_ < parts.integer_end_ && parts.last_nonzero_ > parts.integer_end_;
    return {digits, digits.size() - static_cast<std::size_t>(contains_point), order, parts.negative_};
}

[[nodiscard]] char next_digit(std::string_view& digits) noexcept {
    if (!digits.empty() && digits.front() == '.') {
        digits.remove_prefix(1);
    }
    if (digits.empty()) {
        return '0';
    }
    const char digit = digits.front();
    digits.remove_prefix(1);
    return digit;
}

[[nodiscard]] std::strong_ordering compare_decimal(decimal_view left, decimal_view right) noexcept {
    if (left.negative_ != right.negative_) {
        return left.negative_ ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    auto magnitude = std::strong_ordering::equal;
    if (left.digits_.empty() || right.digits_.empty()) {
        magnitude = (!left.digits_.empty()) <=> (!right.digits_.empty());
    } else if (left.order_ != right.order_) {
        magnitude = left.order_ <=> right.order_;
    } else {
        while (!left.digits_.empty() || !right.digits_.empty()) {
            magnitude = next_digit(left.digits_) <=> next_digit(right.digits_);
            if (magnitude != 0) {
                break;
            }
        }
    }
    return left.negative_ ? 0 <=> magnitude : magnitude;
}

struct clock_parts final {
    std::uint64_t seconds_{};
    std::uint64_t fraction_{};
    bool negative_{};
};

[[nodiscard]] clock_parts offset_clock(clock_type::time_point now, std::chrono::seconds offset) noexcept {
    const auto ticks = now.time_since_epoch().count();
    const auto magnitude = unsigned_magnitude(ticks);
    clock_parts parts{magnitude / ticks_per_second, magnitude % ticks_per_second, ticks < 0};
    const auto offset_magnitude = unsigned_magnitude(offset.count());
    if (parts.negative_ == (offset.count() < 0)) {
        // A subsecond clock divides its <=64-bit magnitude by at least ten;
        // adding any signed seconds magnitude therefore fits in uint64_t.
        parts.seconds_ += offset_magnitude;
    } else if (parts.seconds_ >= offset_magnitude) {
        parts.seconds_ -= offset_magnitude;
    } else {
        parts.seconds_ = offset_magnitude - parts.seconds_;
        parts.negative_ = !parts.negative_;
        if (parts.fraction_ != 0) {
            --parts.seconds_;
            parts.fraction_ = ticks_per_second - parts.fraction_;
        }
    }
    if (parts.seconds_ == 0 && parts.fraction_ == 0) {
        parts.negative_ = false;
    }
    return parts;
}

[[nodiscard]] std::string_view format_seconds(
    std::span<char, 21> output, std::uint64_t seconds, bool negative) noexcept {
    const std::size_t prefix = negative ? 1 : 0;
    if (negative) {
        output[0] = '-';
    }
    const auto digits = http_format_unsigned(
        std::span<char, http_max_unsigned_decimal_size>{output.data() + prefix, http_max_unsigned_decimal_size},
        seconds);
    return {output.data(), prefix + digits.size()};
}

[[nodiscard]] decimal_view format_clock_decimal(std::span<char, 40> buffer, clock_parts parts) noexcept {
    if (parts.seconds_ == 0 && parts.fraction_ == 0) {
        return {};
    }
    if (parts.seconds_ == 0) {
        int order = 0;
        auto fraction = parts.fraction_;
        auto divisor = ticks_per_second / 10;
        while (fraction < divisor) {
            --order;
            divisor /= 10;
        }
        std::size_t size = 0;
        for (; fraction != 0; divisor /= 10) {
            buffer[size++] = static_cast<char>('0' + fraction / divisor);
            fraction %= divisor;
        }
        return {{buffer.data(), size}, size, order, parts.negative_};
    }
    const auto seconds_digits = http_format_unsigned(
        std::span<char, http_max_unsigned_decimal_size>{buffer.data(), http_max_unsigned_decimal_size},
        parts.seconds_);
    const int order = static_cast<int>(seconds_digits.size());
    auto size = seconds_digits.size();
    if (parts.fraction_ == 0) {
        while (buffer[size - 1] == '0') {
            --size;
        }
        return {{buffer.data(), size}, size, order, parts.negative_};
    }
    buffer[size++] = '.';
    auto fraction = parts.fraction_;
    for (auto divisor = ticks_per_second / 10; fraction != 0; divisor /= 10) {
        buffer[size++] = static_cast<char>('0' + fraction / divisor);
        fraction %= divisor;
    }
    return {{buffer.data(), size}, size - 1, order, parts.negative_};
}

// Compares an already validated NumericDate to now + offset without narrowing.
[[nodiscard]] std::strong_ordering jwt_compare_numeric_date(
    std::string_view date, clock_type::time_point now, std::chrono::seconds offset) noexcept {
    std::array<char, 40> buffer;
    return compare_decimal(
        normalize_decimal(decompose_validated_decimal(date)), format_clock_decimal(buffer, offset_clock(now, offset)));
}

// A validated nonnegative offset rounds up; zero rounds down to whole seconds.
[[nodiscard]] std::string_view jwt_signing_date(
    std::span<char, 21> output, clock_type::time_point now, std::chrono::seconds offset) noexcept {
    auto parts = offset_clock(now, offset);
    // Integer offsets preserve the fraction, so rounding the exact sum is
    // equivalent to rounding now first, without ever constructing host ticks.
    if (parts.fraction_ != 0 && (offset.count() == 0 ? parts.negative_ : !parts.negative_)) {
        ++parts.seconds_;
    }
    return format_seconds(output, parts.seconds_, parts.negative_ && parts.seconds_ != 0);
}

[[nodiscard]] constexpr std::uint8_t jwt_reserved_claim_bit(std::string_view name) noexcept {
    if (name.size() != 3) {
        return 0;
    }
    if (name == "iss") {
        return 1U << 0;
    }
    if (name == "sub") {
        return 1U << 1;
    }
    if (name == "aud") {
        return 1U << 2;
    }
    if (name == "jti") {
        return 1U << 3;
    }
    if (name == "exp") {
        return 1U << 4;
    }
    if (name == "nbf") {
        return 1U << 5;
    }
    if (name == "iat") {
        return 1U << 6;
    }
    return 0;
}

[[nodiscard]] constexpr bool jwt_is_reserved_claim(std::string_view name) noexcept {
    return jwt_reserved_claim_bit(name) != 0;
}

// Duplicate names are detected once the object is complete; any duplicate
// rejects the object, so visiting each field on demand changes nothing for
// callers while avoiding a pre-pass skip and field slice buffering.
template <typename visitor_type>
[[nodiscard]] bool visit_unique_jwt_json_object_fields(std::string_view json, visitor_type&& visitor) {
    // Literal names are views into json. Escaped names arrive decoded in storage
    // that lives for one visitor call, so only those are copied; list nodes keep
    // each copy at a stable address.
    static constexpr std::size_t inline_capacity = 16;
    std::array<std::string_view, inline_capacity> inline_names;
    std::size_t inline_count = 0;
    std::vector<std::string_view> heap_names;
    std::forward_list<std::string> decoded_names;
    const auto record_name = [&](std::string_view stored_name) {
        if (!heap_names.empty()) {
            heap_names.push_back(stored_name);
        } else if (inline_count < inline_capacity) {
            inline_names[inline_count++] = stored_name;
        } else {
            heap_names.reserve(inline_capacity * 2U);
            heap_names.assign(inline_names.begin(), inline_names.end());
            heap_names.push_back(stored_name);
        }
    };
    auto input = json;
    auto& visitor_ref = visitor;
    const auto json_begin = reinterpret_cast<std::uintptr_t>(json.data());
    const bool consumed = consume_json_object_fields(input, 0,
        [&](std::string_view name, std::string_view& value_input) {
            const auto name_begin = reinterpret_cast<std::uintptr_t>(name.data());
            if (name_begin >= json_begin && name_begin - json_begin <= json.size() &&
                name.size() <= json.size() - (name_begin - json_begin)) {
                record_name(name);
            } else {
                record_name(decoded_names.emplace_front(name));
            }
            return static_cast<bool>(visitor_ref(name, value_input));
        });
    if (!consumed) {
        return false;
    }
    skip_json_whitespace(input);
    if (!input.empty()) {
        return false;
    }
    if (heap_names.empty()) {
        if (inline_count <= 8) {
            for (std::size_t i = 1; i < inline_count; ++i) {
                for (std::size_t j = 0; j < i; ++j) {
                    if (inline_names[i] == inline_names[j]) {
                        return false;
                    }
                }
            }
        } else {
            const std::span<std::string_view> active_names(inline_names.data(), inline_count);
            std::ranges::sort(active_names);
            if (std::ranges::adjacent_find(active_names) != active_names.end()) {
                return false;
            }
        }
    } else {
        std::ranges::sort(heap_names);
        if (std::ranges::adjacent_find(heap_names) != heap_names.end()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::string_view> jwt_parse_json_numeric_date(std::string_view& value) noexcept {
    skip_json_whitespace(value);
    const auto token = scan_json_number_token(value);
    if (token.length_ == 0) {
        return std::nullopt;
    }
    const auto number = value.substr(0, token.length_);
    value.remove_prefix(token.length_);
    return number;
}

[[nodiscard]] std::optional<std::string> jwt_decode_json_string_value(std::string_view& value) {
    std::string decoded;
    const auto parsed_value = parse_json_string(value, &decoded);
    if (!parsed_value.has_value()) {
        return std::nullopt;
    }
    if (parsed_value->encoding() == json_string_encoding::literal) {
        decoded.assign(parsed_value->raw());
    }
    return decoded;
}

// Decode the JWT "aud" claim (RFC 7519 §4.1.3): either a single JSON string or a
// JSON array of strings. Each decoded audience is appended to `out`. On any
// malformed structure (non-string element, unterminated array, trailing junk)
// `out` is cleared and false is returned, so verification fails closed rather
// than acting on a half-parsed list.
[[nodiscard]] bool jwt_decode_audiences(std::vector<std::string>& out, std::string_view& value) {
    detail::skip_json_whitespace(value);
    if (value.empty()) {
        return false;
    }
    if (value.front() != '[') {
        if (auto single = detail::jwt_decode_json_string_value(value)) {
            out.push_back(std::move(*single));
            return true;
        }
        return false;
    }

    const bool consumed = detail::consume_json_array_elements(value, 1,
        [&out](std::string_view& element) {
            if (auto decoded = detail::jwt_decode_json_string_value(element)) {
                out.push_back(std::move(*decoded));
                return true;
            }
            return false;
        });
    if (!consumed || out.empty()) {
        out.clear();
        return false;
    }
    return true;
}

void jwt_append_json_member(
    inline_text_buffer<512>& out, bool& first, std::string_view name, std::string_view value) {
    if (!first) {
        out.push_back(',');
    }
    first = false;
    append_json_string(out, name);
    out.push_back(':');
    append_json_string(out, value);
}

void jwt_append_json_numeric_date(
    inline_text_buffer<512>& out, bool& first, std::string_view name, std::string_view value) {
    if (!first) {
        out.push_back(',');
    }
    first = false;
    append_json_string(out, name);
    out.push_back(':');
    out.append(value);
}

void jwt_validate_jose_header(std::string_view json, std::optional<std::string_view> expected_algorithm) {
    std::optional<std::string_view> algorithm;
    std::string decoded_algorithm;
    bool unsupported_extension = false;
    const bool valid = visit_unique_jwt_json_object_fields(
        json, [&](std::string_view name, std::string_view& value) {
            if (name == "alg") {
                const auto parsed = parse_json_string(value, &decoded_algorithm);
                if (!parsed.has_value()) {
                    return false;
                }
                algorithm = json_string_text(*parsed, decoded_algorithm);
                return true;
            }
            if (name == "crit" || name == "b64") {
                // No critical extensions are implemented. RFC 7797 section 7
                // forbids b64=false in JWTs; section 6 requires crit whenever
                // b64 is present, even when true. Reject this extension also
                // when crit is missing, rather than silently decoding a payload
                // whose header declares different encoding semantics.
                unsupported_extension = true;
                return false;
            }
            return skip_json_value(value, 1);
        });
    if (!valid || !algorithm.has_value() || unsupported_extension) {
        throw jwt_error(jwt_error::code_type::invalid_header, "JWT JOSE header is invalid");
    }
    if (expected_algorithm.has_value()) {
        if (*algorithm != *expected_algorithm) {
            throw jwt_error(jwt_error::code_type::algorithm_mismatch, "JWT algorithm mismatch");
        }
    } else if (algorithm->empty()) {
        throw jwt_error(jwt_error::code_type::invalid_header, "JWT JOSE header is invalid");
    }
}

// Views into the caller's token.
struct jwt_token_parts final {
    std::string_view header_;
    std::string_view payload_;
    std::string_view signature_;
    std::string_view signing_input_;
};

[[nodiscard]] jwt_token_parts jwt_split_token(std::string_view token) {
    const auto first = token.find('.');
    const auto second =
        first == std::string_view::npos ? std::string_view::npos : token.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        token.find('.', second + 1) != std::string_view::npos) {
        throw jwt_error(jwt_error::code_type::malformed_token, "JWT token must have three sections");
    }
    return jwt_token_parts{token.substr(0, first), token.substr(first + 1, second - first - 1),
        token.substr(second + 1), token.substr(0, second)};
}

void jwt_base64_url_encode_into(char* out, std::string_view input) noexcept {
    http_base64_encode(out,
        std::span(reinterpret_cast<const std::uint8_t*>(input.data()), input.size()),
        http_base64_charset::url_safe, http_base64_padding::forbidden);
}

struct jwt_decoded_section final {
    static constexpr std::size_t inline_capacity = 256;
    std::array<char, inline_capacity> inline_;
    std::string heap_;
    std::size_t size_{0};

    explicit jwt_decoded_section(std::string_view input) {
        const auto bound = http_base64_decoded_size_bound(input.size());
        char* target = inline_.data();
        if (bound > inline_capacity) {
            heap_.resize(bound);
            target = heap_.data();
        }
        const auto decoded_size = http_base64_decode(input,
            std::span(reinterpret_cast<std::uint8_t*>(target), bound),
            http_base64_charset::url_safe, http_base64_padding::forbidden);
        if (!decoded_size) {
            throw jwt_error(jwt_error::code_type::malformed_token, "JWT base64url value is invalid");
        }
        size_ = *decoded_size;
        if (!heap_.empty()) {
            heap_.resize(size_);
        }
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return heap_.empty() ? std::string_view(inline_.data(), size_) : std::string_view(heap_);
    }
    std::string_view view() const&& = delete;
};

[[nodiscard]] hmac_digest jwt_hmac_digest(jwt_algorithm algorithm) {
    switch (algorithm) {
        case jwt_algorithm::hs256:
            return hmac_digest::sha256;
        case jwt_algorithm::hs384:
            return hmac_digest::sha384;
        case jwt_algorithm::hs512:
            return hmac_digest::sha512;
    }
    throw std::invalid_argument("unsupported JWT algorithm");
}

[[nodiscard]] std::string_view jwt_algorithm_name(jwt_algorithm algorithm) {
    switch (algorithm) {
        case jwt_algorithm::hs256:
            return "HS256";
        case jwt_algorithm::hs384:
            return "HS384";
        case jwt_algorithm::hs512:
            return "HS512";
    }
    return {};
}

// The base64url JOSE header jwt_sign emits: {"alg":"<name>","typ":"JWT"}.
[[nodiscard]] std::string_view jwt_encoded_header(jwt_algorithm algorithm) {
    switch (algorithm) {
        case jwt_algorithm::hs256:
            return "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
        case jwt_algorithm::hs384:
            return "eyJhbGciOiJIUzM4NCIsInR5cCI6IkpXVCJ9";
        case jwt_algorithm::hs512:
            return "eyJhbGciOiJIUzUxMiIsInR5cCI6IkpXVCJ9";
    }
    throw std::invalid_argument("unsupported JWT algorithm");
}

void validate_jwt_hmac_secret(std::size_t digest_size, std::string_view secret) {
    if (secret.size() > hmac_max_input_size) {
        throw std::length_error("JWT secret is too large");
    }
    // RFC 7518 section 3.2: the key must be at least as large as the hash output.
    if (secret.size() < digest_size) {
        throw std::invalid_argument("JWT secret is shorter than the selected algorithm's digest");
    }
}

void validate_jwt_custom_claims(std::span<const jwt_claim_view> claims) {
    for (std::size_t i = 0; i < claims.size(); ++i) {
        const auto name = claims[i].name_.view();
        if (name.empty() || detail::jwt_is_reserved_claim(name)) {
            throw std::invalid_argument("JWT custom claim name is empty or reserved");
        }
        // The payload writer replaces each ill-formed UTF-8 subpart with U+FFFD, so
        // two distinct ill-formed names could serialize as one; only well-formed
        // names keep the raw uniqueness check equal to the signed one.
        if (!::ruvia::is_valid_utf8(name)) {
            throw std::invalid_argument("JWT custom claim name must be well-formed UTF-8");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (claims[j].name_.view() == name) {
                throw std::invalid_argument("JWT custom claim names must be unique");
            }
        }
    }
}

// Encodes the base64url signature into `out`. Throws std::invalid_argument for
// a key shorter than the selected digest and std::length_error when the secret
// or signing input exceeds OpenSSL's HMAC length parameters.
[[nodiscard]] std::string_view jwt_hmac_sign(
    jwt_algorithm algorithm, std::string_view secret, std::string_view signing_input, char* out) {
    const auto digest = jwt_hmac_digest(algorithm);
    const auto digest_size = hmac_digest_size(digest);
    validate_jwt_hmac_secret(digest_size, secret);
    if (signing_input.size() > hmac_max_input_size) {
        throw std::length_error("JWT signing input is too large");
    }
    std::array<std::uint8_t, hmac_max_digest_size> mac;
    if (!compute_hmac(digest, secret, signing_input, mac)) {
        throw std::runtime_error("JWT HMAC signing failed");
    }
    const auto encoded_size = http_base64_encoded_size(digest_size, http_base64_padding::forbidden);
    jwt_base64_url_encode_into(
        out, std::string_view(reinterpret_cast<const char*>(mac.data()), digest_size));
    return std::string_view(out, encoded_size);
}

}  // namespace

bool jwt_payload_access::decode_jwt_payload_claim(jwt_payload& payload_value, std::string_view key,
    std::string_view& value, std::uint8_t& seen_reserved_mask) {
    if (const auto bit = detail::jwt_reserved_claim_bit(key); bit != 0) {
        if ((seen_reserved_mask & bit) != 0) {
            return false;
        }
        seen_reserved_mask |= bit;
        switch (bit) {
            case 1U << 0:
                if (auto issuer = detail::jwt_decode_json_string_value(value)) {
                    payload_value.issuer_ = std::move(*issuer);
                    return true;
                }
                return false;
            case 1U << 1:
                if (auto subject = detail::jwt_decode_json_string_value(value)) {
                    payload_value.subject_ = std::move(*subject);
                    return true;
                }
                return false;
            case 1U << 2:
                return jwt_decode_audiences(payload_value.audiences_, value);
            case 1U << 3:
                if (auto id = detail::jwt_decode_json_string_value(value)) {
                    payload_value.id_ = std::move(*id);
                    return true;
                }
                return false;
            case 1U << 4:
                if (const auto exp = detail::jwt_parse_json_numeric_date(value)) {
                    payload_value.expires_at_.emplace(jwt_numeric_date(*exp));
                    return true;
                }
                return false;
            case 1U << 5:
                if (const auto nbf = detail::jwt_parse_json_numeric_date(value)) {
                    payload_value.not_before_.emplace(jwt_numeric_date(*nbf));
                    return true;
                }
                return false;
            case 1U << 6:
                if (const auto iat = detail::jwt_parse_json_numeric_date(value)) {
                    payload_value.issued_at_.emplace(jwt_numeric_date(*iat));
                    return true;
                }
                return false;
        }
        return false;
    }
    // A claim value can be any JSON value (RFC 7519 §2), and claims that are
    // not understood MUST be ignored (§4). Only string values are exposed
    // through claims(); every other valid JSON value is skipped at depth 1.
    detail::skip_json_whitespace(value);
    if (value.empty()) {
        return false;
    }
    if (value.front() != '"') {
        return detail::skip_json_value(value, 1);
    }
    if (auto claim_value = detail::jwt_decode_json_string_value(value)) {
        payload_value.claims_.push_back(detail::jwt_payload_access::claim(
            std::string(key), std::move(*claim_value)));
        return true;
    }
    return false;
}

jwt_payload jwt_payload_access::decode_payload_json(std::string_view json) {
    jwt_payload payload_value;
    std::uint8_t seen_reserved_mask = 0;
    const bool valid = detail::visit_unique_jwt_json_object_fields(
        json, [&](std::string_view key, std::string_view& value) {
            return decode_jwt_payload_claim(payload_value, key, value, seen_reserved_mask);
        });
    if (!valid) {
        throw jwt_error(
            jwt_error::code_type::invalid_payload, "JWT payload is not a valid unique claims object");
    }
    return payload_value;
}

}  // namespace detail

jwt_claim& jwt_claim::operator=(jwt_claim&& other) noexcept {
    if (this != &other) {
        name_ = std::move(other.name_);
        value_ = std::move(other.value_);
        other.name_.clear();
        other.value_.clear();
    }
    return *this;
}

jwt_payload& jwt_payload::operator=(jwt_payload&& other) noexcept {
    if (this != &other) {
        issuer_ = std::move(other.issuer_);
        subject_ = std::move(other.subject_);
        audiences_ = std::move(other.audiences_);
        id_ = std::move(other.id_);
        expires_at_ = std::exchange(other.expires_at_, std::nullopt);
        not_before_ = std::exchange(other.not_before_, std::nullopt);
        issued_at_ = std::exchange(other.issued_at_, std::nullopt);
        claims_ = std::move(other.claims_);
        other.issuer_.clear();
        other.subject_.clear();
        other.audiences_.clear();
        other.id_.clear();
        other.claims_.clear();
    }
    return *this;
}

std::string_view jwt_payload::issuer() const& noexcept {
    return issuer_;
}
std::string_view jwt_payload::subject() const& noexcept {
    return subject_;
}
std::string_view jwt_payload::audience() const& noexcept {
    return audiences_.empty() ? std::string_view{} : std::string_view(audiences_.front());
}
std::span<const std::string> jwt_payload::audiences() const& noexcept {
    return audiences_;
}
bool jwt_payload::has_audience(std::string_view audience) const noexcept {
    return std::ranges::find(audiences_, audience, [](const auto& value) noexcept {
        return std::string_view(value);
    }) != audiences_.end();
}
std::string_view jwt_payload::id() const& noexcept {
    return id_;
}
const jwt_numeric_date* jwt_payload::expires_at() const& noexcept {
    return expires_at_ ? &*expires_at_ : nullptr;
}
const jwt_numeric_date* jwt_payload::not_before() const& noexcept {
    return not_before_ ? &*not_before_ : nullptr;
}
const jwt_numeric_date* jwt_payload::issued_at() const& noexcept {
    return issued_at_ ? &*issued_at_ : nullptr;
}
std::span<const jwt_claim> jwt_payload::claims() const& noexcept {
    return claims_;
}

std::optional<std::string_view> jwt_payload::claim(std::string_view name) const& noexcept {
    for (const auto& item : claims_) {
        if (item.name() == name) {
            return item.value();
        }
    }
    return std::nullopt;
}

std::string jwt_sign(const jwt_sign_options& options) {
    detail::validate_jwt_custom_claims(options.claims_);
    if ((options.expires_in_.has_value() && options.expires_in_->count() < 0) ||
        (options.not_before_delay_.has_value() && options.not_before_delay_->count() < 0)) {
        throw std::invalid_argument("JWT signing time offsets must not be negative");
    }
    const auto digest = detail::jwt_hmac_digest(options.algorithm_);
    detail::validate_jwt_hmac_secret(detail::hmac_digest_size(digest), options.secret_);
    const auto encoded_header = detail::jwt_encoded_header(options.algorithm_);
    const auto now = std::chrono::system_clock::now();

    std::size_t payload_reserve = 64U + options.issuer_.size() + options.subject_.size() +
                                  options.audience_.size() + options.id_.size() +
                                  (options.expires_in_.has_value() ? 20U : 0U) +
                                  (options.not_before_delay_.has_value() ? 20U : 0U);
    for (const auto& claim : options.claims_) {
        payload_reserve += claim.name_.size() + claim.value_.size() + 8U;
    }
    detail::inline_text_buffer<512> payload(payload_reserve);
    payload.push_back('{');
    bool first = true;
    if (!options.issuer_.empty()) {
        detail::jwt_append_json_member(payload, first, "iss", options.issuer_);
    }
    if (!options.subject_.empty()) {
        detail::jwt_append_json_member(payload, first, "sub", options.subject_);
    }
    if (!options.audience_.empty()) {
        detail::jwt_append_json_member(payload, first, "aud", options.audience_);
    }
    if (!options.id_.empty()) {
        detail::jwt_append_json_member(payload, first, "jti", options.id_);
    }
    std::array<char, 21> date_buffer;
    detail::jwt_append_json_numeric_date(payload, first, "iat",
        detail::jwt_signing_date(date_buffer, now, std::chrono::seconds{0}));
    if (options.expires_in_.has_value()) {
        detail::jwt_append_json_numeric_date(payload, first, "exp",
            detail::jwt_signing_date(date_buffer, now, *options.expires_in_));
    }
    if (options.not_before_delay_.has_value()) {
        detail::jwt_append_json_numeric_date(payload, first, "nbf",
            detail::jwt_signing_date(date_buffer, now, *options.not_before_delay_));
    }
    for (const auto& claim : options.claims_) {
        detail::jwt_append_json_member(payload, first, claim.name_.view(), claim.value_.view());
    }
    payload.push_back('}');

    const auto encoded_payload_size =
        http_base64_encoded_size(payload.size(), http_base64_padding::forbidden);
    const auto signature_size = http_base64_encoded_size(
        detail::hmac_digest_size(digest),
        http_base64_padding::forbidden);
    const auto payload_offset = encoded_header.size() + 1;
    if (encoded_payload_size > std::numeric_limits<std::size_t>::max() - 1 - payload_offset ||
        signature_size > std::numeric_limits<std::size_t>::max() - (payload_offset + encoded_payload_size + 1)) {
        throw std::length_error("JWT token is too large");
    }
    const auto signing_input_size = payload_offset + encoded_payload_size;
    std::string token;
    token.resize(signing_input_size + 1 + signature_size);
    encoded_header.copy(token.data(), encoded_header.size());
    token[encoded_header.size()] = '.';
    detail::jwt_base64_url_encode_into(token.data() + payload_offset, payload.view());
    token[signing_input_size] = '.';
    (void)detail::jwt_hmac_sign(options.algorithm_, options.secret_,
        std::string_view(token.data(), signing_input_size), token.data() + signing_input_size + 1);
    return token;
}

jwt_payload jwt_verify(const jwt_verify_options& options) {
    if (options.leeway_.count() < 0) {
        throw std::invalid_argument("JWT verification leeway must not be negative");
    }
    if (options.expiration_claim_ != jwt_expiration_claim_policy::require &&
        options.expiration_claim_ != jwt_expiration_claim_policy::allow_missing) {
        throw std::invalid_argument("JWT expiration claim policy is invalid");
    }
    const auto token = options.token_.view();
    const auto parts = detail::jwt_split_token(token);
    std::array<char, 88> expected_buf;
    const auto expected = detail::jwt_hmac_sign(
        options.algorithm_, options.secret_, parts.signing_input_, expected_buf.data());
    if (!detail::constant_time_bytes_equal(expected, parts.signature_)) {
        throw jwt_error(jwt_error::code_type::signature_mismatch, "JWT signature verification failed");
    }
    if (parts.header_ != detail::jwt_encoded_header(options.algorithm_)) {
        const detail::jwt_decoded_section header_value(parts.header_);
        detail::jwt_validate_jose_header(
            header_value.view(), detail::jwt_algorithm_name(options.algorithm_));
    }
    const detail::jwt_decoded_section payload_json(parts.payload_);
    auto payload_value = detail::jwt_payload_access::decode_payload_json(payload_json.view());
    const auto* expires_at = payload_value.expires_at();
    const auto* not_before = payload_value.not_before();
    if (options.expiration_claim_ == jwt_expiration_claim_policy::require && expires_at == nullptr) {
        throw jwt_error(jwt_error::code_type::missing_claim, "JWT token is missing exp claim");
    }
    if (expires_at != nullptr || not_before != nullptr) {
        const auto now = std::chrono::system_clock::now();
        // RFC 7519 §4.1.4: exp == now - leeway is already expired.
        if (expires_at != nullptr &&
            detail::jwt_compare_numeric_date(expires_at->text(), now, -options.leeway_) != std::strong_ordering::greater) {
            throw jwt_error(jwt_error::code_type::expired, "JWT token is expired");
        }
        // RFC 7519 §4.1.5: nbf == now + leeway is already valid.
        if (not_before != nullptr &&
            detail::jwt_compare_numeric_date(not_before->text(), now, options.leeway_) == std::strong_ordering::greater) {
            throw jwt_error(jwt_error::code_type::not_yet_valid, "JWT token is not yet valid");
        }
    }
    if (!options.issuer_.empty() && payload_value.issuer() != options.issuer_.view()) {
        throw jwt_error(jwt_error::code_type::claim_mismatch, "JWT issuer mismatch");
    }
    if (!options.subject_.empty() && payload_value.subject() != options.subject_.view()) {
        throw jwt_error(jwt_error::code_type::claim_mismatch, "JWT subject mismatch");
    }
    // RFC 7519 §4.1.3: a token that names audiences is rejected unless this
    // verifier identifies itself with one of them.
    if (options.audience_.empty()) {
        if (!payload_value.audiences().empty()) {
            throw jwt_error(
                jwt_error::code_type::claim_mismatch, "JWT audience is present but none is expected");
        }
    } else if (!payload_value.has_audience(options.audience_.view())) {
        throw jwt_error(jwt_error::code_type::claim_mismatch, "JWT audience mismatch");
    }
    return payload_value;
}

jwt_payload jwt_decode_unverified(const jwt_decode_unverified_options& options) {
    const auto token = options.token_.view();
    const auto parts = detail::jwt_split_token(token);
    if (parts.header_ != detail::jwt_encoded_header(jwt_algorithm::hs256) &&
        parts.header_ != detail::jwt_encoded_header(jwt_algorithm::hs384) &&
        parts.header_ != detail::jwt_encoded_header(jwt_algorithm::hs512)) {
        const detail::jwt_decoded_section header_value(parts.header_);
        detail::jwt_validate_jose_header(header_value.view(), std::nullopt);
    }
    const detail::jwt_decoded_section payload_json(parts.payload_);
    return detail::jwt_payload_access::decode_payload_json(payload_json.view());
}

std::optional<std::string_view> jwt_bearer_token(std::string_view authorization) noexcept {
    constexpr std::string_view scheme = "Bearer";
    if (authorization.size() <= scheme.size() ||
        !http_ascii_equals_ignore_case(authorization.substr(0, scheme.size()), scheme)) {
        return std::nullopt;
    }
    if (authorization[scheme.size()] != ' ') {
        return std::nullopt;
    }
    auto token_offset = scheme.size();
    while (token_offset < authorization.size() && authorization[token_offset] == ' ') {
        ++token_offset;
    }
    // RFC 6750 §2.1: b64token = 1*( ALPHA / DIGIT / "-" / "." / "_" / "~" /
    // "+" / "/" ) *"=".
    const auto is_b64token_char = [](char value) noexcept {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
               value == '-' || value == '.' || value == '_' || value == '~' || value == '+' || value == '/';
    };
    const auto token = authorization.substr(token_offset);
    std::size_t index = 0;
    while (index < token.size() && is_b64token_char(token[index])) {
        ++index;
    }
    if (index == 0) {
        return std::nullopt;
    }
    while (index < token.size() && token[index] == '=') {
        ++index;
    }
    if (index != token.size()) {
        return std::nullopt;
    }
    return token;
}

jwt_numeric_date::jwt_numeric_date(std::string_view text)
    : text_(text) {}

jwt_numeric_date::jwt_numeric_date(jwt_numeric_date&& other) noexcept
    : text_(std::move(other.text_)) {
    other.text_.clear();
}

jwt_numeric_date& jwt_numeric_date::operator=(jwt_numeric_date&& other) noexcept {
    if (this != &other) {
        text_ = std::move(other.text_);
        other.text_.clear();
    }
    return *this;
}

std::string_view jwt_numeric_date::text() const& noexcept {
    return text_.empty() ? std::string_view{"0"} : std::string_view{text_};
}

std::optional<std::chrono::system_clock::time_point> jwt_numeric_date::to_time_point() const noexcept {
    using clock_type = std::chrono::system_clock;
    auto date = detail::normalize_decimal(detail::decompose_validated_decimal(text()));
    if (date.digits_.empty()) {
        return clock_type::time_point{};
    }
    const int tick_order = date.order_ + detail::tick_decimal_places;
    if (tick_order <= 0 || tick_order > 19 || date.digit_count_ > static_cast<std::size_t>(tick_order)) {
        return std::nullopt;
    }
    std::uint64_t ticks = 0;
    for (int position = 0; position < tick_order; ++position) {
        ticks = ticks * 10 + static_cast<unsigned>(detail::next_digit(date.digits_) - '0');
    }
    constexpr auto minimum = std::numeric_limits<clock_type::rep>::min();
    constexpr auto maximum = std::numeric_limits<clock_type::rep>::max();
    const auto limit = date.negative_ ? detail::unsigned_magnitude(minimum) : static_cast<std::uint64_t>(maximum);
    if (ticks > limit) {
        return std::nullopt;
    }
    const auto count = date.negative_
                           ? (ticks == detail::unsigned_magnitude(minimum) ? minimum : -static_cast<clock_type::rep>(ticks))
                           : static_cast<clock_type::rep>(ticks);
    return clock_type::time_point{clock_type::duration{count}};
}

}  // namespace ruvia

#endif  // RUVIA_ENABLE_JWT
