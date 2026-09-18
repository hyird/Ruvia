#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>

#include "ruvia/core/detail/io/IpAddress.h"

#include "test_harness.h"

RUVIA_TEST(ip_address_parsing_preserves_family_scope_and_view_bounds) {
    const auto v4 = ruvia::detail::parseIpAddress("192.0.2.1");
    RUVIA_CHECK(v4 && v4->is_v4());
    const auto v6 = ruvia::detail::parseIpAddress("2001:0db8:1234:5678:90ab:cdef:1234:5678");
    RUVIA_CHECK(v6 && v6->is_v6());
    const auto scoped = ruvia::detail::parseIpAddress("fe80::1%12");
    RUVIA_CHECK(scoped && scoped->is_v6() && scoped->to_v6().scope_id() == 12);
    constexpr std::string_view storage = "192.0.2.1suffix";
    RUVIA_CHECK(ruvia::detail::parseIpAddress(storage.substr(0, 9)).has_value());
    RUVIA_CHECK(!ruvia::detail::parseIpAddress(storage));
    RUVIA_CHECK(!ruvia::detail::parseIpAddress(std::string_view{}));
    RUVIA_CHECK(!ruvia::detail::parseIpAddress(std::string_view("192.0.2.1\0suffix", 16)));
    for (const std::size_t size : {63u, 64u, 65u, 1024u}) {
        RUVIA_CHECK(!ruvia::detail::parseIpAddress(std::string(size, 'x')));
    }
}
