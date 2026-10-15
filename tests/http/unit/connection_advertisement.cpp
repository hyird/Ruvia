#include <array>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string>
#include <variant>

#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http_connection_advertisement.h"

#include "test_harness.h"

namespace {
struct advertisement_resource final : std::pmr::memory_resource {
    std::size_t live_{0};
    bool fail_{false};
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        // Fail frame storage, not noexcept debug iterator bookkeeping.
        if (fail_ && bytes_value >= 32) {
            throw std::bad_alloc();
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
RUVIA_TEST(http_connection_advertisement_origin_frames_preserve_results_and_release_operation_storage) {
    advertisement_resource resource;
    {
        const std::array<std::string_view, 2> origins{"https://example.test", "https://other.test"};
        const auto retained = ruvia::encode_http2_origin_frame(origins, 16384, &resource);
        RUVIA_CHECK((retained.index() == 0));
        auto parsed_value = ruvia::decode_http_origin_advertisement(std::span(std::get<0>(retained)).subspan(9), &resource);
        RUVIA_CHECK((parsed_value.index() == 0) && std::get<0>(parsed_value).origins_.size() == 2);
        const auto baseline = resource.live_;
        for (std::size_t i = 0; i < 32; ++i) {
            {
                const auto frame = ruvia::encode_http3_origin_frame(origins, 65536, &resource);
                RUVIA_CHECK((frame.index() == 0));
                const auto result_value = ruvia::decode_http_origin_advertisement(std::span(std::get<0>(frame)).subspan(2), &resource);
                RUVIA_CHECK((result_value.index() == 0) && std::get<0>(result_value).origins_.front() == origins.front());
            }
            RUVIA_CHECK_EQ(resource.live_, baseline);
            RUVIA_CHECK(std::get<0>(parsed_value).origins_.back() == origins.back());
        }
        resource.fail_ = true;
        bool threw = false;
        try {
            (void)ruvia::encode_http3_origin_frame(origins, 65536, &resource);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        resource.fail_ = false;
        RUVIA_CHECK_EQ(resource.live_, baseline);
    }
    RUVIA_CHECK_EQ(resource.live_, 0u);
}
RUVIA_TEST(http_connection_advertisement_alternative_service_respects_protocol_payload_limit) {
    // The length field is always 24 bits, even when the caller supplies a larger
    // limit. An impossible frame must report limit before allocating its output.
    const std::string field_value(ruvia::http2_max_frame_size - 1, 'a');
    advertisement_resource resource;
    resource.fail_ = true;
    for (const auto maximum : {ruvia::http2_max_frame_size, (std::numeric_limits<std::uint32_t>::max)()}) {
        bool threw = false;
        try {
            const auto result_value = ruvia::encode_http2_alternative_service_frame(1, "", field_value, maximum, &resource);
            RUVIA_CHECK((result_value.index() != 0));
            if ((result_value.index() != 0)) {
                RUVIA_CHECK(std::get<1>(result_value) == ruvia::http_connection_advertisement_error::limit);
            }
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(!threw);
        RUVIA_CHECK_EQ(resource.live_, std::size_t{0});
    }
}

RUVIA_TEST(http_connection_advertisement_encoders_honor_exact_payload_budgets) {
    const std::array<std::string_view, 2> origins{"https://one.test", "https://two.test"};
    const auto payload_bytes = 4 + origins[0].size() + origins[1].size();
    const auto http2_frame = ruvia::encode_http2_origin_frame(origins, static_cast<std::uint32_t>(payload_bytes));
    RUVIA_CHECK((http2_frame.index() == 0));
    if ((http2_frame.index() == 0)) {
        const auto header_value = ruvia::parse_http2_frame_header(std::get<0>(http2_frame));
        RUVIA_CHECK(header_value.has_value());
        if (header_value) {
            RUVIA_CHECK_EQ(header_value->length_, payload_bytes);
            RUVIA_CHECK_EQ(std::get<0>(http2_frame).size(), payload_bytes + ruvia::http2_frame_header_bytes);
        }
    }
    const auto http3_frame = ruvia::encode_http3_origin_frame(origins, payload_bytes);
    RUVIA_CHECK((http3_frame.index() == 0));
    if ((http3_frame.index() == 0)) {
        const auto header_value = ruvia::decode_http3_frame_header(std::get<0>(http3_frame));
        RUVIA_CHECK((header_value.index() == 0));
        if ((header_value.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(header_value).length_, payload_bytes);
            RUVIA_CHECK_EQ(std::get<0>(http3_frame).size(), payload_bytes + std::get<0>(header_value).encoded_bytes_);
        }
    }
    const std::array<std::string_view, 0> no_origins{};
    const auto empty_http2 = ruvia::encode_http2_origin_frame(no_origins, 0, nullptr);
    const auto empty_http3 = ruvia::encode_http3_origin_frame(no_origins, 0, nullptr);
    RUVIA_CHECK((empty_http2.index() == 0));
    RUVIA_CHECK((empty_http3.index() == 0));
    if ((empty_http2.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(empty_http2).size(), ruvia::http2_frame_header_bytes);
    }
    if ((empty_http3.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(empty_http3).size(), std::size_t{2});
    }
    for (const auto stream_id : {0u, 1u}) {
        const std::string_view origin = stream_id == 0 ? origins[0] : "";
        const std::string_view value = "clear";
        const auto size = static_cast<std::uint32_t>(2 + origin.size() + value.size());
        const auto result_value = ruvia::encode_http2_alternative_service_frame(stream_id, origin, value, size);
        RUVIA_CHECK((result_value.index() == 0));
        if ((result_value.index() == 0)) {
            const auto decoded = ruvia::decode_http2_alternative_service(stream_id,
                std::span(std::get<0>(result_value)).subspan(ruvia::http2_frame_header_bytes));
            RUVIA_CHECK((decoded.index() == 0));
            if ((decoded.index() == 0)) {
                RUVIA_CHECK(std::get<0>(decoded).origin_ == origin);
                RUVIA_CHECK(std::get<0>(decoded).field_value_ == value);
            }
        }
        const auto limited = ruvia::encode_http2_alternative_service_frame(stream_id, origin, value, size - 1);
        RUVIA_CHECK((limited.index() != 0));
        if ((limited.index() != 0)) {
            RUVIA_CHECK(std::get<1>(limited) == ruvia::http_connection_advertisement_error::limit);
        }
    }
    advertisement_resource resource;
    resource.fail_ = true;
    const auto http2_limited = ruvia::encode_http2_origin_frame(origins,
        static_cast<std::uint32_t>(payload_bytes - 1), &resource);
    const auto http3_limited = ruvia::encode_http3_origin_frame(origins, payload_bytes - 1, &resource);
    for (const auto* result_value : {&http2_limited, &http3_limited}) {
        RUVIA_CHECK((*result_value).index() != 0);
        if (result_value->index() != 0) {
            RUVIA_CHECK(std::get<1>(*result_value) == ruvia::http_connection_advertisement_error::limit);
        }
    }
    RUVIA_CHECK_EQ(resource.live_, std::size_t{0});
}

RUVIA_TEST(http_connection_advertisement_valid_frame_allocation_failure_releases_storage_and_allows_retry) {
    const std::array<std::string_view, 2> origins{"https://example.test", "https://other.test"};
    advertisement_resource resource;
    const auto check = [&](auto make_frame) {
        resource.fail_ = true;
        bool threw = false;
        try {
            static_cast<void>(make_frame());
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK_EQ(resource.live_, std::size_t{0});
        resource.fail_ = false;
        {
            const auto frame = make_frame();
            RUVIA_CHECK(frame.index() == 0);
        }
        RUVIA_CHECK_EQ(resource.live_, std::size_t{0});
    };
    check([&] { return ruvia::encode_http2_origin_frame(origins, 16384, &resource); });
    check([&] { return ruvia::encode_http3_origin_frame(origins, 65536, &resource); });
    check([&] { return ruvia::encode_http2_alternative_service_frame(0, origins[0], "h3=\":443\"; ma=60", 16384, &resource); });
}

RUVIA_TEST(http_connection_advertisement_invalid_entries_truncation_and_alternative_service_association) {
    const std::array<char, 5> invalid{0, 3, 'b', 'a', 'd'};
    const auto ignored = ruvia::decode_http_origin_advertisement(invalid);
    RUVIA_CHECK((ignored.index() == 0) && std::get<0>(ignored).origins_.empty());
    RUVIA_CHECK((ruvia::decode_http_origin_advertisement(std::span(invalid).first(4)).index() != 0));
    RUVIA_CHECK((ruvia::encode_http2_origin_frame(std::array<std::string_view, 1>{"https://example.test/path"}).index() != 0));
    const auto encoded = ruvia::encode_http2_alternative_service_frame(0, "https://example.test", "h3=\":443\"; ma=60");
    RUVIA_CHECK((encoded.index() == 0));
    const auto parsed_value = ruvia::decode_http2_alternative_service(0, std::span(std::get<0>(encoded)).subspan(9));
    RUVIA_CHECK((parsed_value.index() == 0) && std::get<0>(parsed_value).origin_ == "https://example.test" && std::get<0>(parsed_value).field_value_ == "h3=\":443\"; ma=60");
    RUVIA_CHECK((ruvia::encode_http2_alternative_service_frame(1, "https://example.test", "clear").index() != 0));
    RUVIA_CHECK((ruvia::decode_http2_alternative_service(1, std::span(std::get<0>(encoded)).subspan(9)).index() != 0));
    RUVIA_CHECK((ruvia::encode_http2_alternative_service_frame(0, "", "clear").index() != 0));
    RUVIA_CHECK((ruvia::encode_http2_alternative_service_frame(1, "", "clear\r\n").index() != 0));
}
