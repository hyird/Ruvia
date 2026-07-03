#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "net/http2/Http2HeaderList.h"

namespace {

using ruvia::detail::Http2HeaderList;
using ruvia::detail::RequestHeaderKind;

std::pmr::memory_resource* resource() noexcept {
    return std::pmr::new_delete_resource();
}

}  // namespace

RUVIA_TEST(header_list_append_and_read_round_trip) {
    Http2HeaderList list(resource());
    RUVIA_CHECK_EQ(list.size(), std::size_t{0});

    RUVIA_CHECK(list.append("content-type", "text/plain", RequestHeaderKind::kOther));
    RUVIA_CHECK(list.append("accept", "application/json", RequestHeaderKind::kAccept));
    RUVIA_CHECK_EQ(list.size(), std::size_t{2});

    RUVIA_CHECK_EQ(list.at(0).name, std::string_view("content-type"));
    RUVIA_CHECK_EQ(list.at(0).value, std::string_view("text/plain"));
    RUVIA_CHECK(list.at(0).kind == RequestHeaderKind::kOther);
    RUVIA_CHECK_EQ(list.at(1).name, std::string_view("accept"));
    RUVIA_CHECK_EQ(list.at(1).value, std::string_view("application/json"));
    RUVIA_CHECK(list.at(1).kind == RequestHeaderKind::kAccept);
}

RUVIA_TEST(header_list_empty_value_round_trips) {
    Http2HeaderList list(resource());
    RUVIA_CHECK(list.append("x-flag", "", RequestHeaderKind::kOther));
    RUVIA_CHECK_EQ(list.at(0).name, std::string_view("x-flag"));
    RUVIA_CHECK(list.at(0).value.empty());
}

RUVIA_TEST(header_list_spills_both_fields_and_bytes_preserving_prior_views) {
    // 30 headers, each ~35 bytes, cross both the 16-field inline boundary and
    // the 512-byte inline-storage boundary. Every header (including those stored
    // before the byte-storage spill copied the inline blob to overflow) must
    // still read back correctly.
    Http2HeaderList list(resource());
    std::vector<std::string> names;
    std::vector<std::string> values;
    constexpr int kCount = 30;
    for (int i = 0; i < kCount; ++i) {
        names.push_back("header-" + std::to_string(i));
        values.push_back(std::to_string(i) + std::string(30, static_cast<char>('a' + (i % 26))));
    }
    for (int i = 0; i < kCount; ++i) {
        RUVIA_CHECK(list.append(names[static_cast<std::size_t>(i)],
                                values[static_cast<std::size_t>(i)],
                                RequestHeaderKind::kOther));
    }
    RUVIA_CHECK_EQ(list.size(), std::size_t{kCount});
    for (int i = 0; i < kCount; ++i) {
        const auto view = list.at(static_cast<std::size_t>(i));
        RUVIA_CHECK_EQ(view.name, std::string_view(names[static_cast<std::size_t>(i)]));
        RUVIA_CHECK_EQ(view.value, std::string_view(values[static_cast<std::size_t>(i)]));
    }
}

RUVIA_TEST(header_list_full_rejects_further_append) {
    Http2HeaderList list(resource());
    // kMaxRequestHeaders is 64.
    for (int i = 0; i < 64; ++i) {
        RUVIA_CHECK(list.append("k", "v", RequestHeaderKind::kOther));
    }
    RUVIA_CHECK(list.full());
    RUVIA_CHECK_EQ(list.size(), std::size_t{64});
    // A full list rejects the next append but keeps its contents intact.
    RUVIA_CHECK(!list.append("overflow", "x", RequestHeaderKind::kOther));
    RUVIA_CHECK_EQ(list.size(), std::size_t{64});
    RUVIA_CHECK_EQ(list.at(0).name, std::string_view("k"));
    RUVIA_CHECK_EQ(list.at(63).value, std::string_view("v"));
}
