#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "http2/http2_header_list.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http2_header_list;
using ruvia::detail::request_header_kind;

std::pmr::memory_resource* resource() noexcept {
    return std::pmr::new_delete_resource();
}

}  // namespace

RUVIA_TEST(header_list_append_and_read_round_trip) {
    http2_header_list list(resource());
    RUVIA_CHECK_EQ(list.size(), std::size_t{0});

    RUVIA_CHECK(list.append("content-type", "text/plain", request_header_kind::other));
    RUVIA_CHECK(list.append("accept", "application/json", request_header_kind::accept));
    RUVIA_CHECK_EQ(list.size(), std::size_t{2});

    RUVIA_CHECK_EQ(list.at(0).name_, std::string_view("content-type"));
    RUVIA_CHECK_EQ(list.at(0).value_, std::string_view("text/plain"));
    RUVIA_CHECK(list.at(0).kind_ == request_header_kind::other);
    RUVIA_CHECK_EQ(list.at(1).name_, std::string_view("accept"));
    RUVIA_CHECK_EQ(list.at(1).value_, std::string_view("application/json"));
    RUVIA_CHECK(list.at(1).kind_ == request_header_kind::accept);
}

RUVIA_TEST(header_list_empty_value_round_trips) {
    http2_header_list list(resource());
    RUVIA_CHECK(list.append("x-flag", "", request_header_kind::other));
    RUVIA_CHECK_EQ(list.at(0).name_, std::string_view("x-flag"));
    RUVIA_CHECK(list.at(0).value_.empty());
}

RUVIA_TEST(header_list_spills_both_fields_and_bytes_preserving_prior_views) {
    // 30 headers, each ~35 bytes, cross both the 16-field inline boundary and
    // the 512-byte inline-storage boundary. Every header (including those stored
    // before the byte-storage spill copied the inline blob to overflow) must
    // still read back correctly.
    http2_header_list list(resource());
    std::vector<std::string> names;
    std::vector<std::string> values;
    constexpr int count = 30;
    for (int i = 0; i < count; ++i) {
        names.push_back("header-" + std::to_string(i));
        values.push_back(std::to_string(i) + std::string(30, static_cast<char>('a' + (i % 26))));
    }
    for (int i = 0; i < count; ++i) {
        RUVIA_CHECK(list.append(names[static_cast<std::size_t>(i)],
            values[static_cast<std::size_t>(i)], request_header_kind::other));
    }
    RUVIA_CHECK_EQ(list.size(), std::size_t{count});
    for (int i = 0; i < count; ++i) {
        const auto view = list.at(static_cast<std::size_t>(i));
        RUVIA_CHECK_EQ(view.name_, std::string_view(names[static_cast<std::size_t>(i)]));
        RUVIA_CHECK_EQ(view.value_, std::string_view(values[static_cast<std::size_t>(i)]));
    }
}

RUVIA_TEST(header_list_full_rejects_further_append) {
    http2_header_list list(resource());
    // max_http_header_fields is 64.
    for (int i = 0; i < 64; ++i) {
        RUVIA_CHECK(list.append("k", "v", request_header_kind::other));
    }
    RUVIA_CHECK(list.full());
    RUVIA_CHECK_EQ(list.size(), std::size_t{64});
    // A full list rejects the next append but keeps its contents intact.
    RUVIA_CHECK(!list.append("overflow", "x", request_header_kind::other));
    RUVIA_CHECK_EQ(list.size(), std::size_t{64});
    RUVIA_CHECK_EQ(list.at(0).name_, std::string_view("k"));
    RUVIA_CHECK_EQ(list.at(63).value_, std::string_view("v"));
}
