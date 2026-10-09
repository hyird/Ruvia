#include <array>
#include <cstdint>
#include <span>
#include <variant>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3ServerRequestAdmission.h"
#include "ruvia/http/Http3VarInt.h"

#include "test_harness.h"

namespace {

using ruvia::Http3ServerRequestAdmissionAction;
using ruvia::Http3ServerRequestAdmissionConfig;
using ruvia::Http3ServerRequestAdmissionError;
using ruvia::Http3ServerRequestAdmissionPlanner;
using ruvia::Http3ServerRequestAdmissionRejection;

}  // namespace

RUVIA_TEST(http3_server_request_admission_uses_checked_finite_limits) {
    const auto zero = Http3ServerRequestAdmissionPlanner::create({.max_requests_per_connection = 0});
    RUVIA_CHECK(!(zero.index() == 0));
    if ((zero.index() != 0)) {
        RUVIA_CHECK(std::get<1>(zero) == Http3ServerRequestAdmissionError::kZeroRequestLimit);
    }

    const auto tooLarge = Http3ServerRequestAdmissionPlanner::create(
        {.max_requests_per_connection = ruvia::kHttp3VarIntMax / 4 + 1});
    RUVIA_CHECK(!(tooLarge.index() == 0));
    if ((tooLarge.index() != 0)) {
        RUVIA_CHECK(std::get<1>(tooLarge) == Http3ServerRequestAdmissionError::kRequestLimitOutOfRange);
    }

    auto one = Http3ServerRequestAdmissionPlanner::create({.max_requests_per_connection = 1});
    RUVIA_CHECK((one.index() == 0));
    if ((one.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ(std::get<0>(one).max_requests_per_connection(), std::uint64_t{1});
    RUVIA_CHECK_EQ(std::get<0>(one).goawayId(), std::uint64_t{4});

    const auto overLimit = std::get<0>(one).admit(4);
    RUVIA_CHECK(overLimit.action == Http3ServerRequestAdmissionAction::kAnnounceGoaway);
    RUVIA_CHECK(overLimit.rejection == Http3ServerRequestAdmissionRejection::kRequestLimitReached);
    RUVIA_CHECK(overLimit.emitGoaway);
    RUVIA_CHECK_EQ(overLimit.goawayId, std::uint64_t{4});

    const auto repeatedHighStream = std::get<0>(one).admit(8);
    RUVIA_CHECK(repeatedHighStream.action == Http3ServerRequestAdmissionAction::kReject);
    RUVIA_CHECK(repeatedHighStream.rejection ==
                Http3ServerRequestAdmissionRejection::kRequestLimitReached);
    RUVIA_CHECK(!repeatedHighStream.emitGoaway);
    const auto repeatedAnnouncement = std::get<0>(one).announceGoaway();
    RUVIA_CHECK(repeatedAnnouncement.action == Http3ServerRequestAdmissionAction::kAnnounceGoaway);
    RUVIA_CHECK(!repeatedAnnouncement.emitGoaway);
    RUVIA_CHECK_EQ(repeatedAnnouncement.goawayId, std::uint64_t{4});
    RUVIA_CHECK(std::get<0>(one).goawayAnnounced());
    const auto lateLowStream = std::get<0>(one).admit(0);
    RUVIA_CHECK(lateLowStream.action == Http3ServerRequestAdmissionAction::kAdmit);
    RUVIA_CHECK(!lateLowStream.emitGoaway);
}

RUVIA_TEST(http3_server_request_admission_keeps_the_fixed_cutoff_for_out_of_order_streams) {
    auto planner = Http3ServerRequestAdmissionPlanner::create({.max_requests_per_connection = 126});
    RUVIA_CHECK((planner.index() == 0));
    if ((planner.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ(std::get<0>(planner).goawayId(), std::uint64_t{504});
    RUVIA_CHECK(std::get<0>(planner).admit(500).action == Http3ServerRequestAdmissionAction::kAdmit);
    RUVIA_CHECK(std::get<0>(planner).admit(400).action == Http3ServerRequestAdmissionAction::kAdmit);
    RUVIA_CHECK(std::get<0>(planner).admit(0).action == Http3ServerRequestAdmissionAction::kAdmit);
    const auto drain = std::get<0>(planner).announceGoaway();
    RUVIA_CHECK(drain.action == Http3ServerRequestAdmissionAction::kAnnounceGoaway);
    RUVIA_CHECK(drain.emitGoaway);
    RUVIA_CHECK_EQ(drain.goawayId, std::uint64_t{504});
    RUVIA_CHECK(std::get<0>(planner).admit(4).action == Http3ServerRequestAdmissionAction::kAdmit);
    RUVIA_CHECK(std::get<0>(planner).admit(496).action == Http3ServerRequestAdmissionAction::kAdmit);
    const auto laterHighStream = std::get<0>(planner).admit(504);
    RUVIA_CHECK(laterHighStream.action == Http3ServerRequestAdmissionAction::kReject);
    RUVIA_CHECK(!laterHighStream.emitGoaway);
    RUVIA_CHECK(!std::get<0>(planner).announceGoaway().emitGoaway);

    auto reverseOrder = Http3ServerRequestAdmissionPlanner::create({.max_requests_per_connection = 126});
    RUVIA_CHECK((reverseOrder.index() == 0));
    if ((reverseOrder.index() == 0)) {
        RUVIA_CHECK(std::get<0>(reverseOrder).admit(0).action == Http3ServerRequestAdmissionAction::kAdmit);
        RUVIA_CHECK(std::get<0>(reverseOrder).admit(400).action == Http3ServerRequestAdmissionAction::kAdmit);
        RUVIA_CHECK_EQ(std::get<0>(reverseOrder).announceGoaway().goawayId, std::uint64_t{504});
    }
}

RUVIA_TEST(http3_server_request_admission_enforces_the_thousand_request_boundary) {
    auto planner = Http3ServerRequestAdmissionPlanner::create({.max_requests_per_connection = 1000});
    RUVIA_CHECK((planner.index() == 0));
    if ((planner.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ(std::get<0>(planner).goawayId(), std::uint64_t{4000});
    RUVIA_CHECK(std::get<0>(planner).admit(3996).action == Http3ServerRequestAdmissionAction::kAdmit);
    const auto boundary = std::get<0>(planner).admit(4000);
    RUVIA_CHECK(boundary.action == Http3ServerRequestAdmissionAction::kAnnounceGoaway);
    RUVIA_CHECK(boundary.rejection == Http3ServerRequestAdmissionRejection::kRequestLimitReached);
    RUVIA_CHECK(boundary.emitGoaway);
    RUVIA_CHECK_EQ(boundary.goawayId, std::uint64_t{4000});
    RUVIA_CHECK(std::get<0>(planner).admit(0).action == Http3ServerRequestAdmissionAction::kAdmit);
    RUVIA_CHECK(std::get<0>(planner).admit(4000).action == Http3ServerRequestAdmissionAction::kReject);
    RUVIA_CHECK(!std::get<0>(planner).admit(4000).emitGoaway);
}

RUVIA_TEST(http3_server_request_admission_rejects_non_request_stream_ids) {
    auto planner = Http3ServerRequestAdmissionPlanner::create({.max_requests_per_connection = 1000});
    RUVIA_CHECK((planner.index() == 0));
    if ((planner.index() != 0)) {
        return;
    }
    constexpr std::array<std::uint64_t, 5> invalidStreamIds{
        1, 2, 3, ruvia::kHttp3VarIntMax, ruvia::kHttp3VarIntMax + 1};
    for (const auto streamId : invalidStreamIds) {
        const auto result = std::get<0>(planner).admit(streamId);
        RUVIA_CHECK(result.action == Http3ServerRequestAdmissionAction::kReject);
        RUVIA_CHECK(result.rejection == Http3ServerRequestAdmissionRejection::kInvalidStreamId);
    }
    RUVIA_CHECK(!std::get<0>(planner).goawayAnnounced());
    RUVIA_CHECK(std::get<0>(planner).admit(0).action == Http3ServerRequestAdmissionAction::kAdmit);
}

RUVIA_TEST(http3_server_goaway_encoder_emits_a_complete_decodable_frame_at_varint_boundaries) {
    constexpr std::uint64_t maxRequestStreamId = ruvia::kHttp3VarIntMax & ~std::uint64_t{3};
    constexpr std::array<std::uint64_t, 4> goawayIds{4, 504, 4000, maxRequestStreamId};
    for (const auto goawayId : goawayIds) {
        std::array<char, 10> wire{};
        const auto written = ruvia::encodeHttp3ServerGoawayFrame(wire, goawayId);
        RUVIA_CHECK((written.index() == 0));
        if ((written.index() != 0)) {
            continue;
        }
        const auto frame = ruvia::decodeHttp3Frame(std::span<const char>(wire).first(std::get<0>(written)));
        RUVIA_CHECK((frame.index() == 0));
        if ((frame.index() != 0)) {
            continue;
        }
        RUVIA_CHECK(std::get<0>(frame).type == static_cast<std::uint64_t>(ruvia::Http3FrameType::kGoaway));
        RUVIA_CHECK_EQ(std::get<0>(frame).encodedBytes, std::get<0>(written));
        const auto payload = ruvia::decodeHttp3VarInt(std::get<0>(frame).payload);
        RUVIA_CHECK((payload.index() == 0));
        if ((payload.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(payload).value, goawayId);
            RUVIA_CHECK_EQ(std::get<0>(payload).encodedBytes, std::get<0>(frame).payload.size());
        }
    }

    std::array<char, 10> output{};
    RUVIA_CHECK(std::get<1>(ruvia::encodeHttp3ServerGoawayFrame(output, ruvia::kHttp3VarIntMax)) ==
                Http3ServerRequestAdmissionError::kInvalidStreamId);
    RUVIA_CHECK(std::get<1>(ruvia::encodeHttp3ServerGoawayFrame(output, ruvia::kHttp3VarIntMax + 1)) ==
                Http3ServerRequestAdmissionError::kInvalidStreamId);

    std::array<char, 2> shortOutput{'#', '#'};
    const auto before = shortOutput;
    const auto shortResult = ruvia::encodeHttp3ServerGoawayFrame(shortOutput, 4);
    RUVIA_CHECK(!(shortResult.index() == 0));
    if ((shortResult.index() != 0)) {
        RUVIA_CHECK(std::get<1>(shortResult) == Http3ServerRequestAdmissionError::kOutputTooSmall);
    }
    RUVIA_CHECK_EQ(shortOutput, before);
}

RUVIA_TEST(http3_server_request_admission_accepts_the_largest_checked_finite_limit) {
    constexpr auto maxRequests = ruvia::kHttp3VarIntMax / 4;
    constexpr auto maxGoawayId = ruvia::kHttp3VarIntMax & ~std::uint64_t{3};
    auto planner = Http3ServerRequestAdmissionPlanner::create(
        Http3ServerRequestAdmissionConfig{.max_requests_per_connection = maxRequests});
    RUVIA_CHECK((planner.index() == 0));
    if ((planner.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(planner).goawayId(), maxGoawayId);
        RUVIA_CHECK(std::get<0>(planner).admit(maxGoawayId - 4).action == Http3ServerRequestAdmissionAction::kAdmit);
        RUVIA_CHECK_EQ(std::get<0>(planner).announceGoaway().goawayId, maxGoawayId);
    }
}
