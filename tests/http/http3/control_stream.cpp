#include <array>
#include <memory_resource>

#include "ruvia/http/http3_control_stream.h"

#include "test_harness.h"

namespace {
using namespace ruvia;

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t outstanding_{0};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        auto* value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++outstanding_;
        return value;
    }
    void do_deallocate(void* value, std::size_t bytes_value, std::size_t alignment) override {
        --outstanding_;
        std::pmr::new_delete_resource()->deallocate(value, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

RUVIA_TEST(http3_control_stream_releases_settings_storage_while_connection_remains_active) {
    counting_resource resource;
    {
        http3_control_stream stream(http3_control_role::server, &resource);
        const auto baseline = resource.outstanding_;
        constexpr std::array<char, 4> settings{0x4, 0x2, 0x1, 0x0};
        RUVIA_CHECK(stream.feed(settings, false) == http3_control_stream_status::need_more_data);
        RUVIA_CHECK(stream.peer_settings().has_value());
        RUVIA_CHECK_EQ(resource.outstanding_, baseline);
    }
    RUVIA_CHECK_EQ(resource.outstanding_, std::size_t{0});
}

RUVIA_TEST(http3_control_stream_consumes_fragmented_settings_and_monotone_goaway) {
    std::pmr::monotonic_buffer_resource resource;
    http3_control_stream stream(http3_control_role::client, &resource);
    constexpr std::array<char, 8> wire{0x4, 0x0, 0x7, 0x1, 0x8, 0x7, 0x1, 0x4};
    for (char byte : wire) {
        RUVIA_CHECK(stream.feed(std::span<const char>(&byte, 1), false) == http3_control_stream_status::need_more_data);
    }
    RUVIA_CHECK(stream.peer_settings().has_value());
    RUVIA_CHECK(stream.goaway_id() == 4);
}

RUVIA_TEST(http3_control_stream_rejects_invalid_goaway_settings_and_max_push_id_role) {
    std::pmr::monotonic_buffer_resource resource;
    http3_control_stream bad_goaway(http3_control_role::client, &resource);
    constexpr std::array<char, 8> increasing{0x4, 0x0, 0x7, 0x1, 0x4, 0x7, 0x1, 0x8};
    (void)bad_goaway.feed(std::span<const char>(increasing).first(5), false);
    RUVIA_CHECK(bad_goaway.feed(std::span<const char>(increasing).subspan(5), false) == http3_control_stream_status::id_error);

    http3_control_stream max_push(http3_control_role::client, &resource);
    constexpr std::array<char, 5> max_push_wire{0x4, 0x0, 0xd, 0x1, 0x0};
    RUVIA_CHECK(max_push.feed(max_push_wire, false) == http3_control_stream_status::frame_unexpected);

    http3_control_stream duplicate(http3_control_role::server, &resource);
    constexpr std::array<char, 6> duplicate_settings{0x4, 0x4, 0x1, 0x0, 0x1, 0x0};
    RUVIA_CHECK(duplicate.feed(duplicate_settings, false) == http3_control_stream_status::settings_error);
}

RUVIA_TEST(http3_control_stream_requires_settings_and_reports_critical_fin) {
    std::pmr::monotonic_buffer_resource resource;
    http3_control_stream missing(http3_control_role::server, &resource);
    constexpr std::array<char, 2> goaway_first{0x7, 0x0};
    RUVIA_CHECK(missing.feed(goaway_first, false) == http3_control_stream_status::missing_settings);

    http3_control_stream fin(http3_control_role::server, &resource);
    constexpr std::array<char, 2> settings{0x4, 0x0};
    RUVIA_CHECK(fin.feed(settings, true) == http3_control_stream_status::closed_critical_stream);
    RUVIA_CHECK(fin.peer_settings().has_value());
}

RUVIA_TEST(http3_control_stream_server_goaway_push_id_rules) {
    std::pmr::monotonic_buffer_resource resource;
    constexpr std::array<char, 2> settings{0x4, 0x0};
    constexpr std::array<char, 3> goaway0{0x7, 0x1, 0x0};
    constexpr std::array<char, 3> max_push1{0xd, 0x1, 0x1};
    constexpr std::array<char, 3> goaway1{0x7, 0x1, 0x1};
    constexpr std::array<char, 3> goaway2{0x7, 0x1, 0x2};

    // GOAWAY may reject all pushes even when push has not been enabled.
    {
        http3_control_stream server(http3_control_role::server, &resource);
        RUVIA_CHECK(server.feed(settings, false) == http3_control_stream_status::need_more_data);
        RUVIA_CHECK(server.feed(goaway0, false) == http3_control_stream_status::need_more_data);
    }

    // A shutdown boundary may exceed MAX_PUSH_ID (RFC 9114 section 5.2).
    {
        http3_control_stream server(http3_control_role::server, &resource);
        RUVIA_CHECK(server.feed(settings, false) == http3_control_stream_status::need_more_data);
        RUVIA_CHECK(server.feed(max_push1, false) == http3_control_stream_status::need_more_data);
        RUVIA_CHECK(server.feed(goaway2, false) == http3_control_stream_status::need_more_data);
    }

    // Server receiving GOAWAY with Push ID <= MAX_PUSH_ID succeeds
    {
        http3_control_stream server(http3_control_role::server, &resource);
        RUVIA_CHECK(server.feed(settings, false) == http3_control_stream_status::need_more_data);
        RUVIA_CHECK(server.feed(max_push1, false) == http3_control_stream_status::need_more_data);
        RUVIA_CHECK(server.feed(goaway1, false) == http3_control_stream_status::need_more_data);
        RUVIA_CHECK(server.goaway_id() == 1);
        // Subsequent GOAWAY with greater ID must fail with id_error
        RUVIA_CHECK(server.feed(goaway2, false) == http3_control_stream_status::id_error);
    }
}
}  // namespace
