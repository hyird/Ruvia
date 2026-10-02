#include <array>
#include <memory_resource>
#include <string>

#include "ruvia/http/HttpConnectionAdvertisement.h"

#include "test_harness.h"

namespace {
struct AdvertisementResource final : std::pmr::memory_resource {
    std::size_t live{0};
    bool fail{false};
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        // Fail frame storage, not noexcept debug iterator bookkeeping.
        if (fail && bytes >= 32) {
            throw std::bad_alloc();
        }
        auto* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        live += bytes;
        return result;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        live -= bytes;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace
RUVIA_TEST(http_connection_advertisement_origin_frames_preserve_results_and_release_operation_storage) {
    AdvertisementResource resource;
    {
        const std::array<std::string_view, 2> origins{"https://example.test", "https://other.test"};
        const auto retained = ruvia::encodeHttp2OriginFrame(origins, 16384, &resource);
        RUVIA_CHECK(retained.has_value());
        auto parsed = ruvia::decodeHttpOriginAdvertisement(std::span(*retained).subspan(9), &resource);
        RUVIA_CHECK(parsed && parsed->origins.size() == 2);
        const auto baseline = resource.live;
        for (std::size_t i = 0; i < 32; ++i) {
            {
                const auto frame = ruvia::encodeHttp3OriginFrame(origins, 65536, &resource);
                RUVIA_CHECK(frame.has_value());
                const auto result = ruvia::decodeHttpOriginAdvertisement(std::span(*frame).subspan(2), &resource);
                RUVIA_CHECK(result && result->origins.front() == origins.front());
            }
            RUVIA_CHECK_EQ(resource.live, baseline);
            RUVIA_CHECK(parsed->origins.back() == origins.back());
        }
        resource.fail = true;
        bool threw = false;
        try {
            (void)ruvia::encodeHttp3OriginFrame(origins, 65536, &resource);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        resource.fail = false;
        RUVIA_CHECK_EQ(resource.live, baseline);
    }
    RUVIA_CHECK_EQ(resource.live, 0u);
}
RUVIA_TEST(http_connection_advertisement_invalid_entries_truncation_and_alternative_service_association) {
    const std::array<char, 5> invalid{0, 3, 'b', 'a', 'd'};
    const auto ignored = ruvia::decodeHttpOriginAdvertisement(invalid);
    RUVIA_CHECK(ignored && ignored->origins.empty());
    RUVIA_CHECK(!ruvia::decodeHttpOriginAdvertisement(std::span(invalid).first(4)));
    RUVIA_CHECK(!ruvia::encodeHttp2OriginFrame(std::array<std::string_view, 1>{"https://example.test/path"}));
    const auto encoded = ruvia::encodeHttp2AlternativeServiceFrame(0, "https://example.test", "h3=\":443\"; ma=60");
    RUVIA_CHECK(encoded.has_value());
    const auto parsed = ruvia::decodeHttp2AlternativeService(0, std::span(*encoded).subspan(9));
    RUVIA_CHECK(parsed && parsed->origin == "https://example.test" && parsed->fieldValue == "h3=\":443\"; ma=60");
    RUVIA_CHECK(!ruvia::encodeHttp2AlternativeServiceFrame(1, "https://example.test", "clear"));
    RUVIA_CHECK(!ruvia::decodeHttp2AlternativeService(1, std::span(*encoded).subspan(9)));
    RUVIA_CHECK(!ruvia::encodeHttp2AlternativeServiceFrame(0, "", "clear"));
    RUVIA_CHECK(!ruvia::encodeHttp2AlternativeServiceFrame(1, "", "clear\r\n"));
}
