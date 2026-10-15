#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include "ruvia/http/http3_data_write_plan.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_var_int.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_status.h"

#include "test_harness.h"

namespace {

using ruvia::decode_http3_frame_header;
using ruvia::http3_client_request_body_plan;
using ruvia::http3_data_write_error;
using ruvia::http3_data_write_plan;
using ruvia::http3_var_int_max;
using ruvia::http_known_method;
using ruvia::http_status_code;
using ruvia::plan_http_response_body;

}  // namespace

RUVIA_TEST(http3_data_write_plan_borrows_repeated_chunks_and_commits_only_after_write) {
    http3_data_write_plan plan(plan_http_response_body(http_known_method::get, http_status_code::from_value(200)), 5);
    std::array<char, 2> first{'a', 'b'};
    RUVIA_CHECK(!plan.fin_allowed());
    const auto first_chunk = plan.plan_chunk(first, false);
    RUVIA_CHECK((first_chunk.index() == 0));
    if ((first_chunk.index() != 0)) {
        return;
    }
    RUVIA_CHECK(std::get<0>(first_chunk).emits_data_);
    RUVIA_CHECK_EQ(std::get<0>(first_chunk).payload_.data(), first.data());
    const auto header_value = decode_http3_frame_header(
        std::span<const char>(std::get<0>(first_chunk).frame_header_).first(std::get<0>(first_chunk).frame_header_size_));
    RUVIA_CHECK((header_value.index() == 0));
    if ((header_value.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(header_value).type_, std::uint64_t{0});
        RUVIA_CHECK_EQ(std::get<0>(header_value).length_, std::uint64_t{2});
    }
    RUVIA_CHECK((plan.plan_chunk(first, false).index() != 0));
    RUVIA_CHECK((plan.commit_payload(2, false).index() == 0));
    RUVIA_CHECK_EQ(plan.committed_payload_bytes(), std::uint64_t{2});
    RUVIA_CHECK(!plan.fin_allowed());

    std::array<char, 3> second{'c', 'd', 'e'};
    const auto second_chunk = plan.plan_chunk(second, true);
    RUVIA_CHECK((second_chunk.index() == 0));
    if ((second_chunk.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(second_chunk).payload_.data(), second.data());
    }
    RUVIA_CHECK((plan.commit_payload(3, true).index() == 0));
    RUVIA_CHECK(plan.finished());
    RUVIA_CHECK(!plan.fin_allowed());
    RUVIA_CHECK(std::get<1>(plan.plan_chunk({}, true)) == http3_data_write_error::already_finished);
}

RUVIA_TEST(http3_data_write_plan_handles_empty_fin_and_rejects_bad_lengths) {
    http3_data_write_plan empty(plan_http_response_body(http_known_method::get, http_status_code::from_value(200)), 0);
    const auto fin = empty.plan_chunk({}, true);
    RUVIA_CHECK((fin.index() == 0));
    if ((fin.index() == 0)) {
        RUVIA_CHECK(!std::get<0>(fin).emits_data_);
        RUVIA_CHECK_EQ(std::get<0>(fin).frame_header_size_, std::size_t{0});
    }
    RUVIA_CHECK((empty.commit_payload(0, true).index() == 0));

    http3_data_write_plan too_long(plan_http_response_body(http_known_method::get, http_status_code::from_value(200)), 1);
    std::array<char, 2> bytes_value{'x', 'y'};
    RUVIA_CHECK(std::get<1>(too_long.plan_chunk(bytes_value, false)) == http3_data_write_error::content_length_mismatch);
    RUVIA_CHECK_EQ(too_long.committed_payload_bytes(), std::uint64_t{0});
    RUVIA_CHECK(std::get<1>(too_long.plan_chunk(bytes_value, true)) == http3_data_write_error::content_length_mismatch);

    http3_data_write_plan mismatch(plan_http_response_body(http_known_method::get, http_status_code::from_value(200)), 2);
    RUVIA_CHECK(std::get<1>(mismatch.plan_chunk(std::span<const char>(bytes_value).first(1), true)) ==
                http3_data_write_error::content_length_mismatch);
}

RUVIA_TEST(http3_data_write_plan_checks_varint_limit_and_commit_contract) {
    http3_data_write_plan plan(plan_http_response_body(http_known_method::get, http_status_code::from_value(200)), std::nullopt);
    constexpr std::array<char, 1> byte{'x'};
    RUVIA_CHECK((plan.plan_chunk(byte, false).index() == 0));
    RUVIA_CHECK(std::get<1>(plan.commit_payload(2, false)) == http3_data_write_error::commit_does_not_match_plan);
    RUVIA_CHECK_EQ(plan.committed_payload_bytes(), std::uint64_t{0});
    RUVIA_CHECK((plan.commit_payload(1, false).index() == 0));
    RUVIA_CHECK_EQ(plan.committed_payload_bytes(), std::uint64_t{1});
}

RUVIA_TEST(http3_data_write_plan_supports_connect_tunnels_without_content_length) {
    const auto tunnel_policy = plan_http_response_body(http_known_method::connect,
        http_status_code::from_value(200));
    http3_data_write_plan tunnel(tunnel_policy, std::nullopt);
    const std::array<char, 2> bytes_value{'h', 'i'};
    RUVIA_CHECK(tunnel.body_allowed());
    RUVIA_CHECK((tunnel.plan_chunk(bytes_value, false).index() == 0));
    RUVIA_CHECK((tunnel.commit_payload(2, false).index() == 0));
    RUVIA_CHECK((tunnel.plan_chunk({}, true).index() == 0));
    RUVIA_CHECK((tunnel.commit_payload(0, true).index() == 0));

    http3_data_write_plan illegal_length(tunnel_policy, 2);
    RUVIA_CHECK(!illegal_length.fin_allowed());
    RUVIA_CHECK(std::get<1>(illegal_length.plan_chunk(bytes_value, true)) ==
                http3_data_write_error::content_length_forbidden);
}

RUVIA_TEST(http3_data_write_plan_supports_request_body_with_content_length) {
    http3_data_write_plan plan(http3_client_request_body_plan{.expected_length_ = 5});
    std::array<char, 2> first{'a', 'b'};
    const auto first_chunk = plan.plan_chunk(first, false);
    RUVIA_CHECK((first_chunk.index() == 0));
    if ((first_chunk.index() == 0)) {
        RUVIA_CHECK(std::get<0>(first_chunk).emits_data_);
        const auto header_value = decode_http3_frame_header(
            std::span<const char>(std::get<0>(first_chunk).frame_header_).first(std::get<0>(first_chunk).frame_header_size_));
        RUVIA_CHECK((header_value.index() == 0));
        if ((header_value.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(header_value).type_, std::uint64_t{0});
            RUVIA_CHECK_EQ(std::get<0>(header_value).length_, std::uint64_t{2});
        }
    }
    RUVIA_CHECK((plan.commit_payload(2, false).index() == 0));
    std::array<char, 3> second{'c', 'd', 'e'};
    RUVIA_CHECK((plan.plan_chunk(second, true).index() == 0));
    RUVIA_CHECK((plan.commit_payload(3, true).index() == 0));
    RUVIA_CHECK(plan.finished());
}

RUVIA_TEST(http3_data_write_plan_supports_streaming_requests_and_empty_fin) {
    http3_data_write_plan streaming(http3_client_request_body_plan{});
    const std::array<char, 2> data{'o', 'k'};
    RUVIA_CHECK((streaming.plan_chunk(data, false).index() == 0));
    RUVIA_CHECK((streaming.commit_payload(2, false).index() == 0));
    RUVIA_CHECK(streaming.fin_allowed());
    RUVIA_CHECK((streaming.plan_chunk({}, true).index() == 0));
    RUVIA_CHECK((streaming.commit_payload(0, true).index() == 0));
    RUVIA_CHECK(streaming.finished());

    http3_data_write_plan empty_get(http3_client_request_body_plan{});
    const auto fin = empty_get.plan_chunk({}, true);
    RUVIA_CHECK((fin.index() == 0));
    if ((fin.index() == 0)) {
        RUVIA_CHECK(!std::get<0>(fin).emits_data_);
        RUVIA_CHECK_EQ(std::get<0>(fin).frame_header_size_, std::size_t{0});
    }
    RUVIA_CHECK((empty_get.commit_payload(0, true).index() == 0));
}

RUVIA_TEST(http3_data_write_plan_rejects_request_length_mismatch_and_pending_abandonment) {
    http3_data_write_plan short_body(http3_client_request_body_plan{.expected_length_ = 3});
    const std::array<char, 2> data{'n', 'o'};
    RUVIA_CHECK(std::get<1>(short_body.plan_chunk(data, true)) == http3_data_write_error::content_length_mismatch);
    RUVIA_CHECK((short_body.plan_chunk(data, false).index() == 0));
    RUVIA_CHECK(std::get<1>(short_body.commit_payload(1, false)) == http3_data_write_error::commit_does_not_match_plan);
    // An uncommitted write is abandoned, never implicitly committed or FINished.
    {
        http3_data_write_plan pending(http3_client_request_body_plan{});
        RUVIA_CHECK((pending.plan_chunk(data, false).index() == 0));
    }
}

RUVIA_TEST(http3_data_write_plan_supports_headers_only_responses) {
    http3_data_write_plan head(plan_http_response_body(http_known_method::head, http_status_code::from_value(200)), 123);
    RUVIA_CHECK(!head.body_allowed());
    RUVIA_CHECK(head.fin_allowed());
    RUVIA_CHECK((head.plan_chunk({}, true).index() == 0));
    RUVIA_CHECK((head.commit_payload(0, true).index() == 0));

    http3_data_write_plan no_content(plan_http_response_body(http_known_method::get, http_status_code::from_value(204)), std::nullopt);
    RUVIA_CHECK(!no_content.body_allowed());
    RUVIA_CHECK((no_content.plan_chunk({}, true).index() == 0));
    RUVIA_CHECK((no_content.commit_payload(0, true).index() == 0));

    http3_data_write_plan not_modified(plan_http_response_body(http_known_method::get,
                                           http_status_code::from_value(304)),
        123);
    RUVIA_CHECK(!not_modified.body_allowed());
    RUVIA_CHECK((not_modified.plan_chunk({}, true).index() == 0));
    RUVIA_CHECK((not_modified.commit_payload(0, true).index() == 0));
}
