#include <array>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string>
#include <variant>

#include "ruvia/http/Http2Framing.h"
#include "ruvia/http/Http3Frames.h"
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
        RUVIA_CHECK((retained.index() == 0));
        auto parsed = ruvia::decodeHttpOriginAdvertisement(std::span(std::get<0>(retained)).subspan(9), &resource);
        RUVIA_CHECK((parsed.index() == 0) && std::get<0>(parsed).origins.size() == 2);
        const auto baseline = resource.live;
        for (std::size_t i = 0; i < 32; ++i) {
            {
                const auto frame = ruvia::encodeHttp3OriginFrame(origins, 65536, &resource);
                RUVIA_CHECK((frame.index() == 0));
                const auto result = ruvia::decodeHttpOriginAdvertisement(std::span(std::get<0>(frame)).subspan(2), &resource);
                RUVIA_CHECK((result.index() == 0) && std::get<0>(result).origins.front() == origins.front());
            }
            RUVIA_CHECK_EQ(resource.live, baseline);
            RUVIA_CHECK(std::get<0>(parsed).origins.back() == origins.back());
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
RUVIA_TEST(http_connection_advertisement_alternative_service_respects_protocol_payload_limit) {
    // The length field is always 24 bits, even when the caller supplies a larger
    // limit. An impossible frame must report limit before allocating its output.
    const std::string field_value(ruvia::kHttp2MaxFrameSize - 1, 'a');
    AdvertisementResource resource;
    resource.fail = true;
    for (const auto maximum : {ruvia::kHttp2MaxFrameSize, (std::numeric_limits<std::uint32_t>::max)()}) {
        bool threw = false;
        try {
            const auto result = ruvia::encodeHttp2AlternativeServiceFrame(1, "", field_value, maximum, &resource);
            RUVIA_CHECK((result.index() != 0));
            if ((result.index() != 0)) {
                RUVIA_CHECK(std::get<1>(result) == ruvia::HttpConnectionAdvertisementError::kLimit);
            }
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(!threw);
        RUVIA_CHECK_EQ(resource.live, std::size_t{0});
    }
}

RUVIA_TEST(http_connection_advertisement_encoders_honor_exact_payload_budgets) {
    const std::array<std::string_view, 2> origins{"https://one.test", "https://two.test"};
    const auto payload_bytes = 4 + origins[0].size() + origins[1].size();
    const auto http2_frame = ruvia::encodeHttp2OriginFrame(origins, static_cast<std::uint32_t>(payload_bytes));
    RUVIA_CHECK((http2_frame.index() == 0));
    if ((http2_frame.index() == 0)) {
        const auto header = ruvia::parseHttp2FrameHeader(std::get<0>(http2_frame));
        RUVIA_CHECK(header.has_value());
        if (header) {
            RUVIA_CHECK_EQ(header->length, payload_bytes);
            RUVIA_CHECK_EQ(std::get<0>(http2_frame).size(), payload_bytes + ruvia::kHttp2FrameHeaderBytes);
        }
    }
    const auto http3_frame = ruvia::encodeHttp3OriginFrame(origins, payload_bytes);
    RUVIA_CHECK((http3_frame.index() == 0));
    if ((http3_frame.index() == 0)) {
        const auto header = ruvia::decodeHttp3FrameHeader(std::get<0>(http3_frame));
        RUVIA_CHECK((header.index() == 0));
        if ((header.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(header).length, payload_bytes);
            RUVIA_CHECK_EQ(std::get<0>(http3_frame).size(), payload_bytes + std::get<0>(header).encodedBytes);
        }
    }
    const std::array<std::string_view, 0> no_origins{};
    const auto empty_http2 = ruvia::encodeHttp2OriginFrame(no_origins, 0, nullptr);
    const auto empty_http3 = ruvia::encodeHttp3OriginFrame(no_origins, 0, nullptr);
    RUVIA_CHECK((empty_http2.index() == 0));
    RUVIA_CHECK((empty_http3.index() == 0));
    if ((empty_http2.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(empty_http2).size(), ruvia::kHttp2FrameHeaderBytes);
    }
    if ((empty_http3.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(empty_http3).size(), std::size_t{2});
    }
    for (const auto stream_id : {0u, 1u}) {
        const std::string_view origin = stream_id == 0 ? origins[0] : "";
        const std::string_view value = "clear";
        const auto size = static_cast<std::uint32_t>(2 + origin.size() + value.size());
        const auto result = ruvia::encodeHttp2AlternativeServiceFrame(stream_id, origin, value, size);
        RUVIA_CHECK((result.index() == 0));
        if ((result.index() == 0)) {
            const auto decoded = ruvia::decodeHttp2AlternativeService(stream_id,
                std::span(std::get<0>(result)).subspan(ruvia::kHttp2FrameHeaderBytes));
            RUVIA_CHECK((decoded.index() == 0));
            if ((decoded.index() == 0)) {
                RUVIA_CHECK(std::get<0>(decoded).origin == origin);
                RUVIA_CHECK(std::get<0>(decoded).fieldValue == value);
            }
        }
        const auto limited = ruvia::encodeHttp2AlternativeServiceFrame(stream_id, origin, value, size - 1);
        RUVIA_CHECK((limited.index() != 0));
        if ((limited.index() != 0)) {
            RUVIA_CHECK(std::get<1>(limited) == ruvia::HttpConnectionAdvertisementError::kLimit);
        }
    }
    AdvertisementResource resource;
    resource.fail = true;
    const auto http2_limited = ruvia::encodeHttp2OriginFrame(origins,
        static_cast<std::uint32_t>(payload_bytes - 1), &resource);
    const auto http3_limited = ruvia::encodeHttp3OriginFrame(origins, payload_bytes - 1, &resource);
    for (const auto* result : {&http2_limited, &http3_limited}) {
        RUVIA_CHECK((*result).index() != 0);
        if (result->index() != 0) {
            RUVIA_CHECK(std::get<1>(*result) == ruvia::HttpConnectionAdvertisementError::kLimit);
        }
    }
    RUVIA_CHECK_EQ(resource.live, std::size_t{0});
}

RUVIA_TEST(http_connection_advertisement_valid_frame_allocation_failure_releases_storage_and_allows_retry) {
    const std::array<std::string_view, 2> origins{"https://example.test", "https://other.test"};
    AdvertisementResource resource;
    const auto check = [&](auto make_frame) {
        resource.fail = true;
        bool threw = false;
        try {
            static_cast<void>(make_frame());
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK_EQ(resource.live, std::size_t{0});
        resource.fail = false;
        {
            const auto frame = make_frame();
            RUVIA_CHECK(frame.index() == 0);
        }
        RUVIA_CHECK_EQ(resource.live, std::size_t{0});
    };
    check([&] { return ruvia::encodeHttp2OriginFrame(origins, 16384, &resource); });
    check([&] { return ruvia::encodeHttp3OriginFrame(origins, 65536, &resource); });
    check([&] { return ruvia::encodeHttp2AlternativeServiceFrame(0, origins[0], "h3=\":443\"; ma=60", 16384, &resource); });
}

RUVIA_TEST(http_connection_advertisement_invalid_entries_truncation_and_alternative_service_association) {
    const std::array<char, 5> invalid{0, 3, 'b', 'a', 'd'};
    const auto ignored = ruvia::decodeHttpOriginAdvertisement(invalid);
    RUVIA_CHECK((ignored.index() == 0) && std::get<0>(ignored).origins.empty());
    RUVIA_CHECK((ruvia::decodeHttpOriginAdvertisement(std::span(invalid).first(4)).index() != 0));
    RUVIA_CHECK((ruvia::encodeHttp2OriginFrame(std::array<std::string_view, 1>{"https://example.test/path"}).index() != 0));
    const auto encoded = ruvia::encodeHttp2AlternativeServiceFrame(0, "https://example.test", "h3=\":443\"; ma=60");
    RUVIA_CHECK((encoded.index() == 0));
    const auto parsed = ruvia::decodeHttp2AlternativeService(0, std::span(std::get<0>(encoded)).subspan(9));
    RUVIA_CHECK((parsed.index() == 0) && std::get<0>(parsed).origin == "https://example.test" && std::get<0>(parsed).fieldValue == "h3=\":443\"; ma=60");
    RUVIA_CHECK((ruvia::encodeHttp2AlternativeServiceFrame(1, "https://example.test", "clear").index() != 0));
    RUVIA_CHECK((ruvia::decodeHttp2AlternativeService(1, std::span(std::get<0>(encoded)).subspan(9)).index() != 0));
    RUVIA_CHECK((ruvia::encodeHttp2AlternativeServiceFrame(0, "", "clear").index() != 0));
    RUVIA_CHECK((ruvia::encodeHttp2AlternativeServiceFrame(1, "", "clear\r\n").index() != 0));
}
