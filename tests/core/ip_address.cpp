#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/core/socket.h"

#include "test_harness.h"

namespace {

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{};
    std::size_t returns_{};
    std::size_t live_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        void* block = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++allocations_;
        ++live_;
        return block;
    }

    void do_deallocate(void* block, std::size_t bytes_value, std::size_t alignment) override {
        ++returns_;
        --live_;
        std::pmr::new_delete_resource()->deallocate(block, bytes_value, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

class scoped_default_resource final {
public:
    explicit scoped_default_resource(std::pmr::memory_resource& resource) noexcept
        : previous_(std::pmr::set_default_resource(&resource)) {}

    ~scoped_default_resource() {
        std::pmr::set_default_resource(previous_);
    }

    scoped_default_resource(const scoped_default_resource&) = delete;
    scoped_default_resource& operator=(const scoped_default_resource&) = delete;

private:
    std::pmr::memory_resource* previous_;
};

}  // namespace

RUVIA_TEST(ip_address_parsing_handles_ipv4_ipv6_and_view_bounds) {
    const auto v4 = ruvia::parse_ip_address("192.0.2.1");
    RUVIA_CHECK(v4.has_value() && v4->is_v4());
    const auto v6 = ruvia::parse_ip_address("2001:0db8:1234:5678:90ab:cdef:1234:5678");
    RUVIA_CHECK(v6.has_value() && v6->is_v6());
    const auto scoped = ruvia::parse_ip_address("fe80::1%12");
    RUVIA_CHECK(scoped.has_value() && scoped->is_v6() && scoped->to_v6().scope_id() == 12);
    constexpr std::string_view storage = "192.0.2.1suffix";
    RUVIA_CHECK(ruvia::parse_ip_address(storage.substr(0, 9)).has_value());
    RUVIA_CHECK(!ruvia::parse_ip_address(storage).has_value());
}

RUVIA_TEST(ip_address_parsing_rejects_malformed_and_embedded_nul_input) {
    RUVIA_CHECK(!ruvia::parse_ip_address(std::string_view{}).has_value());
    RUVIA_CHECK(!ruvia::parse_ip_address("not an address").has_value());
    RUVIA_CHECK(!ruvia::parse_ip_address(std::string_view("192.0.2.1\0suffix", 16)).has_value());
}

RUVIA_TEST(ip_address_parsing_rejects_ports_brackets_and_partial_or_padded_ipv4) {
    for (const std::string_view text : {"192.0.2.1:80", "[::1]:80", "[::1]", "192.0.2", "01.2.3.4", "192.0.2.256",
             "192.0.2.1.", ".192.0.2.1", "192.0.2.1%1", "fe80::1%", "fe80::1%1%2", "::1 ", "1",
             "::ffff:01.2.3.4", "::ffff:192.0.2", "fe80::1%12:80", "fe80::1%12 ", "fe80::1%1]:80"}) {
        RUVIA_CHECK(!ruvia::parse_ip_address(text).has_value());
    }
    RUVIA_CHECK(ruvia::parse_ip_address("0.0.0.0").has_value());
    RUVIA_CHECK(ruvia::parse_ip_address("255.255.255.255").has_value());
    const auto mapped = ruvia::parse_ip_address("::ffff:192.0.2.1");
    RUVIA_CHECK(mapped.has_value() && mapped->is_v6());
}

RUVIA_TEST(ip_address_parsing_returns_long_input_pmr_storage) {
    counting_resource resource;
    const auto result_value = [&] {
        scoped_default_resource scope(resource);
        // Only a lexically plausible literal reaches the platform parser;
        // temporary storage uses process_resource() rather than the global default.
        return ruvia::parse_ip_address("fe80::1%" + std::string(1024, 'x'));
    }();

    // An unknown zone name is a platform decision; the storage must round-trip either way.
    RUVIA_CHECK(!result_value.has_value() || result_value->is_v6());
    RUVIA_CHECK_EQ(resource.allocations_, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_, std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocations_, resource.returns_);
}

RUVIA_TEST(ip_address_parsing_rejects_invalid_ipv6_zone_identifiers) {
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
        RUVIA_CHECK(!ruvia::parse_ip_address(text).has_value());
    }
}
