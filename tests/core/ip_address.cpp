#include "ruvia/core/ip_address.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <variant>

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
    RUVIA_CHECK((v4.index() == 0) && std::get<0>(v4).is_v4());
    const auto v6 = ruvia::parse_ip_address("2001:0db8:1234:5678:90ab:cdef:1234:5678");
    RUVIA_CHECK((v6.index() == 0) && std::get<0>(v6).is_v6());
    const auto scoped = ruvia::parse_ip_address("fe80::1%12");
    RUVIA_CHECK((scoped.index() == 0) && std::get<0>(scoped).is_v6() && std::get<0>(scoped).to_v6().scope_id() == 12);
    constexpr std::string_view storage = "192.0.2.1suffix";
    RUVIA_CHECK((ruvia::parse_ip_address(storage.substr(0, 9)).index() == 0));
    RUVIA_CHECK((ruvia::parse_ip_address(storage).index() != 0));
}

RUVIA_TEST(ip_address_parsing_rejects_malformed_and_embedded_nul_input) {
    RUVIA_CHECK((ruvia::parse_ip_address(std::string_view{}).index() != 0));
    RUVIA_CHECK((ruvia::parse_ip_address("not an address").index() != 0));
    RUVIA_CHECK((ruvia::parse_ip_address(std::string_view("192.0.2.1\0suffix", 16)).index() != 0));
}

RUVIA_TEST(ip_address_parsing_returns_long_input_pmr_storage) {
    counting_resource resource;
    const auto result_value = [&] {
        scoped_default_resource scope(resource);
        return ruvia::parse_ip_address(std::string(1024, 'x'));
    }();

    RUVIA_CHECK(result_value.index() != 0);
    RUVIA_CHECK(resource.allocations_ > 0);
    RUVIA_CHECK_EQ(resource.live_, std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocations_, resource.returns_);
}
