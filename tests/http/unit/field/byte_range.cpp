#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/http_byte_range.h"

#include "test_exceptions.h"

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

TEST(Ruvia, byte_range_set_discriminates_outcomes) {
    const auto resolved = resolve_http_byte_range_set("bytes=10-19", 100);
    EXPECT_TRUE(!resolved.ignored());
    EXPECT_TRUE(!resolved.unsatisfiable());
    EXPECT_EQ(resolved.size(), std::size_t{1});
    if (resolved.size() == 1) {
        const auto range = resolved[0];
        EXPECT_EQ(range.offset_, std::uint64_t{10});
        EXPECT_EQ(range.length_, std::uint64_t{10});
    }

    const auto ignored = resolve_http_byte_range_set("items=10-19", 100);
    EXPECT_TRUE(ignored.ignored());
    EXPECT_EQ(ignored.size(), std::size_t{0});

    const auto unsatisfiable = resolve_http_byte_range_set("bytes=100-", 100);
    EXPECT_TRUE(!unsatisfiable.ignored());
    EXPECT_TRUE(unsatisfiable.unsatisfiable());
    EXPECT_EQ(unsatisfiable.size(), std::size_t{0});
}

TEST(Ruvia, byte_range_set_bounded_and_open_ended) {
    const auto bounded = resolve_http_byte_range_set("bytes=100-199", 1000);
    EXPECT_EQ(bounded.size(), std::size_t{1});
    if (bounded.size() == 1) {
        const auto range = bounded[0];
        EXPECT_EQ(range.offset_, std::uint64_t{100});
        EXPECT_EQ(range.length_, std::uint64_t{100});
    }

    const auto open = resolve_http_byte_range_set("bytes=500-", 1000);
    EXPECT_EQ(open.size(), std::size_t{1});
    if (open.size() == 1) {
        const auto range = open[0];
        EXPECT_EQ(range.offset_, std::uint64_t{500});
        EXPECT_EQ(range.length_, std::uint64_t{500});
    }

    const auto last_byte = resolve_http_byte_range_set("bytes=999-999", 1000);
    EXPECT_EQ(last_byte.size(), std::size_t{1});
    if (last_byte.size() == 1) {
        EXPECT_EQ(last_byte[0].offset_, std::uint64_t{999});
        EXPECT_EQ(last_byte[0].length_, std::uint64_t{1});
    }

    const auto clamped_end = resolve_http_byte_range_set("bytes=0-2000", 1000);
    EXPECT_EQ(clamped_end.size(), std::size_t{1});
    if (clamped_end.size() == 1) {
        EXPECT_EQ(clamped_end[0].offset_, std::uint64_t{0});
        EXPECT_EQ(clamped_end[0].length_, std::uint64_t{1000});
    }
}

TEST(Ruvia, byte_range_set_suffix) {
    const auto suffix = resolve_http_byte_range_set("bytes=-100", 1000);
    EXPECT_EQ(suffix.size(), std::size_t{1});
    if (suffix.size() == 1) {
        EXPECT_EQ(suffix[0].offset_, std::uint64_t{900});
        EXPECT_EQ(suffix[0].length_, std::uint64_t{100});
    }

    const auto whole = resolve_http_byte_range_set("bytes=-2000", 1000);
    EXPECT_EQ(whole.size(), std::size_t{1});
    if (whole.size() == 1) {
        EXPECT_EQ(whole[0].offset_, std::uint64_t{0});
        EXPECT_EQ(whole[0].length_, std::uint64_t{1000});
    }
}

TEST(Ruvia, byte_range_set_unsatisfiable_is_payload_free) {
    EXPECT_TRUE(is_unsatisfiable_range("bytes=1000-", 1000));
    EXPECT_TRUE(is_unsatisfiable_range("bytes=1500-1600", 1000));
    EXPECT_TRUE(is_unsatisfiable_range("bytes=-0", 1000));
}

TEST(Ruvia, byte_range_set_ignores_invalid_and_unknown_values) {
    EXPECT_TRUE(is_ignored_range("items=0-99", 1000));
    EXPECT_TRUE(is_ignored_range("0-99", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=500-100", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=abc", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=x-9", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=0-x", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=-x", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=-", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=", 1000));
}

TEST(Ruvia, byte_range_set_supports_multiple_ranges) {
    const auto ranges = resolve_http_byte_range_set("bytes=0-99,200-299", 1000);
    EXPECT_TRUE(!ranges.ignored());
    EXPECT_TRUE(!ranges.unsatisfiable());
    EXPECT_EQ(ranges.size(), std::size_t{2});
    if (ranges.size() == 2) {
        EXPECT_EQ(ranges[0].offset_, std::uint64_t{0});
        EXPECT_EQ(ranges[0].length_, std::uint64_t{100});
        EXPECT_EQ(ranges[1].offset_, std::uint64_t{200});
        EXPECT_EQ(ranges[1].length_, std::uint64_t{100});
    }
}

TEST(Ruvia, byte_range_set_unit_is_case_insensitive) {
    const auto title_case = resolve_http_byte_range_set("Bytes=0-9", 100);
    const auto upper_case = resolve_http_byte_range_set("BYTES=90-", 100);
    EXPECT_EQ(title_case.size(), std::size_t{1});
    EXPECT_EQ(upper_case.size(), std::size_t{1});
    if (title_case.size() == 1) {
        EXPECT_EQ(title_case[0].offset_, std::uint64_t{0});
        EXPECT_EQ(title_case[0].length_, std::uint64_t{10});
    }
    if (upper_case.size() == 1) {
        EXPECT_EQ(upper_case[0].offset_, std::uint64_t{90});
        EXPECT_EQ(upper_case[0].length_, std::uint64_t{10});
    }
}

TEST(Ruvia, byte_range_set_allows_ows_after_equals) {
    const auto spaced = resolve_http_byte_range_set("bytes= \t10-19", 100);
    EXPECT_EQ(spaced.size(), std::size_t{1});
    if (spaced.size() == 1) {
        EXPECT_EQ(spaced[0].offset_, std::uint64_t{10});
        EXPECT_EQ(spaced[0].length_, std::uint64_t{10});
    }
}

TEST(Ruvia, byte_range_set_trims_field_value_ows) {
    const auto trailing = resolve_http_byte_range_set("bytes=10-19 \t", 100);
    const auto wrapped = resolve_http_byte_range_set(" \tbytes=20-29\t ", 100);
    EXPECT_EQ(trailing.size(), std::size_t{1});
    EXPECT_EQ(wrapped.size(), std::size_t{1});
    if (trailing.size() == 1) {
        EXPECT_EQ(trailing[0].offset_, std::uint64_t{10});
        EXPECT_EQ(trailing[0].length_, std::uint64_t{10});
    }
    if (wrapped.size() == 1) {
        EXPECT_EQ(wrapped[0].offset_, std::uint64_t{20});
        EXPECT_EQ(wrapped[0].length_, std::uint64_t{10});
    }
}

TEST(Ruvia, byte_range_set_huge_decimal_numerals_preserve_semantics) {
    const auto huge_start = resolve_http_byte_range_set("bytes=184467440737095516160-", 1000);
    EXPECT_TRUE(huge_start.unsatisfiable());

    const auto huge_end = resolve_http_byte_range_set("bytes=0-184467440737095516160", 1000);
    EXPECT_EQ(huge_end.size(), std::size_t{1});
    if (huge_end.size() == 1) {
        EXPECT_EQ(huge_end[0].offset_, std::uint64_t{0});
        EXPECT_EQ(huge_end[0].length_, std::uint64_t{1000});
    }

    const auto huge_suffix = resolve_http_byte_range_set("bytes=-184467440737095516160", 1000);
    EXPECT_EQ(huge_suffix.size(), std::size_t{1});
    if (huge_suffix.size() == 1) {
        EXPECT_EQ(huge_suffix[0].offset_, std::uint64_t{0});
        EXPECT_EQ(huge_suffix[0].length_, std::uint64_t{1000});
    }

    EXPECT_TRUE(is_ignored_range("bytes=184467440737095516160x-", 1000));
    EXPECT_TRUE(is_ignored_range("bytes=184467440737095516160-184467440737095516159", 1000));
}

TEST(Ruvia, byte_range_set_preserves_disjoint_order_and_coalesces_touching_ranges) {
    const auto ranges = resolve_http_byte_range_set("bytes=50-59,0-9,20-29,90-", 100);
    EXPECT_EQ(ranges.size(), std::size_t{4});
    if (ranges.size() == 4) {
        EXPECT_EQ(ranges[0].offset_, std::uint64_t{50});
        EXPECT_EQ(ranges[1].offset_, std::uint64_t{0});
        EXPECT_EQ(ranges[2].offset_, std::uint64_t{20});
        EXPECT_EQ(ranges[3].offset_, std::uint64_t{90});
    }
    // A coalesced part takes the place of its earliest member, whatever order
    // its members were received in.
    const auto coalesced = resolve_http_byte_range_set("bytes=20-29,0-9,10-19,18-22,90-", 100);
    EXPECT_TRUE(!coalesced.ignored());
    EXPECT_TRUE(!coalesced.unsatisfiable());
    EXPECT_EQ(coalesced.size(), std::size_t{2});
    if (coalesced.size() == 2) {
        EXPECT_EQ(coalesced[0].offset_, std::uint64_t{0});
        EXPECT_EQ(coalesced[0].length_, std::uint64_t{30});
        EXPECT_EQ(coalesced[1].offset_, std::uint64_t{90});
        EXPECT_EQ(coalesced[1].length_, std::uint64_t{10});
    }
    const auto partial = resolve_http_byte_range_set("bytes=0-1,900-", 100);
    EXPECT_EQ(partial.size(), std::size_t{1});
    const auto none = resolve_http_byte_range_set("bytes=100-,200-", 100);
    EXPECT_TRUE(none.unsatisfiable());
}

TEST(Ruvia, byte_range_set_never_repeats_representation_bytes) {
    // Descending open-ended and suffix ranges each overlap every earlier one;
    // as separate parts they would multiply the response size.
    std::string open_ended = "bytes=";
    std::string suffixes = "bytes=";
    for (std::size_t index = 0; index < http_byte_range_set::capacity; ++index) {
        if (index != 0) {
            open_ended.push_back(',');
            suffixes.push_back(',');
        }
        open_ended.append(std::to_string(http_byte_range_set::capacity - 1 - index) + "-");
        suffixes.append("-" + std::to_string(1000 - index));
    }
    for (const auto& field : {open_ended, suffixes}) {
        const auto ranges = resolve_http_byte_range_set(field, 1000);
        EXPECT_EQ(ranges.size(), std::size_t{1});
        if (ranges.size() == 1) {
            EXPECT_EQ(ranges[0].offset_, std::uint64_t{0});
            EXPECT_EQ(ranges[0].length_, std::uint64_t{1000});
        }
    }

    // Ranges that only touch once a later range bridges them become one part.
    const auto bridged = resolve_http_byte_range_set("bytes=30-39,0-9,50-59,5-35", 100);
    EXPECT_EQ(bridged.size(), std::size_t{2});
    if (bridged.size() == 2) {
        EXPECT_EQ(bridged[0].offset_, std::uint64_t{0});
        EXPECT_EQ(bridged[0].length_, std::uint64_t{40});
        EXPECT_EQ(bridged[1].offset_, std::uint64_t{50});
        EXPECT_EQ(bridged[1].length_, std::uint64_t{10});
    }
}

TEST(Ruvia, byte_range_set_tolerates_bounded_empty_members) {
    const auto ranges = resolve_http_byte_range_set("bytes=,0-1,,2-3,", 100);
    EXPECT_EQ(ranges.size(), std::size_t{1});
    if (ranges.size() == 1) {
        EXPECT_EQ(ranges[0].offset_, std::uint64_t{0});
        EXPECT_EQ(ranges[0].length_, std::uint64_t{4});
    }
    EXPECT_TRUE(resolve_http_byte_range_set("bytes=,,", 100).ignored());
    EXPECT_EQ(resolve_http_byte_range_set("bytes=0-1,,3-4", 100).size(), std::size_t{2});
    EXPECT_TRUE(resolve_http_byte_range_set(
        "bytes=0-1,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,", 100)
            .ignored());
}

TEST(Ruvia, byte_range_set_counts_only_received_empty_members) {
    const auto prefix = std::string("bytes=") + std::string(32, ',');
    for (const auto tail : {"100-", "100-199", "-0"}) {
        const auto ranges = resolve_http_byte_range_set(prefix + tail, 100);
        EXPECT_TRUE(!ranges.ignored());
        EXPECT_TRUE(ranges.unsatisfiable());
    }
    const auto satisfiable = resolve_http_byte_range_set(prefix + "0-1", 100);
    EXPECT_EQ(satisfiable.size(), std::size_t{1});
    if (satisfiable.size() == 1) {
        EXPECT_EQ(satisfiable[0].offset_, std::uint64_t{0});
        EXPECT_EQ(satisfiable[0].length_, std::uint64_t{2});
    }
    EXPECT_TRUE(resolve_http_byte_range_set(prefix + ",100-", 100).ignored());
    EXPECT_TRUE(resolve_http_byte_range_set(prefix + ",0-1", 100).ignored());
}

TEST(Ruvia, byte_range_set_ignores_malformed_and_over_limit_sets) {
    std::string ranges = "bytes=";
    for (std::size_t index = 0; index <= http_byte_range_set::capacity; ++index) {
        if (index != 0) {
            ranges.push_back(',');
        }
        ranges.append(std::to_string(index * 2));
        ranges.push_back('-');
        ranges.append(std::to_string(index * 2));
    }
    EXPECT_TRUE(resolve_http_byte_range_set(ranges, 100).ignored());
}

TEST(Ruvia, byte_range_set_empty_representation_uses_ignore_policy) {
    EXPECT_TRUE(is_ignored_range("bytes=0-99", 0));
    EXPECT_TRUE(is_ignored_range("bytes=-1", 0));
    EXPECT_TRUE(is_ignored_range("BYTES=0-", 0));
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}}) {
        EXPECT_TRUE(ruvia::testing::throws_on(
            [&] { (void)resolve_http_byte_range_set("bytes=0-1", invalid); }));
    }
}

TEST(Ruvia, byte_range_set_rejects_out_of_range_index) {
    const auto resolved = resolve_http_byte_range_set("bytes=0-9", 100);
    EXPECT_EQ(resolved.size(), std::size_t{1});
    const auto throws_out_of_range = [](auto&& access) {
        try {
            (void)access();
        } catch (const std::out_of_range&) {
            return true;
        }
        return false;
    };
    EXPECT_TRUE(!throws_out_of_range([&] { return resolved[0]; }));
    EXPECT_TRUE(throws_out_of_range([&] { return resolved[1]; }));
    EXPECT_TRUE(throws_out_of_range([&] { return resolved[http_byte_range_set::capacity]; }));
    const auto ignored = resolve_http_byte_range_set("items=0-9", 100);
    EXPECT_TRUE(throws_out_of_range([&] { return ignored[0]; }));
}
