#include "auth/jwt_date.h"

#ifdef RUVIA_ENABLE_JWT

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <ratio>
#include <utility>

#include "ruvia/http/http_number_format.h"
#include "ruvia/web/auth/jwt.h"

#include "util/pmr_owned_storage.h"

namespace ruvia {
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

[[nodiscard]] std::optional<decimal_parts> scan_decimal(std::string_view text) noexcept {
    decimal_parts parts{.text_ = text};
    std::size_t position = 0;
    if (position < text.size() && text[position] == '-') {
        parts.negative_ = true;
        ++position;
    }
    if (position == text.size() || !is_digit(text[position])) {
        return std::nullopt;
    }
    const auto integer_begin = position;
    while (position < text.size() && is_digit(text[position])) {
        if (text[position] != '0') {
            if (parts.first_nonzero_ == std::string_view::npos) {
                parts.first_nonzero_ = position;
            }
            parts.last_nonzero_ = position;
        }
        ++position;
    }
    if (position - integer_begin > 1 && text[integer_begin] == '0') {
        return std::nullopt;
    }
    parts.integer_end_ = position;
    if (position < text.size() && text[position] == '.') {
        ++position;
        const auto fraction_begin = position;
        while (position < text.size() && is_digit(text[position])) {
            if (text[position] != '0') {
                if (parts.first_nonzero_ == std::string_view::npos) {
                    parts.first_nonzero_ = position;
                }
                parts.last_nonzero_ = position;
            }
            ++position;
        }
        if (position == fraction_begin) {
            return std::nullopt;
        }
    }
    if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
            parts.exponent_negative_ = text[position] == '-';
            ++position;
        }
        const auto exponent_begin = position;
        while (position < text.size() && text[position] == '0') {
            ++position;
        }
        const auto significant_begin = position;
        while (position < text.size() && is_digit(text[position])) {
            ++position;
        }
        if (position == exponent_begin) {
            return std::nullopt;
        }
        parts.exponent_ = text.substr(significant_begin, position - significant_begin);
    }
    if (position != text.size()) {
        return std::nullopt;
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

[[nodiscard]] std::string_view format_seconds(std::span<char, 21> output, std::uint64_t seconds, bool negative) noexcept {
    const std::size_t prefix = negative ? 1 : 0;
    if (negative) {
        output[0] = '-';
    }
    const auto digits = http_format_unsigned(std::span<char, http_max_unsigned_decimal_size>{output.data() + prefix, http_max_unsigned_decimal_size}, seconds);
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

}  // namespace

jwt_numeric_date::jwt_numeric_date(std::string_view text, std::pmr::memory_resource* resource)
    : text_(text, pmr_resource_or_default(resource)) {
}

jwt_numeric_date::jwt_numeric_date(jwt_numeric_date&& other) noexcept
    : text_(std::move(other.text_)) {
    other.text_.clear();
}

jwt_numeric_date& jwt_numeric_date::operator=(jwt_numeric_date&& other) {
    if (this != &other) {
        auto rebound = detail::rebind_owned_storage(other.text_, text_.get_allocator().resource());
        text_.swap(rebound);
        other.text_.clear();
    }
    return *this;
}

std::string_view jwt_numeric_date::text() const& noexcept {
    return text_.empty() ? std::string_view{"0"} : std::string_view{text_};
}

std::optional<clock_type::time_point> jwt_numeric_date::to_time_point() const noexcept {
    auto date = normalize_decimal(*scan_decimal(text()));
    if (date.digits_.empty()) {
        return clock_type::time_point{};
    }
    const int tick_order = date.order_ + tick_decimal_places;
    if (tick_order <= 0 || tick_order > 19 || date.digit_count_ > static_cast<std::size_t>(tick_order)) {
        return std::nullopt;
    }
    std::uint64_t ticks = 0;
    for (int position = 0; position < tick_order; ++position) {
        ticks = ticks * 10 + static_cast<unsigned>(next_digit(date.digits_) - '0');
    }
    constexpr auto minimum = std::numeric_limits<clock_type::rep>::min();
    constexpr auto maximum = std::numeric_limits<clock_type::rep>::max();
    const auto limit = date.negative_ ? unsigned_magnitude(minimum) : static_cast<std::uint64_t>(maximum);
    if (ticks > limit) {
        return std::nullopt;
    }
    const auto count = date.negative_ ? (ticks == unsigned_magnitude(minimum) ? minimum : -static_cast<clock_type::rep>(ticks)) : static_cast<clock_type::rep>(ticks);
    return clock_type::time_point{clock_type::duration{count}};
}

namespace detail {

bool jwt_is_numeric_date(std::string_view text) noexcept {
    return scan_decimal(text).has_value();
}

std::strong_ordering jwt_compare_numeric_date(std::string_view date, clock_type::time_point now, std::chrono::seconds offset) noexcept {
    std::array<char, 40> buffer;
    return compare_decimal(
        normalize_decimal(*scan_decimal(date)), format_clock_decimal(buffer, offset_clock(now, offset)));
}

std::string_view jwt_signing_date(std::span<char, 21> output, clock_type::time_point now, std::chrono::seconds offset) noexcept {
    auto parts = offset_clock(now, offset);
    // Integer offsets preserve the fraction, so rounding the exact sum is
    // equivalent to rounding now first, without ever constructing host ticks.
    if (parts.fraction_ != 0 && (offset.count() == 0 ? parts.negative_ : !parts.negative_)) {
        ++parts.seconds_;
    }
    return format_seconds(output, parts.seconds_, parts.negative_ && parts.seconds_ != 0);
}

}  // namespace detail
}  // namespace ruvia

#endif  // RUVIA_ENABLE_JWT
