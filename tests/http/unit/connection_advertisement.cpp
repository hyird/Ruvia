#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_connection_advertisement.h"

#include "stl_allocation_fixture.h"

namespace {
struct advertisement_resource final : std::pmr::memory_resource {
    std::size_t live_{0};
    bool fail_{false};
    std::size_t allocations_before_failure_{(std::numeric_limits<std::size_t>::max)()};
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (fail_ || allocations_before_failure_ == 0) {
            throw std::bad_alloc();
        }
        if (allocations_before_failure_ != (std::numeric_limits<std::size_t>::max)()) {
            --allocations_before_failure_;
        }
        auto* result_value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        live_ += bytes_value;
        return result_value;
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        live_ -= bytes_value;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace
TEST(Ruvia, http_connection_advertisement_origin_frames_preserve_results_and_release_operation_storage) {
    advertisement_resource resource;
    {
        const std::array<std::string_view, 2> origins{"https://example.test", "https://other.test"};
        const auto retained = ruvia::encode_http2_origin_frame(origins, 16384, &resource);
        EXPECT_TRUE((retained.index() == 0));
        auto parsed_value = ruvia::decode_http_origin_advertisement(std::span(std::get<0>(retained)).subspan(9), &resource);
        EXPECT_TRUE((parsed_value.index() == 0) && std::get<0>(parsed_value).origins_.size() == 2);
        const auto baseline = resource.live_;
        for (std::size_t i = 0; i < 32; ++i) {
            {
                const auto frame = ruvia::encode_http2_origin_frame(origins, 16384, &resource);
                EXPECT_TRUE((frame.index() == 0));
                const auto result_value = ruvia::decode_http_origin_advertisement(std::span(std::get<0>(frame)).subspan(ruvia::http2_frame_header_bytes), &resource);
                EXPECT_TRUE((result_value.index() == 0) && std::get<0>(result_value).origins_.front() == origins.front());
            }
            EXPECT_EQ(resource.live_, baseline);
            EXPECT_TRUE(std::get<0>(parsed_value).origins_.back() == origins.back());
        }
        if (ruvia::test::stl_allocation_failure_injection_enabled()) {
            resource.fail_ = true;
            bool threw = false;
            try {
                (void)ruvia::encode_http2_origin_frame(origins, 16384, &resource);
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            EXPECT_TRUE(threw);
            resource.fail_ = false;
        }
        EXPECT_EQ(resource.live_, baseline);
    }
    EXPECT_EQ(resource.live_, 0u);
}
TEST(Ruvia, http_connection_advertisement_alternative_service_respects_protocol_payload_limit) {
    // The length field is always 24 bits, even when the caller supplies a larger
    // limit. An impossible frame must report limit before allocating its output.
    const std::string field_value(ruvia::http2_max_frame_size - 1, 'a');
    advertisement_resource resource;
    resource.fail_ = true;
    for (const auto maximum : {ruvia::http2_max_frame_size, (std::numeric_limits<std::uint32_t>::max)()}) {
        bool threw = false;
        try {
            const auto result_value = ruvia::encode_http2_alternative_service_frame(1, "", field_value, maximum, &resource);
            EXPECT_TRUE((result_value.index() != 0));
            if ((result_value.index() != 0)) {
                EXPECT_TRUE(std::get<1>(result_value) == ruvia::http_connection_advertisement_error::limit);
            }
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        EXPECT_TRUE(!threw);
        EXPECT_EQ(resource.live_, std::size_t{0});
    }
}

TEST(Ruvia, http_connection_advertisement_encoders_honor_exact_payload_budgets) {
    const std::array<std::string_view, 2> origins{"https://one.test", "https://two.test"};
    const auto payload_bytes = 4 + origins[0].size() + origins[1].size();
    const auto http2_frame = ruvia::encode_http2_origin_frame(origins, static_cast<std::uint32_t>(payload_bytes));
    EXPECT_TRUE((http2_frame.index() == 0));
    if ((http2_frame.index() == 0)) {
        const auto header_value = ruvia::parse_http2_frame_header(std::get<0>(http2_frame));
        EXPECT_TRUE(header_value.has_value());
        if (header_value) {
            EXPECT_EQ(header_value->length_, payload_bytes);
            EXPECT_EQ(std::get<0>(http2_frame).size(), payload_bytes + ruvia::http2_frame_header_bytes);
        }
    }
    const std::array<std::string_view, 0> no_origins{};
    const auto empty_http2 = ruvia::encode_http2_origin_frame(no_origins, 0, nullptr);
    EXPECT_TRUE((empty_http2.index() == 0));
    if ((empty_http2.index() == 0)) {
        EXPECT_EQ(std::get<0>(empty_http2).size(), ruvia::http2_frame_header_bytes);
    }
    for (const auto stream_id : {0u, 1u}) {
        const std::string_view origin = stream_id == 0 ? origins[0] : "";
        const std::string_view value = "clear";
        const auto size = static_cast<std::uint32_t>(2 + origin.size() + value.size());
        const auto result_value = ruvia::encode_http2_alternative_service_frame(stream_id, origin, value, size);
        EXPECT_TRUE((result_value.index() == 0));
        if ((result_value.index() == 0)) {
            const auto decoded = ruvia::decode_http2_alternative_service(stream_id,
                std::span(std::get<0>(result_value)).subspan(ruvia::http2_frame_header_bytes));
            EXPECT_TRUE((decoded.index() == 0));
            if ((decoded.index() == 0)) {
                EXPECT_TRUE(std::get<0>(decoded).origin_ == origin);
                EXPECT_TRUE(std::get<0>(decoded).field_value_ == value);
            }
        }
        const auto limited = ruvia::encode_http2_alternative_service_frame(stream_id, origin, value, size - 1);
        EXPECT_TRUE((limited.index() != 0));
        if ((limited.index() != 0)) {
            EXPECT_TRUE(std::get<1>(limited) == ruvia::http_connection_advertisement_error::limit);
        }
    }
    advertisement_resource resource;
    resource.fail_ = true;
    const auto http2_limited = ruvia::encode_http2_origin_frame(origins,
        static_cast<std::uint32_t>(payload_bytes - 1), &resource);
    EXPECT_TRUE(http2_limited.index() != 0);
    if (http2_limited.index() != 0) {
        EXPECT_TRUE(std::get<1>(http2_limited) == ruvia::http_connection_advertisement_error::limit);
    }
    EXPECT_EQ(resource.live_, std::size_t{0});
}

TEST(Ruvia, http_connection_advertisement_valid_frame_allocation_failure_releases_storage_and_allows_retry) {
    const std::array<std::string_view, 2> origins{"https://example.test", "https://other.test"};
    advertisement_resource resource;
    const bool inject_failures = ruvia::test::stl_allocation_failure_injection_enabled();
    const auto check = [&](auto make_frame) {
        if (inject_failures) {
            resource.fail_ = true;
            bool threw = false;
            try {
                static_cast<void>(make_frame());
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            EXPECT_TRUE(threw);
            EXPECT_EQ(resource.live_, std::size_t{0});
            resource.fail_ = false;
        }
        {
            const auto frame = make_frame();
            EXPECT_TRUE(frame.index() == 0);
        }
        EXPECT_EQ(resource.live_, std::size_t{0});
    };
    check([&] { return ruvia::encode_http2_origin_frame(origins, 16384, &resource); });
    check([&] { return ruvia::encode_http2_alternative_service_frame(0, origins[0], "h3=\":443\"; ma=60", 16384, &resource); });
}

TEST(Ruvia, http_connection_advertisement_invalid_entries_truncation_and_alternative_service_association) {
    const std::array<char, 5> invalid{0, 3, 'b', 'a', 'd'};
    const auto ignored = ruvia::decode_http_origin_advertisement(invalid);
    EXPECT_TRUE((ignored.index() == 0) && std::get<0>(ignored).origins_.empty());
    EXPECT_TRUE((ruvia::decode_http_origin_advertisement(std::span(invalid).first(4)).index() != 0));
    EXPECT_TRUE((ruvia::encode_http2_origin_frame(std::array<std::string_view, 1>{"https://example.test/path"}).index() != 0));
    const auto encoded = ruvia::encode_http2_alternative_service_frame(0, "https://example.test", "h3=\":443\"; ma=60");
    EXPECT_TRUE((encoded.index() == 0));
    const auto parsed_value = ruvia::decode_http2_alternative_service(0, std::span(std::get<0>(encoded)).subspan(9));
    EXPECT_TRUE((parsed_value.index() == 0) && std::get<0>(parsed_value).origin_ == "https://example.test" && std::get<0>(parsed_value).field_value_ == "h3=\":443\"; ma=60");
    EXPECT_TRUE((ruvia::encode_http2_alternative_service_frame(1, "https://example.test", "clear").index() != 0));
    EXPECT_TRUE((ruvia::decode_http2_alternative_service(1, std::span(std::get<0>(encoded)).subspan(9)).index() != 0));
    EXPECT_TRUE((ruvia::encode_http2_alternative_service_frame(0, "", "clear").index() != 0));
    EXPECT_TRUE((ruvia::encode_http2_alternative_service_frame(1, "", "clear\r\n").index() != 0));
}

TEST(Ruvia, http_connection_advertisement_copies_inherit_source_resources) {
    static_assert(std::is_nothrow_move_constructible_v<ruvia::http_origin_advertisement>);
    static_assert(std::is_nothrow_move_constructible_v<ruvia::http_alternative_service_advertisement>);
    advertisement_resource resource;
    {
        const std::array<std::string_view, 2> origins{"https://one.example.test", "https://two.example.test"};
        const auto frame = ruvia::encode_http2_origin_frame(origins, 16384, &resource);
        ASSERT_EQ(frame.index(), 0u);
        const auto decoded = ruvia::decode_http_origin_advertisement(
            std::span(std::get<0>(frame)).subspan(ruvia::http2_frame_header_bytes), &resource);
        ASSERT_EQ(decoded.index(), 0u);
        const auto copy = std::get<0>(decoded);
        EXPECT_EQ(copy.origins_, std::get<0>(decoded).origins_);
        EXPECT_EQ(copy.origins_.get_allocator().resource(), &resource);
        for (const auto& origin : copy.origins_) {
            EXPECT_EQ(origin.get_allocator().resource(), &resource);
        }

        const std::string_view field = "h3=\":443\"; ma=86400; persist=1";
        const auto alternative_frame = ruvia::encode_http2_alternative_service_frame(0, origins[0], field, 16384, &resource);
        ASSERT_EQ(alternative_frame.index(), 0u);
        const auto alternative_decoded = ruvia::decode_http2_alternative_service(0,
            std::span(std::get<0>(alternative_frame)).subspan(ruvia::http2_frame_header_bytes), &resource);
        ASSERT_EQ(alternative_decoded.index(), 0u);
        const auto alternative_copy = std::get<0>(alternative_decoded);
        EXPECT_EQ(alternative_copy.stream_id_, 0u);
        EXPECT_EQ(alternative_copy.origin_, origins[0]);
        EXPECT_EQ(alternative_copy.field_value_, field);
        EXPECT_EQ(alternative_copy.origin_.get_allocator().resource(), &resource);
        EXPECT_EQ(alternative_copy.field_value_.get_allocator().resource(), &resource);
    }
    EXPECT_EQ(resource.live_, 0u);
}

TEST(Ruvia, http_connection_advertisement_origin_assignment_preserves_resource_and_rolls_back) {
    const std::array<std::string_view, 2> origins{
        "https://new-first.test", "https://abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz.example.test"};
    const bool inject_failures = ruvia::test::stl_allocation_failure_injection_enabled();
    for (const bool move : {false, true}) {
        advertisement_resource source_resource;
        advertisement_resource destination_resource;
        {
            ruvia::http_origin_advertisement source(&source_resource);
            for (const auto origin : origins) {
                source.origins_.emplace_back(origin);
            }
            ruvia::http_origin_advertisement destination(&destination_resource);
            destination.origins_.emplace_back("https://old-first.test");
            destination.origins_.emplace_back("https://old-second.test");
            const auto before = destination_resource.live_;
            if (inject_failures) {
                destination_resource.fail_ = true;
                if (move) {
                    EXPECT_THROW(destination = std::move(source), std::bad_alloc);
                } else {
                    EXPECT_THROW(destination = source, std::bad_alloc);
                }
                destination_resource.fail_ = false;
                ASSERT_EQ(destination.origins_.size(), 2u);
                EXPECT_EQ(destination.origins_[0], "https://old-first.test");
                EXPECT_EQ(destination.origins_[1], "https://old-second.test");
                EXPECT_EQ(destination_resource.live_, before);
                ASSERT_EQ(source.origins_.size(), origins.size());
                EXPECT_EQ(source.origins_[0], origins[0]);
                EXPECT_EQ(source.origins_[1], origins[1]);
            }
            if (move) {
                destination = std::move(source);
            } else {
                destination = source;
            }
            ASSERT_EQ(destination.origins_.size(), origins.size());
            EXPECT_EQ(destination.origins_[0], origins[0]);
            EXPECT_EQ(destination.origins_[1], origins[1]);
            EXPECT_EQ(destination.origins_.get_allocator().resource(), &destination_resource);
            for (const auto& origin : destination.origins_) {
                EXPECT_EQ(origin.get_allocator().resource(), &destination_resource);
            }
        }
        EXPECT_EQ(source_resource.live_, 0u);
        EXPECT_EQ(destination_resource.live_, 0u);
    }
}

TEST(Ruvia, http_connection_advertisement_alternative_service_assignment_is_atomic) {
    const std::string_view origin = "https://abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz.example.test";
    const std::string_view field = "h3=\":443\"; ma=86400; persist=1; x=\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\"";
    const bool inject_failures = ruvia::test::stl_allocation_failure_injection_enabled();
    for (const bool move : {false, true}) {
        for (std::size_t allowed = 0; allowed != (inject_failures ? 2u : 1u); ++allowed) {
            advertisement_resource source_resource;
            advertisement_resource destination_resource;
            {
                ruvia::http_alternative_service_advertisement source(&source_resource);
                source.origin_ = origin;
                source.field_value_ = field;
                ruvia::http_alternative_service_advertisement destination(&destination_resource);
                destination.stream_id_ = 1;
                destination.field_value_ = "clear";
                const auto before = destination_resource.live_;
                if (inject_failures) {
                    destination_resource.allocations_before_failure_ = allowed;
                    if (move) {
                        EXPECT_THROW(destination = std::move(source), std::bad_alloc);
                    } else {
                        EXPECT_THROW(destination = source, std::bad_alloc);
                    }
                    destination_resource.allocations_before_failure_ = (std::numeric_limits<std::size_t>::max)();
                    EXPECT_EQ(destination.stream_id_, 1u);
                    EXPECT_TRUE(destination.origin_.empty());
                    EXPECT_EQ(destination.field_value_, "clear");
                    EXPECT_EQ(destination_resource.live_, before);
                    EXPECT_EQ(source.stream_id_, 0u);
                    EXPECT_EQ(source.origin_, origin);
                    EXPECT_EQ(source.field_value_, field);
                }
                if (move) {
                    destination = std::move(source);
                } else {
                    destination = source;
                }
                EXPECT_EQ(destination.stream_id_, 0u);
                EXPECT_EQ(destination.origin_, origin);
                EXPECT_EQ(destination.field_value_, field);
                EXPECT_EQ(destination.origin_.get_allocator().resource(), &destination_resource);
                EXPECT_EQ(destination.field_value_.get_allocator().resource(), &destination_resource);
            }
            EXPECT_EQ(source_resource.live_, 0u);
            EXPECT_EQ(destination_resource.live_, 0u);
        }
    }
}

TEST(Ruvia, http_connection_advertisement_same_resource_moves_transfer_storage) {
    advertisement_resource resource;
    const bool inject_failures = ruvia::test::stl_allocation_failure_injection_enabled();
    {
        ruvia::http_origin_advertisement source(&resource);
        source.origins_.emplace_back("https://source.example.test");
        ruvia::http_alternative_service_advertisement service(&resource);
        service.origin_ = "https://source.example.test";
        service.field_value_ = "h3=\":443\"; ma=86400; persist=1";
        const auto retained_bytes = resource.live_;
        const auto* origins_storage = source.origins_.data();
        const auto* origin_storage = service.origin_.data();
        const auto* field_storage = service.field_value_.data();
        ruvia::http_origin_advertisement destination(&resource);
        destination.origins_.emplace_back("https://retired.example.test");
        ruvia::http_alternative_service_advertisement destination_service(&resource);
        destination_service.stream_id_ = 1;
        destination_service.field_value_ = "h3=\":8443\"; ma=3600; persist=1";

        resource.fail_ = inject_failures;
        auto moved = std::move(source);
        auto moved_service = std::move(service);
        destination = std::move(moved);
        destination_service = std::move(moved_service);
        auto* self = &destination;
        destination = *self;
        destination = std::move(*self);
        auto* service_self = &destination_service;
        destination_service = *service_self;
        destination_service = std::move(*service_self);
        resource.fail_ = false;

        EXPECT_EQ(destination.origins_.data(), origins_storage);
        ASSERT_EQ(destination.origins_.size(), 1u);
        EXPECT_EQ(destination.origins_[0], "https://source.example.test");
        EXPECT_EQ(destination_service.origin_.data(), origin_storage);
        EXPECT_EQ(destination_service.field_value_.data(), field_storage);
        EXPECT_EQ(destination_service.stream_id_, 0u);
        EXPECT_EQ(destination_service.origin_, "https://source.example.test");
        EXPECT_EQ(destination_service.field_value_, "h3=\":443\"; ma=86400; persist=1");
        if (inject_failures) {
            EXPECT_EQ(resource.live_, retained_bytes);
        }
    }
    EXPECT_EQ(resource.live_, 0u);
}
