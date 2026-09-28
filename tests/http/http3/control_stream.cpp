#include <array>
#include <memory_resource>

#include "ruvia/http/Http3ControlStream.h"

#include "test_harness.h"

namespace {
using namespace ruvia;

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t outstanding{0};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto* value = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++outstanding;
        return value;
    }
    void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
        --outstanding;
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

RUVIA_TEST(http3_control_stream_releases_settings_storage_while_connection_remains_active) {
    CountingResource resource;
    Http3ControlStream stream(Http3ControlRole::kServer, &resource);
    constexpr std::array<char, 4> settings{0x4, 0x2, 0x1, 0x0};
    RUVIA_CHECK(stream.feed(settings, false) == Http3ControlStreamStatus::kNeedMoreData);
    RUVIA_CHECK(stream.peerSettings().has_value());
    RUVIA_CHECK_EQ(resource.outstanding, std::size_t{0});
}

RUVIA_TEST(http3_control_stream_consumes_fragmented_settings_and_monotone_goaway) {
    std::pmr::monotonic_buffer_resource resource;
    Http3ControlStream stream(Http3ControlRole::kClient, &resource);
    constexpr std::array<char, 8> wire{0x4, 0x0, 0x7, 0x1, 0x8, 0x7, 0x1, 0x4};
    for (char byte : wire) {
        RUVIA_CHECK(stream.feed(std::span<const char>(&byte, 1), false) == Http3ControlStreamStatus::kNeedMoreData);
    }
    RUVIA_CHECK(stream.peerSettings().has_value());
    RUVIA_CHECK(stream.goawayId() == 4);
}

RUVIA_TEST(http3_control_stream_rejects_invalid_goaway_settings_and_max_push_id_role) {
    std::pmr::monotonic_buffer_resource resource;
    Http3ControlStream badGoaway(Http3ControlRole::kClient, &resource);
    constexpr std::array<char, 8> increasing{0x4, 0x0, 0x7, 0x1, 0x4, 0x7, 0x1, 0x8};
    (void)badGoaway.feed(std::span<const char>(increasing).first(5), false);
    RUVIA_CHECK(badGoaway.feed(std::span<const char>(increasing).subspan(5), false) == Http3ControlStreamStatus::kIdError);

    Http3ControlStream maxPush(Http3ControlRole::kClient, &resource);
    constexpr std::array<char, 5> maxPushWire{0x4, 0x0, 0xd, 0x1, 0x0};
    RUVIA_CHECK(maxPush.feed(maxPushWire, false) == Http3ControlStreamStatus::kFrameUnexpected);

    Http3ControlStream duplicate(Http3ControlRole::kServer, &resource);
    constexpr std::array<char, 6> duplicateSettings{0x4, 0x4, 0x1, 0x0, 0x1, 0x0};
    RUVIA_CHECK(duplicate.feed(duplicateSettings, false) == Http3ControlStreamStatus::kSettingsError);
}

RUVIA_TEST(http3_control_stream_requires_settings_and_reports_critical_fin) {
    std::pmr::monotonic_buffer_resource resource;
    Http3ControlStream missing(Http3ControlRole::kServer, &resource);
    constexpr std::array<char, 2> goawayFirst{0x7, 0x0};
    RUVIA_CHECK(missing.feed(goawayFirst, false) == Http3ControlStreamStatus::kMissingSettings);

    Http3ControlStream fin(Http3ControlRole::kServer, &resource);
    constexpr std::array<char, 2> settings{0x4, 0x0};
    RUVIA_CHECK(fin.feed(settings, true) == Http3ControlStreamStatus::kClosedCriticalStream);
    RUVIA_CHECK(fin.peerSettings().has_value());
}
}  // namespace
