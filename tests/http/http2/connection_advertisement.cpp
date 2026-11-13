#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_priority.h"
TEST(Ruvia, http_connection_advertisement_origin_frames_preserve_results) {
    const std::array<std::string_view, 2> origins{"https://example.test", "https://other.test"};
    const auto retained = ruvia::encode_http2_origin_frame(origins, 16384);
    ASSERT_EQ(retained.index(), 0u);
    auto parsed_value = ruvia::decode_http_origin_advertisement(std::span(std::get<0>(retained)).subspan(9));
    ASSERT_EQ(parsed_value.index(), 0u);
    ASSERT_EQ(std::get<0>(parsed_value).origins_.size(), 2u);
    for (std::size_t i = 0; i < 32; ++i) {
        const auto frame = ruvia::encode_http2_origin_frame(origins, 16384);
        ASSERT_EQ(frame.index(), 0u);
        const auto result_value = ruvia::decode_http_origin_advertisement(
            std::span(std::get<0>(frame)).subspan(ruvia::http2_frame_header_bytes));
        ASSERT_EQ(result_value.index(), 0u);
        EXPECT_EQ(std::get<0>(result_value).origins_.front(), origins.front());
        EXPECT_EQ(std::get<0>(parsed_value).origins_.back(), origins.back());
    }
}

TEST(Ruvia, http_connection_advertisement_alternative_service_respects_protocol_payload_limit) {
    const std::string field_value(ruvia::http2_max_frame_size - 1, 'a');
    for (const auto maximum : {ruvia::http2_max_frame_size, (std::numeric_limits<std::uint32_t>::max)()}) {
        const auto result_value = ruvia::encode_http2_alternative_service_frame(1, "", field_value, maximum);
        ASSERT_NE(result_value.index(), 0u);
        EXPECT_EQ(std::get<1>(result_value), ruvia::http_connection_advertisement_error::limit);
    }
}

TEST(Ruvia, http_connection_advertisement_encoders_honor_exact_payload_budgets) {
    const std::array<std::string_view, 2> origins{"https://one.test", "https://two.test"};
    const auto payload_bytes = 4 + origins[0].size() + origins[1].size();
    const auto http2_frame = ruvia::encode_http2_origin_frame(origins, static_cast<std::uint32_t>(payload_bytes));
    ASSERT_EQ(http2_frame.index(), 0u);
    const auto header_value = ruvia::parse_http2_frame_header(std::get<0>(http2_frame));
    ASSERT_TRUE(header_value.has_value());
    EXPECT_EQ(header_value->length_, payload_bytes);
    EXPECT_EQ(std::get<0>(http2_frame).size(), payload_bytes + ruvia::http2_frame_header_bytes);

    const std::array<std::string_view, 0> no_origins{};
    const auto empty_http2 = ruvia::encode_http2_origin_frame(no_origins, 0);
    ASSERT_EQ(empty_http2.index(), 0u);
    EXPECT_EQ(std::get<0>(empty_http2).size(), ruvia::http2_frame_header_bytes);

    for (const auto stream_id : {0u, 1u}) {
        const std::string_view origin = stream_id == 0 ? origins[0] : "";
        const std::string_view value = "clear";
        const auto size = static_cast<std::uint32_t>(2 + origin.size() + value.size());
        const auto result_value = ruvia::encode_http2_alternative_service_frame(stream_id, origin, value, size);
        ASSERT_EQ(result_value.index(), 0u);
        const auto decoded = ruvia::decode_http2_alternative_service(stream_id,
            std::span(std::get<0>(result_value)).subspan(ruvia::http2_frame_header_bytes));
        ASSERT_EQ(decoded.index(), 0u);
        EXPECT_EQ(std::get<0>(decoded).origin_, origin);
        EXPECT_EQ(std::get<0>(decoded).field_value_, value);

        const auto limited = ruvia::encode_http2_alternative_service_frame(stream_id, origin, value, size - 1);
        ASSERT_NE(limited.index(), 0u);
        EXPECT_EQ(std::get<1>(limited), ruvia::http_connection_advertisement_error::limit);
    }
    const auto http2_limited = ruvia::encode_http2_origin_frame(origins,
        static_cast<std::uint32_t>(payload_bytes - 1));
    ASSERT_NE(http2_limited.index(), 0u);
    EXPECT_EQ(std::get<1>(http2_limited), ruvia::http_connection_advertisement_error::limit);
}

TEST(Ruvia, http_connection_advertisement_invalid_entries_truncation_and_alternative_service_association) {
    const std::array<char, 5> invalid{0, 3, 'b', 'a', 'd'};
    const auto ignored = ruvia::decode_http_origin_advertisement(invalid);
    EXPECT_TRUE((ignored.index() == 0) && std::get<0>(ignored).origins_.empty());
    EXPECT_TRUE((ruvia::decode_http_origin_advertisement(std::span(invalid).first(4)).index() != 0));
    EXPECT_TRUE((ruvia::encode_http2_origin_frame(std::array<std::string_view, 1>{"https://example.test/path"}).index() != 0));
    const auto encoded = ruvia::encode_http2_alternative_service_frame(0, "https://example.test", "h3=\":443\"; ma=60");
    ASSERT_EQ(encoded.index(), 0u);
    const auto parsed_value = ruvia::decode_http2_alternative_service(0, std::span(std::get<0>(encoded)).subspan(9));
    EXPECT_TRUE((parsed_value.index() == 0) && std::get<0>(parsed_value).origin_ == "https://example.test" && std::get<0>(parsed_value).field_value_ == "h3=\":443\"; ma=60");
    EXPECT_TRUE((ruvia::encode_http2_alternative_service_frame(1, "https://example.test", "clear").index() != 0));
    EXPECT_TRUE((ruvia::decode_http2_alternative_service(1, std::span(std::get<0>(encoded)).subspan(9)).index() != 0));
    EXPECT_TRUE((ruvia::encode_http2_alternative_service_frame(0, "", "clear").index() != 0));
    EXPECT_TRUE((ruvia::encode_http2_alternative_service_frame(1, "", "clear\r\n").index() != 0));
}

TEST(Ruvia, http_connection_advertisement_copy_and_move_semantics) {
    static_assert(std::is_nothrow_move_constructible_v<ruvia::http_origin_advertisement>);
    static_assert(std::is_nothrow_move_constructible_v<ruvia::http_alternative_service_advertisement>);

    ruvia::http_origin_advertisement source;
    source.origins_.emplace_back("https://source.example.test");
    ruvia::http_alternative_service_advertisement service;
    service.origin_ = "https://source.example.test";
    service.field_value_ = "h3=\":443\"; ma=86400; persist=1";

    const auto* origins_storage = source.origins_.data();
    const auto copy = source;
    EXPECT_EQ(copy.origins_, source.origins_);
    const auto service_copy = service;
    EXPECT_EQ(service_copy.origin_, service.origin_);
    EXPECT_EQ(service_copy.field_value_, service.field_value_);

    auto moved = std::move(source);
    EXPECT_EQ(moved.origins_.data(), origins_storage);
    ASSERT_EQ(moved.origins_.size(), 1u);
    EXPECT_EQ(moved.origins_[0], "https://source.example.test");
}
