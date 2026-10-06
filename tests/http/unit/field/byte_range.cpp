#include <cstdint>
#include <string>
#include <string_view>

#include "ruvia/http/HttpByteRange.h"

#include "test_harness.h"

namespace {

using ruvia::http_byte_range_set;
using ruvia::resolve_http_byte_range_set;

[[nodiscard]] bool is_ignored_range(std::string_view value, std::uint64_t representation_length) {
    return resolve_http_byte_range_set(value, representation_length).ignored();
}

[[nodiscard]] bool is_unsatisfiable_range(
    std::string_view value, std::uint64_t representation_length) {
    return resolve_http_byte_range_set(value, representation_length).unsatisfiable();
}

}  // namespace

RUVIA_TEST(byte_range_set_discriminates_outcomes) {
    const auto resolved = resolve_http_byte_range_set("bytes=10-19", 100);
    RUVIA_CHECK(!resolved.ignored());
    RUVIA_CHECK(!resolved.unsatisfiable());
    RUVIA_CHECK_EQ(resolved.size(), std::size_t{1});
    if (resolved.size() == 1) {
        const auto range = resolved.resolved_range(0);
        RUVIA_CHECK_EQ(range.offset(), std::uint64_t{10});
        RUVIA_CHECK_EQ(range.length(), std::uint64_t{10});
    }

    const auto ignored = resolve_http_byte_range_set("items=10-19", 100);
    RUVIA_CHECK(ignored.ignored());
    RUVIA_CHECK_EQ(ignored.size(), std::size_t{0});

    const auto unsatisfiable = resolve_http_byte_range_set("bytes=100-", 100);
    RUVIA_CHECK(!unsatisfiable.ignored());
    RUVIA_CHECK(unsatisfiable.unsatisfiable());
    RUVIA_CHECK_EQ(unsatisfiable.size(), std::size_t{0});
    [[maybe_unused]] const auto outcome = unsatisfiable.unsatisfiable_outcome();
}

RUVIA_TEST(byte_range_set_bounded_and_open_ended) {
    const auto bounded = resolve_http_byte_range_set("bytes=100-199", 1000);
    RUVIA_CHECK_EQ(bounded.size(), std::size_t{1});
    if (bounded.size() == 1) {
        const auto range = bounded.resolved_range(0);
        RUVIA_CHECK_EQ(range.offset(), std::uint64_t{100});
        RUVIA_CHECK_EQ(range.length(), std::uint64_t{100});
    }

    const auto open = resolve_http_byte_range_set("bytes=500-", 1000);
    RUVIA_CHECK_EQ(open.size(), std::size_t{1});
    if (open.size() == 1) {
        const auto range = open.resolved_range(0);
        RUVIA_CHECK_EQ(range.offset(), std::uint64_t{500});
        RUVIA_CHECK_EQ(range.length(), std::uint64_t{500});
    }

    const auto last_byte = resolve_http_byte_range_set("bytes=999-999", 1000);
    RUVIA_CHECK_EQ(last_byte.size(), std::size_t{1});
    if (last_byte.size() == 1) {
        RUVIA_CHECK_EQ(last_byte.resolved_range(0).offset(), std::uint64_t{999});
        RUVIA_CHECK_EQ(last_byte.resolved_range(0).length(), std::uint64_t{1});
    }

    const auto clamped_end = resolve_http_byte_range_set("bytes=0-2000", 1000);
    RUVIA_CHECK_EQ(clamped_end.size(), std::size_t{1});
    if (clamped_end.size() == 1) {
        RUVIA_CHECK_EQ(clamped_end.resolved_range(0).offset(), std::uint64_t{0});
        RUVIA_CHECK_EQ(clamped_end.resolved_range(0).length(), std::uint64_t{1000});
    }
}

RUVIA_TEST(byte_range_set_suffix) {
    const auto suffix = resolve_http_byte_range_set("bytes=-100", 1000);
    RUVIA_CHECK_EQ(suffix.size(), std::size_t{1});
    if (suffix.size() == 1) {
        RUVIA_CHECK_EQ(suffix.resolved_range(0).offset(), std::uint64_t{900});
        RUVIA_CHECK_EQ(suffix.resolved_range(0).length(), std::uint64_t{100});
    }

    const auto whole = resolve_http_byte_range_set("bytes=-2000", 1000);
    RUVIA_CHECK_EQ(whole.size(), std::size_t{1});
    if (whole.size() == 1) {
        RUVIA_CHECK_EQ(whole.resolved_range(0).offset(), std::uint64_t{0});
        RUVIA_CHECK_EQ(whole.resolved_range(0).length(), std::uint64_t{1000});
    }
}

RUVIA_TEST(byte_range_set_unsatisfiable_is_payload_free) {
    RUVIA_CHECK(is_unsatisfiable_range("bytes=1000-", 1000));
    RUVIA_CHECK(is_unsatisfiable_range("bytes=1500-1600", 1000));
    RUVIA_CHECK(is_unsatisfiable_range("bytes=-0", 1000));
}

RUVIA_TEST(byte_range_set_ignores_invalid_and_unknown_values) {
    RUVIA_CHECK(is_ignored_range("items=0-99", 1000));
    RUVIA_CHECK(is_ignored_range("0-99", 1000));
    RUVIA_CHECK(is_ignored_range("bytes=500-100", 1000));
    RUVIA_CHECK(is_ignored_range("bytes=abc", 1000));
    RUVIA_CHECK(is_ignored_range("bytes=x-9", 1000));
    RUVIA_CHECK(is_ignored_range("bytes=0-x", 1000));
    RUVIA_CHECK(is_ignored_range("bytes=-x", 1000));
    RUVIA_CHECK(is_ignored_range("bytes=-", 1000));
    RUVIA_CHECK(is_ignored_range("bytes=", 1000));
}

RUVIA_TEST(byte_range_set_supports_multiple_ranges) {
    const auto ranges = resolve_http_byte_range_set("bytes=0-99,200-299", 1000);
    RUVIA_CHECK(!ranges.ignored());
    RUVIA_CHECK(!ranges.unsatisfiable());
    RUVIA_CHECK_EQ(ranges.size(), std::size_t{2});
    if (ranges.size() == 2) {
        RUVIA_CHECK_EQ(ranges.resolved_range(0).offset(), std::uint64_t{0});
        RUVIA_CHECK_EQ(ranges.resolved_range(0).length(), std::uint64_t{100});
        RUVIA_CHECK_EQ(ranges.resolved_range(1).offset(), std::uint64_t{200});
        RUVIA_CHECK_EQ(ranges.resolved_range(1).length(), std::uint64_t{100});
    }
}

RUVIA_TEST(byte_range_set_unit_is_case_insensitive) {
    const auto title_case = resolve_http_byte_range_set("Bytes=0-9", 100);
    const auto upper_case = resolve_http_byte_range_set("BYTES=90-", 100);
    RUVIA_CHECK_EQ(title_case.size(), std::size_t{1});
    RUVIA_CHECK_EQ(upper_case.size(), std::size_t{1});
    if (title_case.size() == 1) {
        RUVIA_CHECK_EQ(title_case.resolved_range(0).offset(), std::uint64_t{0});
        RUVIA_CHECK_EQ(title_case.resolved_range(0).length(), std::uint64_t{10});
    }
    if (upper_case.size() == 1) {
        RUVIA_CHECK_EQ(upper_case.resolved_range(0).offset(), std::uint64_t{90});
        RUVIA_CHECK_EQ(upper_case.resolved_range(0).length(), std::uint64_t{10});
    }
}

RUVIA_TEST(byte_range_set_allows_ows_after_equals) {
    const auto spaced = resolve_http_byte_range_set("bytes= \t10-19", 100);
    RUVIA_CHECK_EQ(spaced.size(), std::size_t{1});
    if (spaced.size() == 1) {
        RUVIA_CHECK_EQ(spaced.resolved_range(0).offset(), std::uint64_t{10});
        RUVIA_CHECK_EQ(spaced.resolved_range(0).length(), std::uint64_t{10});
    }
}

RUVIA_TEST(byte_range_set_trims_field_value_ows) {
    const auto trailing = resolve_http_byte_range_set("bytes=10-19 \t", 100);
    const auto wrapped = resolve_http_byte_range_set(" \tbytes=20-29\t ", 100);
    RUVIA_CHECK_EQ(trailing.size(), std::size_t{1});
    RUVIA_CHECK_EQ(wrapped.size(), std::size_t{1});
    if (trailing.size() == 1) {
        RUVIA_CHECK_EQ(trailing.resolved_range(0).offset(), std::uint64_t{10});
        RUVIA_CHECK_EQ(trailing.resolved_range(0).length(), std::uint64_t{10});
    }
    if (wrapped.size() == 1) {
        RUVIA_CHECK_EQ(wrapped.resolved_range(0).offset(), std::uint64_t{20});
        RUVIA_CHECK_EQ(wrapped.resolved_range(0).length(), std::uint64_t{10});
    }
}

RUVIA_TEST(byte_range_set_huge_decimal_numerals_preserve_semantics) {
    const auto huge_start = resolve_http_byte_range_set("bytes=184467440737095516160-", 1000);
    RUVIA_CHECK(huge_start.unsatisfiable());

    const auto huge_end = resolve_http_byte_range_set("bytes=0-184467440737095516160", 1000);
    RUVIA_CHECK_EQ(huge_end.size(), std::size_t{1});
    if (huge_end.size() == 1) {
        RUVIA_CHECK_EQ(huge_end.resolved_range(0).offset(), std::uint64_t{0});
        RUVIA_CHECK_EQ(huge_end.resolved_range(0).length(), std::uint64_t{1000});
    }

    const auto huge_suffix = resolve_http_byte_range_set("bytes=-184467440737095516160", 1000);
    RUVIA_CHECK_EQ(huge_suffix.size(), std::size_t{1});
    if (huge_suffix.size() == 1) {
        RUVIA_CHECK_EQ(huge_suffix.resolved_range(0).offset(), std::uint64_t{0});
        RUVIA_CHECK_EQ(huge_suffix.resolved_range(0).length(), std::uint64_t{1000});
    }

    RUVIA_CHECK(is_ignored_range("bytes=184467440737095516160x-", 1000));
    RUVIA_CHECK(is_ignored_range("bytes=184467440737095516160-184467440737095516159", 1000));
}

RUVIA_TEST(byte_range_set_preserves_order_and_merges_only_adjacent_neighbors) {
    const auto ranges = resolve_http_byte_range_set("bytes=20-29,0-9,10-19,18-22,90-", 100);
    RUVIA_CHECK(!ranges.ignored());
    RUVIA_CHECK(!ranges.unsatisfiable());
    RUVIA_CHECK_EQ(ranges.size(), std::size_t{3});
    if (ranges.size() == 3) {
        RUVIA_CHECK_EQ(ranges[0].offset_, std::uint64_t{20});
        RUVIA_CHECK_EQ(ranges[0].length_, std::uint64_t{10});
        RUVIA_CHECK_EQ(ranges[1].offset_, std::uint64_t{0});
        RUVIA_CHECK_EQ(ranges[1].length_, std::uint64_t{23});
        RUVIA_CHECK_EQ(ranges[2].offset_, std::uint64_t{90});
        RUVIA_CHECK_EQ(ranges[2].length_, std::uint64_t{10});
    }
    const auto partial = resolve_http_byte_range_set("bytes=0-1,900-", 100);
    RUVIA_CHECK_EQ(partial.size(), std::size_t{1});
    const auto none = resolve_http_byte_range_set("bytes=100-,200-", 100);
    RUVIA_CHECK(none.unsatisfiable());
}

RUVIA_TEST(byte_range_set_tolerates_bounded_empty_members) {
    const auto ranges = resolve_http_byte_range_set("bytes=,0-1,,2-3,", 100);
    RUVIA_CHECK_EQ(ranges.size(), std::size_t{1});
    if (ranges.size() == 1) {
        RUVIA_CHECK_EQ(ranges[0].offset_, std::uint64_t{0});
        RUVIA_CHECK_EQ(ranges[0].length_, std::uint64_t{4});
    }
    RUVIA_CHECK(resolve_http_byte_range_set("bytes=,,", 100).ignored());
    RUVIA_CHECK_EQ(resolve_http_byte_range_set("bytes=0-1,,3-4", 100).size(), std::size_t{2});
    RUVIA_CHECK(resolve_http_byte_range_set(
        "bytes=0-1,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,", 100)
            .ignored());
}

RUVIA_TEST(byte_range_set_counts_only_received_empty_members) {
    const auto prefix = std::string("bytes=") + std::string(32, ',');
    for (const auto tail : {"100-", "100-199", "-0"}) {
        const auto ranges = resolve_http_byte_range_set(prefix + tail, 100);
        RUVIA_CHECK(!ranges.ignored());
        RUVIA_CHECK(ranges.unsatisfiable());
    }
    const auto satisfiable = resolve_http_byte_range_set(prefix + "0-1", 100);
    RUVIA_CHECK_EQ(satisfiable.size(), std::size_t{1});
    if (satisfiable.size() == 1) {
        RUVIA_CHECK_EQ(satisfiable[0].offset_, std::uint64_t{0});
        RUVIA_CHECK_EQ(satisfiable[0].length_, std::uint64_t{2});
    }
    RUVIA_CHECK(resolve_http_byte_range_set(prefix + ",100-", 100).ignored());
    RUVIA_CHECK(resolve_http_byte_range_set(prefix + ",0-1", 100).ignored());
}

RUVIA_TEST(byte_range_set_ignores_malformed_and_over_limit_sets) {
    std::string ranges = "bytes=";
    for (std::size_t index = 0; index <= http_byte_range_set::capacity; ++index) {
        if (index != 0) {
            ranges.push_back(',');
        }
        ranges.append(std::to_string(index * 2));
        ranges.push_back('-');
        ranges.append(std::to_string(index * 2));
    }
    RUVIA_CHECK(resolve_http_byte_range_set(ranges, 100).ignored());
}

RUVIA_TEST(byte_range_set_empty_representation_uses_ignore_policy) {
    RUVIA_CHECK(is_ignored_range("bytes=0-99", 0));
    RUVIA_CHECK(is_ignored_range("bytes=-1", 0));
    RUVIA_CHECK(is_ignored_range("BYTES=0-", 0));
}
