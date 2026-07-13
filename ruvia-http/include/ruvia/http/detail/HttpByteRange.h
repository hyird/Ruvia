#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <variant>

#include "ruvia/http/detail/AsciiCase.h"

namespace ruvia::detail {

class HttpByteRangeResolution;

[[nodiscard]] inline HttpByteRangeResolution resolveHttpByteRange(
    std::string_view fieldValue,
    std::uint64_t representationLength) noexcept;

class HttpByteRangeIgnored final {
private:
    friend class HttpByteRangeResolution;

    constexpr HttpByteRangeIgnored() noexcept = default;
};

class HttpByteRangeUnsatisfiable final {
private:
    friend class HttpByteRangeResolution;

    constexpr HttpByteRangeUnsatisfiable() noexcept = default;
};

// A single byte range already resolved against the selected representation.
// Only this resolution alternative owns file/body slicing coordinates.
class HttpResolvedByteRange final {
public:
    [[nodiscard]] constexpr std::uint64_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }

private:
    friend class HttpByteRangeResolution;

    constexpr HttpResolvedByteRange(
        std::uint64_t offset,
        std::uint64_t length) noexcept
        : offset_(offset), length_(length) {
        if (length_ == 0 ||
            offset_ > (std::numeric_limits<std::uint64_t>::max)() - length_) {
            std::terminate();
        }
    }

    std::uint64_t offset_;
    std::uint64_t length_;
};

// A Range field has exactly one material outcome for a server that supports one
// byte range: ignore the field, emit 416, or serve one resolved nonempty slice.
// No status can be paired with default offset/length coordinates.
class HttpByteRangeResolution final {
public:
    [[nodiscard]] constexpr const HttpByteRangeIgnored* ignored() const noexcept {
        return std::get_if<HttpByteRangeIgnored>(&value_);
    }

    [[nodiscard]] constexpr const HttpByteRangeUnsatisfiable*
    unsatisfiable() const noexcept {
        return std::get_if<HttpByteRangeUnsatisfiable>(&value_);
    }

    [[nodiscard]] constexpr const HttpResolvedByteRange* resolved() const noexcept {
        return std::get_if<HttpResolvedByteRange>(&value_);
    }

private:
    friend HttpByteRangeResolution resolveHttpByteRange(
        std::string_view,
        std::uint64_t) noexcept;

    using Value = std::variant<
        HttpByteRangeIgnored,
        HttpByteRangeUnsatisfiable,
        HttpResolvedByteRange>;

    template <typename Alternative>
    explicit constexpr HttpByteRangeResolution(Alternative alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static constexpr HttpByteRangeResolution makeIgnored() noexcept {
        return HttpByteRangeResolution(HttpByteRangeIgnored());
    }

    [[nodiscard]] static constexpr HttpByteRangeResolution
    makeUnsatisfiable() noexcept {
        return HttpByteRangeResolution(HttpByteRangeUnsatisfiable());
    }

    [[nodiscard]] static constexpr HttpByteRangeResolution makeResolved(
        std::uint64_t offset,
        std::uint64_t length) noexcept {
        return HttpByteRangeResolution(HttpResolvedByteRange(offset, length));
    }

    Value value_;
};

[[nodiscard]] inline HttpByteRangeResolution resolveHttpByteRange(
    std::string_view fieldValue,
    std::uint64_t representationLength) noexcept {
    constexpr std::string_view unit = "bytes";
    constexpr std::size_t separatorOffset = unit.size();
    if (fieldValue.size() <= separatorOffset + 1 ||
        fieldValue[separatorOffset] != '=' ||
        !httpAsciiEqualsIgnoreCase(fieldValue.substr(0, separatorOffset), unit)) {
        return HttpByteRangeResolution::makeIgnored();
    }

    // RFC 9110 Section 14.2 explicitly permits ignoring Range when the selected
    // representation has no content. This avoids manufacturing a zero-length
    // 206 range, for which no valid byte Content-Range can be generated.
    if (representationLength == 0) {
        return HttpByteRangeResolution::makeIgnored();
    }

    const auto parseDecimal = [](std::string_view value) noexcept
        -> std::optional<std::uint64_t> {
        if (value.empty()) {
            return std::nullopt;
        }
        std::uint64_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(
            value.data(), value.data() + value.size(), parsed);
        if (ptr != value.data() + value.size()) {
            return std::nullopt;
        }
        if (ec == std::errc::result_out_of_range) {
            // The representation length itself is uint64_t. Saturating larger,
            // syntactically valid numerals preserves every relevant comparison:
            // huge starts are unsatisfiable; huge ends/suffixes clamp to the end.
            return (std::numeric_limits<std::uint64_t>::max)();
        }
        return ec == std::errc{}
            ? std::optional<std::uint64_t>(parsed)
            : std::nullopt;
    };

    const auto spec = fieldValue.substr(separatorOffset + 1);
    // This helper deliberately resolves one range. A valid range set requiring
    // multipart/byteranges is ignored as an unsupported server capability.
    if (spec.find(',') != std::string_view::npos) {
        return HttpByteRangeResolution::makeIgnored();
    }

    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) {
        return HttpByteRangeResolution::makeIgnored();
    }

    const auto first = spec.substr(0, dash);
    const auto last = spec.substr(dash + 1);
    if (first.empty()) {
        const auto suffix = parseDecimal(last);
        if (!suffix) {
            return HttpByteRangeResolution::makeIgnored();
        }
        if (*suffix == 0) {
            return HttpByteRangeResolution::makeUnsatisfiable();
        }
        const auto length = std::min(*suffix, representationLength);
        return HttpByteRangeResolution::makeResolved(
            representationLength - length, length);
    }

    const auto start = parseDecimal(first);
    if (!start) {
        return HttpByteRangeResolution::makeIgnored();
    }
    std::uint64_t end = 0;
    if (!last.empty()) {
        const auto parsedEnd = parseDecimal(last);
        if (!parsedEnd || *parsedEnd < *start) {
            return HttpByteRangeResolution::makeIgnored();
        }
        end = *parsedEnd;
    }

    if (*start >= representationLength) {
        return HttpByteRangeResolution::makeUnsatisfiable();
    }
    const auto clampedEnd = last.empty()
        ? representationLength - 1
        : std::min(end, representationLength - 1);
    return HttpByteRangeResolution::makeResolved(
        *start, clampedEnd - *start + 1);
}

}  // namespace ruvia::detail
