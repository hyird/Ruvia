#include <gtest/gtest.h>

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/socket.h"

#include "memory_resource_fixture.h"

TEST(Ruvia, ip_address_parsing_handles_ipv4_ipv6_and_view_bounds) {
    const auto v4 = ruvia::parse_ip_address("192.0.2.1");
    EXPECT_TRUE(v4.has_value() && v4->is_v4());
    const auto v6 = ruvia::parse_ip_address("2001:0db8:1234:5678:90ab:cdef:1234:5678");
    EXPECT_TRUE(v6.has_value() && v6->is_v6());
    const auto scoped = ruvia::parse_ip_address("fe80::1%12");
    EXPECT_TRUE(scoped.has_value() && scoped->is_v6() && scoped->to_v6().scope_id() == 12);
    constexpr std::string_view storage = "192.0.2.1suffix";
    EXPECT_TRUE(ruvia::parse_ip_address(storage.substr(0, 9)).has_value());
    EXPECT_TRUE(!ruvia::parse_ip_address(storage).has_value());
}

TEST(Ruvia, ip_address_parsing_rejects_malformed_and_embedded_nul_input) {
    EXPECT_TRUE(!ruvia::parse_ip_address(std::string_view{}).has_value());
    EXPECT_TRUE(!ruvia::parse_ip_address("not an address").has_value());
    EXPECT_TRUE(!ruvia::parse_ip_address(std::string_view("192.0.2.1\0suffix", 16)).has_value());
}

TEST(Ruvia, ip_address_parsing_rejects_ports_brackets_and_partial_or_padded_ipv4) {
    for (const std::string_view text : {"192.0.2.1:80", "[::1]:80", "[::1]", "192.0.2", "01.2.3.4", "192.0.2.256",
             "192.0.2.1.", ".192.0.2.1", "192.0.2.1%1", "fe80::1%", "fe80::1%1%2", "::1 ", "1",
             "::ffff:01.2.3.4", "::ffff:192.0.2", "fe80::1%12:80", "fe80::1%12 ", "fe80::1%1]:80"}) {
        EXPECT_TRUE(!ruvia::parse_ip_address(text).has_value());
    }
    EXPECT_TRUE(ruvia::parse_ip_address("0.0.0.0").has_value());
    EXPECT_TRUE(ruvia::parse_ip_address("255.255.255.255").has_value());
    const auto mapped = ruvia::parse_ip_address("::ffff:192.0.2.1");
    EXPECT_TRUE(mapped.has_value() && mapped->is_v6());
}

TEST(Ruvia, ip_address_parsing_returns_long_input_pmr_storage) {
    ruvia::test::counting_memory_resource resource;
    const auto result_value = [&] {
        ruvia::test::default_resource_guard scope(resource);
        // Only a lexically plausible literal reaches the platform parser;
        // temporary storage uses process_resource() rather than the global default.
        return ruvia::parse_ip_address("fe80::1%" + std::pmr::string(1024, 'x', ruvia::process_resource()));
    }();

    // An unknown zone name is a platform decision; the storage must round-trip either way.
    EXPECT_TRUE(!result_value.has_value() || result_value->is_v6());
    EXPECT_EQ(resource.allocation_count(), std::size_t{0});
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
    EXPECT_EQ(resource.allocation_count(), resource.deallocation_count());
}

TEST(Ruvia, ip_address_parsing_rejects_invalid_ipv6_zone_identifiers) {
    for (const std::string_view text : {
             "fe80::1%0",
             "fe80::1%01",
             "fe80::1%-1",
             "fe80::1%+1",
             "fe80::1%12abc",
             "fe80::1%4294967296",
             "fe80::1%99999999999999999999",
             "fe80::1%nonexistent_ruvia_zone_id",
         }) {
        EXPECT_TRUE(!ruvia::parse_ip_address(text).has_value());
    }
}
