#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include "ruvia/http/Http3DataWritePlan.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpStatus.h"

#include "test_harness.h"

namespace {

using ruvia::decodeHttp3FrameHeader;
using ruvia::Http3ClientRequestBodyPlan;
using ruvia::Http3DataWriteError;
using ruvia::Http3DataWritePlan;
using ruvia::HttpKnownMethod;
using ruvia::HttpStatusCode;
using ruvia::kHttp3VarIntMax;
using ruvia::planHttpResponseBody;

}  // namespace

RUVIA_TEST(http3_data_write_plan_borrows_repeated_chunks_and_commits_only_after_write) {
    Http3DataWritePlan plan(planHttpResponseBody(HttpKnownMethod::kGet, HttpStatusCode::fromValue(200)), 5);
    std::array<char, 2> first{'a', 'b'};
    RUVIA_CHECK(!plan.finAllowed());
    const auto firstChunk = plan.planChunk(first, false);
    RUVIA_CHECK((firstChunk.index() == 0));
    if ((firstChunk.index() != 0)) {
        return;
    }
    RUVIA_CHECK(std::get<0>(firstChunk).emitsData);
    RUVIA_CHECK_EQ(std::get<0>(firstChunk).payload.data(), first.data());
    const auto header = decodeHttp3FrameHeader(
        std::span<const char>(std::get<0>(firstChunk).frameHeader).first(std::get<0>(firstChunk).frameHeaderSize));
    RUVIA_CHECK((header.index() == 0));
    if ((header.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(header).type, std::uint64_t{0});
        RUVIA_CHECK_EQ(std::get<0>(header).length, std::uint64_t{2});
    }
    RUVIA_CHECK((plan.planChunk(first, false).index() != 0));
    RUVIA_CHECK((plan.commitPayload(2, false).index() == 0));
    RUVIA_CHECK_EQ(plan.committedPayloadBytes(), std::uint64_t{2});
    RUVIA_CHECK(!plan.finAllowed());

    std::array<char, 3> second{'c', 'd', 'e'};
    const auto secondChunk = plan.planChunk(second, true);
    RUVIA_CHECK((secondChunk.index() == 0));
    if ((secondChunk.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(secondChunk).payload.data(), second.data());
    }
    RUVIA_CHECK((plan.commitPayload(3, true).index() == 0));
    RUVIA_CHECK(plan.finished());
    RUVIA_CHECK(!plan.finAllowed());
    RUVIA_CHECK(std::get<1>(plan.planChunk({}, true)) == Http3DataWriteError::kAlreadyFinished);
}

RUVIA_TEST(http3_data_write_plan_handles_empty_fin_and_rejects_bad_lengths) {
    Http3DataWritePlan empty(planHttpResponseBody(HttpKnownMethod::kGet, HttpStatusCode::fromValue(200)), 0);
    const auto fin = empty.planChunk({}, true);
    RUVIA_CHECK((fin.index() == 0));
    if ((fin.index() == 0)) {
        RUVIA_CHECK(!std::get<0>(fin).emitsData);
        RUVIA_CHECK_EQ(std::get<0>(fin).frameHeaderSize, std::size_t{0});
    }
    RUVIA_CHECK((empty.commitPayload(0, true).index() == 0));

    Http3DataWritePlan tooLong(planHttpResponseBody(HttpKnownMethod::kGet, HttpStatusCode::fromValue(200)), 1);
    std::array<char, 2> bytes{'x', 'y'};
    RUVIA_CHECK(std::get<1>(tooLong.planChunk(bytes, false)) == Http3DataWriteError::kContentLengthMismatch);
    RUVIA_CHECK_EQ(tooLong.committedPayloadBytes(), std::uint64_t{0});
    RUVIA_CHECK(std::get<1>(tooLong.planChunk(bytes, true)) == Http3DataWriteError::kContentLengthMismatch);

    Http3DataWritePlan mismatch(planHttpResponseBody(HttpKnownMethod::kGet, HttpStatusCode::fromValue(200)), 2);
    RUVIA_CHECK(std::get<1>(mismatch.planChunk(std::span<const char>(bytes).first(1), true)) ==
                Http3DataWriteError::kContentLengthMismatch);
}

RUVIA_TEST(http3_data_write_plan_checks_varint_limit_and_commit_contract) {
    Http3DataWritePlan plan(planHttpResponseBody(HttpKnownMethod::kGet, HttpStatusCode::fromValue(200)), std::nullopt);
    constexpr std::array<char, 1> byte{'x'};
    RUVIA_CHECK((plan.planChunk(byte, false).index() == 0));
    RUVIA_CHECK(std::get<1>(plan.commitPayload(2, false)) == Http3DataWriteError::kCommitDoesNotMatchPlan);
    RUVIA_CHECK_EQ(plan.committedPayloadBytes(), std::uint64_t{0});
    RUVIA_CHECK((plan.commitPayload(1, false).index() == 0));
    RUVIA_CHECK_EQ(plan.committedPayloadBytes(), std::uint64_t{1});
}

RUVIA_TEST(http3_data_write_plan_supports_connect_tunnels_without_content_length) {
    const auto tunnelPolicy = planHttpResponseBody(HttpKnownMethod::kConnect,
        HttpStatusCode::fromValue(200));
    Http3DataWritePlan tunnel(tunnelPolicy, std::nullopt);
    const std::array<char, 2> bytes{'h', 'i'};
    RUVIA_CHECK(tunnel.bodyAllowed());
    RUVIA_CHECK((tunnel.planChunk(bytes, false).index() == 0));
    RUVIA_CHECK((tunnel.commitPayload(2, false).index() == 0));
    RUVIA_CHECK((tunnel.planChunk({}, true).index() == 0));
    RUVIA_CHECK((tunnel.commitPayload(0, true).index() == 0));

    Http3DataWritePlan illegalLength(tunnelPolicy, 2);
    RUVIA_CHECK(!illegalLength.finAllowed());
    RUVIA_CHECK(std::get<1>(illegalLength.planChunk(bytes, true)) ==
                Http3DataWriteError::kContentLengthForbidden);
}

RUVIA_TEST(http3_data_write_plan_supports_request_body_with_content_length) {
    Http3DataWritePlan plan(Http3ClientRequestBodyPlan{.expectedLength = 5});
    std::array<char, 2> first{'a', 'b'};
    const auto firstChunk = plan.planChunk(first, false);
    RUVIA_CHECK((firstChunk.index() == 0));
    if ((firstChunk.index() == 0)) {
        RUVIA_CHECK(std::get<0>(firstChunk).emitsData);
        const auto header = decodeHttp3FrameHeader(
            std::span<const char>(std::get<0>(firstChunk).frameHeader).first(std::get<0>(firstChunk).frameHeaderSize));
        RUVIA_CHECK((header.index() == 0));
        if ((header.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(header).type, std::uint64_t{0});
            RUVIA_CHECK_EQ(std::get<0>(header).length, std::uint64_t{2});
        }
    }
    RUVIA_CHECK((plan.commitPayload(2, false).index() == 0));
    std::array<char, 3> second{'c', 'd', 'e'};
    RUVIA_CHECK((plan.planChunk(second, true).index() == 0));
    RUVIA_CHECK((plan.commitPayload(3, true).index() == 0));
    RUVIA_CHECK(plan.finished());
}

RUVIA_TEST(http3_data_write_plan_supports_streaming_requests_and_empty_fin) {
    Http3DataWritePlan streaming(Http3ClientRequestBodyPlan{});
    const std::array<char, 2> data{'o', 'k'};
    RUVIA_CHECK((streaming.planChunk(data, false).index() == 0));
    RUVIA_CHECK((streaming.commitPayload(2, false).index() == 0));
    RUVIA_CHECK(streaming.finAllowed());
    RUVIA_CHECK((streaming.planChunk({}, true).index() == 0));
    RUVIA_CHECK((streaming.commitPayload(0, true).index() == 0));
    RUVIA_CHECK(streaming.finished());

    Http3DataWritePlan emptyGet(Http3ClientRequestBodyPlan{});
    const auto fin = emptyGet.planChunk({}, true);
    RUVIA_CHECK((fin.index() == 0));
    if ((fin.index() == 0)) {
        RUVIA_CHECK(!std::get<0>(fin).emitsData);
        RUVIA_CHECK_EQ(std::get<0>(fin).frameHeaderSize, std::size_t{0});
    }
    RUVIA_CHECK((emptyGet.commitPayload(0, true).index() == 0));
}

RUVIA_TEST(http3_data_write_plan_rejects_request_length_mismatch_and_pending_abandonment) {
    Http3DataWritePlan shortBody(Http3ClientRequestBodyPlan{.expectedLength = 3});
    const std::array<char, 2> data{'n', 'o'};
    RUVIA_CHECK(std::get<1>(shortBody.planChunk(data, true)) == Http3DataWriteError::kContentLengthMismatch);
    RUVIA_CHECK((shortBody.planChunk(data, false).index() == 0));
    RUVIA_CHECK(std::get<1>(shortBody.commitPayload(1, false)) == Http3DataWriteError::kCommitDoesNotMatchPlan);
    // An uncommitted write is abandoned, never implicitly committed or FINished.
    {
        Http3DataWritePlan pending(Http3ClientRequestBodyPlan{});
        RUVIA_CHECK((pending.planChunk(data, false).index() == 0));
    }
}

RUVIA_TEST(http3_data_write_plan_supports_headers_only_responses) {
    Http3DataWritePlan head(planHttpResponseBody(HttpKnownMethod::kHead, HttpStatusCode::fromValue(200)), 123);
    RUVIA_CHECK(!head.bodyAllowed());
    RUVIA_CHECK(head.finAllowed());
    RUVIA_CHECK((head.planChunk({}, true).index() == 0));
    RUVIA_CHECK((head.commitPayload(0, true).index() == 0));

    Http3DataWritePlan noContent(planHttpResponseBody(HttpKnownMethod::kGet, HttpStatusCode::fromValue(204)), std::nullopt);
    RUVIA_CHECK(!noContent.bodyAllowed());
    RUVIA_CHECK((noContent.planChunk({}, true).index() == 0));
    RUVIA_CHECK((noContent.commitPayload(0, true).index() == 0));

    Http3DataWritePlan notModified(planHttpResponseBody(HttpKnownMethod::kGet,
                                       HttpStatusCode::fromValue(304)),
        123);
    RUVIA_CHECK(!notModified.bodyAllowed());
    RUVIA_CHECK((notModified.planChunk({}, true).index() == 0));
    RUVIA_CHECK((notModified.commitPayload(0, true).index() == 0));
}
