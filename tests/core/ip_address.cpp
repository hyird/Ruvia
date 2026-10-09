#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/core/IpAddress.h"

#include "test_harness.h"

namespace {

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{};
    std::size_t returns{};
    std::size_t live{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* block = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++allocations;
        ++live;
        return block;
    }

    void do_deallocate(void* block, std::size_t bytes, std::size_t alignment) override {
        ++returns;
        --live;
        std::pmr::new_delete_resource()->deallocate(block, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

class ScopedDefaultResource final {
public:
    explicit ScopedDefaultResource(std::pmr::memory_resource& resource) noexcept
        : previous_(std::pmr::set_default_resource(&resource)) {}

    ~ScopedDefaultResource() {
        std::pmr::set_default_resource(previous_);
    }

    ScopedDefaultResource(const ScopedDefaultResource&) = delete;
    ScopedDefaultResource& operator=(const ScopedDefaultResource&) = delete;

private:
    std::pmr::memory_resource* previous_;
};

}  // namespace

RUVIA_TEST(ip_address_parsing_handles_ipv4_ipv6_and_view_bounds) {
    const auto v4 = ruvia::parseIpAddress("192.0.2.1");
    RUVIA_CHECK((v4.index() == 0) && std::get<0>(v4).is_v4());
    const auto v6 = ruvia::parseIpAddress("2001:0db8:1234:5678:90ab:cdef:1234:5678");
    RUVIA_CHECK((v6.index() == 0) && std::get<0>(v6).is_v6());
    const auto scoped = ruvia::parseIpAddress("fe80::1%12");
    RUVIA_CHECK((scoped.index() == 0) && std::get<0>(scoped).is_v6() && std::get<0>(scoped).to_v6().scope_id() == 12);
    constexpr std::string_view storage = "192.0.2.1suffix";
    RUVIA_CHECK((ruvia::parseIpAddress(storage.substr(0, 9)).index() == 0));
    RUVIA_CHECK((ruvia::parseIpAddress(storage).index() != 0));
}

RUVIA_TEST(ip_address_parsing_rejects_malformed_and_embedded_nul_input) {
    RUVIA_CHECK((ruvia::parseIpAddress(std::string_view{}).index() != 0));
    RUVIA_CHECK((ruvia::parseIpAddress("not an address").index() != 0));
    RUVIA_CHECK((ruvia::parseIpAddress(std::string_view("192.0.2.1\0suffix", 16)).index() != 0));
}

RUVIA_TEST(ip_address_parsing_returns_long_input_pmr_storage) {
    CountingResource resource;
    const auto result = [&] {
        ScopedDefaultResource scope(resource);
        return ruvia::parseIpAddress(std::string(1024, 'x'));
    }();

    RUVIA_CHECK(result.index() != 0);
    RUVIA_CHECK(resource.allocations > 0);
    RUVIA_CHECK_EQ(resource.live, std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocations, resource.returns);
}
