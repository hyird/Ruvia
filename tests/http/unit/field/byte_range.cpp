#include "test_harness.h"

#include <concepts>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/field/HttpByteRange.h"

namespace {

using ruvia::detail::HttpByteRangeIgnored;
using ruvia::detail::HttpByteRangeResolution;
using ruvia::detail::HttpByteRangeUnsatisfiable;
using ruvia::detail::HttpResolvedByteRange;
using ruvia::detail::resolveHttpByteRange;

template <typename T>
concept HasByteRangeOutcomeField = requires(const T& value) { value.outcome; };

template <typename T>
concept HasByteRangePayloadField = requires(const T& value) { value.range; };

template <typename T>
concept HasByteRangeOffsetAccessor = requires(const T& value) {
    { value.offset() } -> std::same_as<std::uint64_t>;
};

template <typename T>
concept HasByteRangeLengthAccessor = requires(const T& value) {
    { value.length() } -> std::same_as<std::uint64_t>;
};

template <typename T>
concept HasAnyRvalueByteRangeResolutionAccessor =
    requires(T&& value) { std::move(value).ignored(); } || requires(T&& value) {
        std::move(value).unsatisfiable();
    } || requires(T&& value) { std::move(value).resolved(); };

[[nodiscard]] bool isIgnoredRange(std::string_view value, std::uint64_t representationLength) {
    const auto resolution = resolveHttpByteRange(value, representationLength);
    return resolution.ignored() != nullptr;
}

[[nodiscard]] bool isUnsatisfiableRange(
    std::string_view value, std::uint64_t representationLength) {
    const auto resolution = resolveHttpByteRange(value, representationLength);
    return resolution.unsatisfiable() != nullptr;
}

static_assert(!std::default_initializable<HttpByteRangeResolution>);
static_assert(!HasAnyRvalueByteRangeResolutionAccessor<HttpByteRangeResolution>);
static_assert(std::same_as<decltype(std::declval<const HttpByteRangeResolution&>().ignored()),
    const HttpByteRangeIgnored*>);
static_assert(std::same_as<decltype(std::declval<const HttpByteRangeResolution&>().unsatisfiable()),
    const HttpByteRangeUnsatisfiable*>);
static_assert(std::same_as<decltype(std::declval<const HttpByteRangeResolution&>().resolved()),
    const HttpResolvedByteRange*>);
static_assert(!HasByteRangeOutcomeField<HttpByteRangeResolution>);
static_assert(!HasByteRangePayloadField<HttpByteRangeResolution>);
static_assert(!HasByteRangeOffsetAccessor<HttpByteRangeResolution>);
static_assert(!HasByteRangeLengthAccessor<HttpByteRangeResolution>);
static_assert(!HasByteRangeOffsetAccessor<HttpByteRangeIgnored>);
static_assert(!HasByteRangeLengthAccessor<HttpByteRangeIgnored>);
static_assert(!HasByteRangeOffsetAccessor<HttpByteRangeUnsatisfiable>);
static_assert(!HasByteRangeLengthAccessor<HttpByteRangeUnsatisfiable>);
static_assert(HasByteRangeOffsetAccessor<HttpResolvedByteRange>);
static_assert(HasByteRangeLengthAccessor<HttpResolvedByteRange>);
static_assert(!std::default_initializable<HttpByteRangeIgnored>);
static_assert(!std::default_initializable<HttpByteRangeUnsatisfiable>);
static_assert(!std::default_initializable<HttpResolvedByteRange>);
static_assert(!std::constructible_from<HttpResolvedByteRange, std::uint64_t, std::uint64_t>);

}  // namespace

RUVIA_TEST(byte_range_resolution_is_discriminated) {
    const auto resolved = resolveHttpByteRange("bytes=10-19", 100);
    RUVIA_CHECK(resolved.ignored() == nullptr);
    RUVIA_CHECK(resolved.unsatisfiable() == nullptr);
    RUVIA_CHECK(resolved.resolved() != nullptr);

    const auto ignored = resolveHttpByteRange("items=10-19", 100);
    RUVIA_CHECK(ignored.ignored() != nullptr);
    RUVIA_CHECK(ignored.unsatisfiable() == nullptr);
    RUVIA_CHECK(ignored.resolved() == nullptr);

    const auto unsatisfiable = resolveHttpByteRange("bytes=100-", 100);
    RUVIA_CHECK(unsatisfiable.ignored() == nullptr);
    RUVIA_CHECK(unsatisfiable.unsatisfiable() != nullptr);
    RUVIA_CHECK(unsatisfiable.resolved() == nullptr);
}

RUVIA_TEST(byte_range_bounded_and_open_ended) {
    const auto bounded = resolveHttpByteRange("bytes=100-199", 1000);
    const auto* boundedRange = bounded.resolved();
    RUVIA_CHECK(boundedRange != nullptr);
    if (boundedRange != nullptr) {
        RUVIA_CHECK_EQ(boundedRange->offset(), std::uint64_t{100});
        RUVIA_CHECK_EQ(boundedRange->length(), std::uint64_t{100});
    }

    const auto open = resolveHttpByteRange("bytes=500-", 1000);
    const auto* openRange = open.resolved();
    RUVIA_CHECK(openRange != nullptr);
    if (openRange != nullptr) {
        RUVIA_CHECK_EQ(openRange->offset(), std::uint64_t{500});
        RUVIA_CHECK_EQ(openRange->length(), std::uint64_t{500});
    }

    const auto lastByte = resolveHttpByteRange("bytes=999-999", 1000);
    const auto* lastByteRange = lastByte.resolved();
    RUVIA_CHECK(lastByteRange != nullptr);
    if (lastByteRange != nullptr) {
        RUVIA_CHECK_EQ(lastByteRange->offset(), std::uint64_t{999});
        RUVIA_CHECK_EQ(lastByteRange->length(), std::uint64_t{1});
    }

    const auto clampedEnd = resolveHttpByteRange("bytes=0-2000", 1000);
    const auto* clampedRange = clampedEnd.resolved();
    RUVIA_CHECK(clampedRange != nullptr);
    if (clampedRange != nullptr) {
        RUVIA_CHECK_EQ(clampedRange->offset(), std::uint64_t{0});
        RUVIA_CHECK_EQ(clampedRange->length(), std::uint64_t{1000});
    }
}

RUVIA_TEST(byte_range_suffix) {
    const auto suffix = resolveHttpByteRange("bytes=-100", 1000);
    const auto* suffixRange = suffix.resolved();
    RUVIA_CHECK(suffixRange != nullptr);
    if (suffixRange != nullptr) {
        RUVIA_CHECK_EQ(suffixRange->offset(), std::uint64_t{900});
        RUVIA_CHECK_EQ(suffixRange->length(), std::uint64_t{100});
    }

    const auto whole = resolveHttpByteRange("bytes=-2000", 1000);
    const auto* wholeRange = whole.resolved();
    RUVIA_CHECK(wholeRange != nullptr);
    if (wholeRange != nullptr) {
        RUVIA_CHECK_EQ(wholeRange->offset(), std::uint64_t{0});
        RUVIA_CHECK_EQ(wholeRange->length(), std::uint64_t{1000});
    }
}

RUVIA_TEST(byte_range_unsatisfiable_is_payload_free) {
    RUVIA_CHECK(isUnsatisfiableRange("bytes=1000-", 1000));
    RUVIA_CHECK(isUnsatisfiableRange("bytes=1500-1600", 1000));
    RUVIA_CHECK(isUnsatisfiableRange("bytes=-0", 1000));
}

RUVIA_TEST(byte_range_invalid_unknown_and_multiple_are_ignored) {
    // Unknown units MUST be ignored. This single-range resolver also chooses the
    // RFC-permitted ignore policy for invalid or unsupported range sets.
    RUVIA_CHECK(isIgnoredRange("items=0-99", 1000));
    RUVIA_CHECK(isIgnoredRange("0-99", 1000));
    RUVIA_CHECK(isIgnoredRange("bytes=500-100", 1000));
    RUVIA_CHECK(isIgnoredRange("bytes=abc", 1000));
    RUVIA_CHECK(isIgnoredRange("bytes=x-9", 1000));
    RUVIA_CHECK(isIgnoredRange("bytes=0-x", 1000));
    RUVIA_CHECK(isIgnoredRange("bytes=-x", 1000));
    RUVIA_CHECK(isIgnoredRange("bytes=-", 1000));
    RUVIA_CHECK(isIgnoredRange("bytes=", 1000));
    RUVIA_CHECK(isIgnoredRange("bytes=0-99,200-299", 1000));
}

RUVIA_TEST(byte_range_unit_is_case_insensitive) {
    const auto titleCase = resolveHttpByteRange("Bytes=0-9", 100);
    const auto upperCase = resolveHttpByteRange("BYTES=90-", 100);
    RUVIA_CHECK(titleCase.resolved() != nullptr);
    RUVIA_CHECK(upperCase.resolved() != nullptr);
    if (titleCase.resolved() != nullptr) {
        RUVIA_CHECK_EQ(titleCase.resolved()->offset(), std::uint64_t{0});
        RUVIA_CHECK_EQ(titleCase.resolved()->length(), std::uint64_t{10});
    }
    if (upperCase.resolved() != nullptr) {
        RUVIA_CHECK_EQ(upperCase.resolved()->offset(), std::uint64_t{90});
        RUVIA_CHECK_EQ(upperCase.resolved()->length(), std::uint64_t{10});
    }
}

RUVIA_TEST(byte_range_allows_ows_after_equals) {
    const auto spaced = resolveHttpByteRange("bytes= \t10-19", 100);
    const auto* range = spaced.resolved();
    RUVIA_CHECK(range != nullptr);
    if (range != nullptr) {
        RUVIA_CHECK_EQ(range->offset(), std::uint64_t{10});
        RUVIA_CHECK_EQ(range->length(), std::uint64_t{10});
    }
}

RUVIA_TEST(byte_range_trims_field_value_ows) {
    const auto trailing = resolveHttpByteRange("bytes=10-19 \t", 100);
    const auto wrapped = resolveHttpByteRange(" \tbytes=20-29\t ", 100);
    const auto* trailingRange = trailing.resolved();
    const auto* wrappedRange = wrapped.resolved();
    RUVIA_CHECK(trailingRange != nullptr);
    RUVIA_CHECK(wrappedRange != nullptr);
    if (trailingRange != nullptr) {
        RUVIA_CHECK_EQ(trailingRange->offset(), std::uint64_t{10});
        RUVIA_CHECK_EQ(trailingRange->length(), std::uint64_t{10});
    }
    if (wrappedRange != nullptr) {
        RUVIA_CHECK_EQ(wrappedRange->offset(), std::uint64_t{20});
        RUVIA_CHECK_EQ(wrappedRange->length(), std::uint64_t{10});
    }
}

RUVIA_TEST(byte_range_huge_decimal_numerals_preserve_semantics) {
    // RFC 9110 §14.1.2 requires recipients to prevent conversion overflow.
    // Numerals beyond uint64_t still have obvious semantics against a uint64_t
    // representation: a huge start is outside it, while huge ends/suffixes clamp.
    const auto hugeStart = resolveHttpByteRange("bytes=184467440737095516160-", 1000);
    RUVIA_CHECK(hugeStart.unsatisfiable() != nullptr);

    const auto hugeEnd = resolveHttpByteRange("bytes=0-184467440737095516160", 1000);
    const auto* hugeEndRange = hugeEnd.resolved();
    RUVIA_CHECK(hugeEndRange != nullptr);
    if (hugeEndRange != nullptr) {
        RUVIA_CHECK_EQ(hugeEndRange->offset(), std::uint64_t{0});
        RUVIA_CHECK_EQ(hugeEndRange->length(), std::uint64_t{1000});
    }

    const auto hugeSuffix = resolveHttpByteRange("bytes=-184467440737095516160", 1000);
    const auto* hugeSuffixRange = hugeSuffix.resolved();
    RUVIA_CHECK(hugeSuffixRange != nullptr);
    if (hugeSuffixRange != nullptr) {
        RUVIA_CHECK_EQ(hugeSuffixRange->offset(), std::uint64_t{0});
        RUVIA_CHECK_EQ(hugeSuffixRange->length(), std::uint64_t{1000});
    }

    RUVIA_CHECK(isIgnoredRange("bytes=184467440737095516160x-", 1000));

    // Saturating both numerals must not erase their relative order. This is an
    // invalid int-range (last-pos < first-pos), not a valid unsatisfiable range.
    RUVIA_CHECK(isIgnoredRange("bytes=184467440737095516160-184467440737095516159", 1000));
}

RUVIA_TEST(byte_range_empty_representation_uses_ignore_policy) {
    // RFC 9110 §14.2 permits ignoring Range when the representation has no
    // content. This prevents a 206 with an impossible zero-length Content-Range.
    RUVIA_CHECK(isIgnoredRange("bytes=0-99", 0));
    RUVIA_CHECK(isIgnoredRange("bytes=-1", 0));
    RUVIA_CHECK(isIgnoredRange("BYTES=0-", 0));
}
