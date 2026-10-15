#include <array>
#include <cstdint>
#include <span>
#include <variant>

#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_server_request_admission.h"
#include "ruvia/http/http3_var_int.h"

#include "test_harness.h"

namespace {

using ruvia::http3_server_request_admission_action;
using ruvia::http3_server_request_admission_config;
using ruvia::http3_server_request_admission_error;
using ruvia::http3_server_request_admission_planner;
using ruvia::http3_server_request_admission_rejection;

}  // namespace

RUVIA_TEST(http3_server_request_admission_uses_checked_finite_limits) {
    const auto zero = http3_server_request_admission_planner::create({.max_requests_per_connection_ = 0});
    RUVIA_CHECK(!(zero.index() == 0));
    if ((zero.index() != 0)) {
        RUVIA_CHECK(std::get<1>(zero) == http3_server_request_admission_error::zero_request_limit);
    }

    const auto too_large = http3_server_request_admission_planner::create(
        {.max_requests_per_connection_ = ruvia::http3_var_int_max / 4 + 1});
    RUVIA_CHECK(!(too_large.index() == 0));
    if ((too_large.index() != 0)) {
        RUVIA_CHECK(std::get<1>(too_large) == http3_server_request_admission_error::request_limit_out_of_range);
    }

    auto one = http3_server_request_admission_planner::create({.max_requests_per_connection_ = 1});
    RUVIA_CHECK((one.index() == 0));
    if ((one.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ(std::get<0>(one).max_requests_per_connection(), std::uint64_t{1});
    RUVIA_CHECK_EQ(std::get<0>(one).goaway_id(), std::uint64_t{4});

    const auto over_limit = std::get<0>(one).admit(4);
    RUVIA_CHECK(over_limit.action_ == http3_server_request_admission_action::announce_goaway);
    RUVIA_CHECK(over_limit.rejection_ == http3_server_request_admission_rejection::request_limit_reached);
    RUVIA_CHECK(over_limit.emit_goaway_);
    RUVIA_CHECK_EQ(over_limit.goaway_id_, std::uint64_t{4});

    const auto repeated_high_stream = std::get<0>(one).admit(8);
    RUVIA_CHECK(repeated_high_stream.action_ == http3_server_request_admission_action::reject);
    RUVIA_CHECK(repeated_high_stream.rejection_ ==
                http3_server_request_admission_rejection::request_limit_reached);
    RUVIA_CHECK(!repeated_high_stream.emit_goaway_);
    const auto repeated_announcement = std::get<0>(one).announce_goaway();
    RUVIA_CHECK(repeated_announcement.action_ == http3_server_request_admission_action::announce_goaway);
    RUVIA_CHECK(!repeated_announcement.emit_goaway_);
    RUVIA_CHECK_EQ(repeated_announcement.goaway_id_, std::uint64_t{4});
    RUVIA_CHECK(std::get<0>(one).goaway_announced());
    const auto late_low_stream = std::get<0>(one).admit(0);
    RUVIA_CHECK(late_low_stream.action_ == http3_server_request_admission_action::admit);
    RUVIA_CHECK(!late_low_stream.emit_goaway_);
}

RUVIA_TEST(http3_server_request_admission_keeps_the_fixed_cutoff_for_out_of_order_streams) {
    auto planner = http3_server_request_admission_planner::create({.max_requests_per_connection_ = 126});
    RUVIA_CHECK((planner.index() == 0));
    if ((planner.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ(std::get<0>(planner).goaway_id(), std::uint64_t{504});
    RUVIA_CHECK(std::get<0>(planner).admit(500).action_ == http3_server_request_admission_action::admit);
    RUVIA_CHECK(std::get<0>(planner).admit(400).action_ == http3_server_request_admission_action::admit);
    RUVIA_CHECK(std::get<0>(planner).admit(0).action_ == http3_server_request_admission_action::admit);
    const auto drain = std::get<0>(planner).announce_goaway();
    RUVIA_CHECK(drain.action_ == http3_server_request_admission_action::announce_goaway);
    RUVIA_CHECK(drain.emit_goaway_);
    RUVIA_CHECK_EQ(drain.goaway_id_, std::uint64_t{504});
    RUVIA_CHECK(std::get<0>(planner).admit(4).action_ == http3_server_request_admission_action::admit);
    RUVIA_CHECK(std::get<0>(planner).admit(496).action_ == http3_server_request_admission_action::admit);
    const auto later_high_stream = std::get<0>(planner).admit(504);
    RUVIA_CHECK(later_high_stream.action_ == http3_server_request_admission_action::reject);
    RUVIA_CHECK(!later_high_stream.emit_goaway_);
    RUVIA_CHECK(!std::get<0>(planner).announce_goaway().emit_goaway_);

    auto reverse_order = http3_server_request_admission_planner::create({.max_requests_per_connection_ = 126});
    RUVIA_CHECK((reverse_order.index() == 0));
    if ((reverse_order.index() == 0)) {
        RUVIA_CHECK(std::get<0>(reverse_order).admit(0).action_ == http3_server_request_admission_action::admit);
        RUVIA_CHECK(std::get<0>(reverse_order).admit(400).action_ == http3_server_request_admission_action::admit);
        RUVIA_CHECK_EQ(std::get<0>(reverse_order).announce_goaway().goaway_id_, std::uint64_t{504});
    }
}

RUVIA_TEST(http3_server_request_admission_enforces_the_thousand_request_boundary) {
    auto planner = http3_server_request_admission_planner::create({.max_requests_per_connection_ = 1000});
    RUVIA_CHECK((planner.index() == 0));
    if ((planner.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ(std::get<0>(planner).goaway_id(), std::uint64_t{4000});
    RUVIA_CHECK(std::get<0>(planner).admit(3996).action_ == http3_server_request_admission_action::admit);
    const auto boundary = std::get<0>(planner).admit(4000);
    RUVIA_CHECK(boundary.action_ == http3_server_request_admission_action::announce_goaway);
    RUVIA_CHECK(boundary.rejection_ == http3_server_request_admission_rejection::request_limit_reached);
    RUVIA_CHECK(boundary.emit_goaway_);
    RUVIA_CHECK_EQ(boundary.goaway_id_, std::uint64_t{4000});
    RUVIA_CHECK(std::get<0>(planner).admit(0).action_ == http3_server_request_admission_action::admit);
    RUVIA_CHECK(std::get<0>(planner).admit(4000).action_ == http3_server_request_admission_action::reject);
    RUVIA_CHECK(!std::get<0>(planner).admit(4000).emit_goaway_);
}

RUVIA_TEST(http3_server_request_admission_rejects_non_request_stream_ids) {
    auto planner = http3_server_request_admission_planner::create({.max_requests_per_connection_ = 1000});
    RUVIA_CHECK((planner.index() == 0));
    if ((planner.index() != 0)) {
        return;
    }
    constexpr std::array<std::uint64_t, 5> invalid_stream_ids{
        1, 2, 3, ruvia::http3_var_int_max, ruvia::http3_var_int_max + 1};
    for (const auto stream_id : invalid_stream_ids) {
        const auto result_value = std::get<0>(planner).admit(stream_id);
        RUVIA_CHECK(result_value.action_ == http3_server_request_admission_action::reject);
        RUVIA_CHECK(result_value.rejection_ == http3_server_request_admission_rejection::invalid_stream_id);
    }
    RUVIA_CHECK(!std::get<0>(planner).goaway_announced());
    RUVIA_CHECK(std::get<0>(planner).admit(0).action_ == http3_server_request_admission_action::admit);
}

RUVIA_TEST(http3_server_goaway_encoder_emits_a_complete_decodable_frame_at_varint_boundaries) {
    constexpr std::uint64_t max_request_stream_id = ruvia::http3_var_int_max & ~std::uint64_t{3};
    constexpr std::array<std::uint64_t, 4> goaway_ids{4, 504, 4000, max_request_stream_id};
    for (const auto goaway_id : goaway_ids) {
        std::array<char, 10> wire{};
        const auto written = ruvia::encode_http3_server_goaway_frame(wire, goaway_id);
        RUVIA_CHECK((written.index() == 0));
        if ((written.index() != 0)) {
            continue;
        }
        const auto frame = ruvia::decode_http3_frame(std::span<const char>(wire).first(std::get<0>(written)));
        RUVIA_CHECK((frame.index() == 0));
        if ((frame.index() != 0)) {
            continue;
        }
        RUVIA_CHECK(std::get<0>(frame).type_ == static_cast<std::uint64_t>(ruvia::http3_frame_type::goaway));
        RUVIA_CHECK_EQ(std::get<0>(frame).encoded_bytes_, std::get<0>(written));
        const auto payload_value = ruvia::decode_http3_var_int(std::get<0>(frame).payload_);
        RUVIA_CHECK((payload_value.index() == 0));
        if ((payload_value.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(payload_value).value_, goaway_id);
            RUVIA_CHECK_EQ(std::get<0>(payload_value).encoded_bytes_, std::get<0>(frame).payload_.size());
        }
    }

    std::array<char, 10> output{};
    RUVIA_CHECK(std::get<1>(ruvia::encode_http3_server_goaway_frame(output, ruvia::http3_var_int_max)) ==
                http3_server_request_admission_error::invalid_stream_id);
    RUVIA_CHECK(std::get<1>(ruvia::encode_http3_server_goaway_frame(output, ruvia::http3_var_int_max + 1)) ==
                http3_server_request_admission_error::invalid_stream_id);

    std::array<char, 2> short_output{'#', '#'};
    const auto before = short_output;
    const auto short_result = ruvia::encode_http3_server_goaway_frame(short_output, 4);
    RUVIA_CHECK(!(short_result.index() == 0));
    if ((short_result.index() != 0)) {
        RUVIA_CHECK(std::get<1>(short_result) == http3_server_request_admission_error::output_too_small);
    }
    RUVIA_CHECK_EQ(short_output, before);
}

RUVIA_TEST(http3_server_request_admission_accepts_the_largest_checked_finite_limit) {
    constexpr auto max_requests = ruvia::http3_var_int_max / 4;
    constexpr auto max_goaway_id = ruvia::http3_var_int_max & ~std::uint64_t{3};
    auto planner = http3_server_request_admission_planner::create(
        http3_server_request_admission_config{.max_requests_per_connection_ = max_requests});
    RUVIA_CHECK((planner.index() == 0));
    if ((planner.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(planner).goaway_id(), max_goaway_id);
        RUVIA_CHECK(std::get<0>(planner).admit(max_goaway_id - 4).action_ == http3_server_request_admission_action::admit);
        RUVIA_CHECK_EQ(std::get<0>(planner).announce_goaway().goaway_id_, max_goaway_id);
    }
}
