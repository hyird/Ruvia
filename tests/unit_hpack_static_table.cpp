#include "test_harness.h"

#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/http2/Http2HpackStaticTable.h"

namespace {

using ruvia::detail::hpackFindStaticHeaderMatch;
using ruvia::detail::hpackStaticHeaderAt;
using ruvia::detail::kHpackStaticTableSize;

}  // namespace

RUVIA_TEST(hpack_static_table_known_indices) {
    RUVIA_CHECK_EQ(kHpackStaticTableSize, std::size_t{61});
    // Spot-check normative entries from RFC 7541 Appendix A (1-indexed).
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(1).name, std::string_view(":authority"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(1).value, std::string_view(""));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(2).name, std::string_view(":method"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(2).value, std::string_view("GET"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(3).value, std::string_view("POST"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(8).name, std::string_view(":status"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(8).value, std::string_view("200"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(16).name, std::string_view("accept-encoding"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(16).value, std::string_view("gzip, deflate"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(32).name, std::string_view("cookie"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(61).name, std::string_view("www-authenticate"));
}

RUVIA_TEST(hpack_find_exact_match) {
    auto method = hpackFindStaticHeaderMatch(":method", "GET");
    RUVIA_CHECK_EQ(method.exactIndex, std::uint32_t{2});
    RUVIA_CHECK_EQ(method.nameIndex, std::uint32_t{2});

    // A later value for the same name resolves to its own exact index, but the
    // name index still points at the first occurrence of that name.
    auto post = hpackFindStaticHeaderMatch(":method", "POST");
    RUVIA_CHECK_EQ(post.exactIndex, std::uint32_t{3});
    RUVIA_CHECK_EQ(post.nameIndex, std::uint32_t{2});

    auto status404 = hpackFindStaticHeaderMatch(":status", "404");
    RUVIA_CHECK_EQ(status404.exactIndex, std::uint32_t{13});
    RUVIA_CHECK_EQ(status404.nameIndex, std::uint32_t{8});

    auto acceptEncoding = hpackFindStaticHeaderMatch("accept-encoding", "gzip, deflate");
    RUVIA_CHECK_EQ(acceptEncoding.exactIndex, std::uint32_t{16});
    RUVIA_CHECK_EQ(acceptEncoding.nameIndex, std::uint32_t{16});

    // An empty-value entry.
    auto cookie = hpackFindStaticHeaderMatch("cookie", "");
    RUVIA_CHECK_EQ(cookie.exactIndex, std::uint32_t{32});
}

RUVIA_TEST(hpack_find_name_only_match) {
    // Name present, value not in the table -> name index only, no exact index.
    auto method = hpackFindStaticHeaderMatch(":method", "PUT");
    RUVIA_CHECK_EQ(method.exactIndex, std::uint32_t{0});
    RUVIA_CHECK_EQ(method.nameIndex, std::uint32_t{2});

    auto status = hpackFindStaticHeaderMatch(":status", "201");
    RUVIA_CHECK_EQ(status.exactIndex, std::uint32_t{0});
    RUVIA_CHECK_EQ(status.nameIndex, std::uint32_t{8});
}

RUVIA_TEST(hpack_find_no_match) {
    auto custom = hpackFindStaticHeaderMatch("x-custom-header", "value");
    RUVIA_CHECK_EQ(custom.exactIndex, std::uint32_t{0});
    RUVIA_CHECK_EQ(custom.nameIndex, std::uint32_t{0});
}
