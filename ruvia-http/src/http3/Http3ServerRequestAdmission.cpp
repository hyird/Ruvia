#include "ruvia/http/Http3ServerRequestAdmission.h"

#include <array>
#include <cstdint>
#include <span>
#include <variant>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3PeerStreams.h"
#include "ruvia/http/Http3VarInt.h"

namespace ruvia {
namespace {

constexpr std::uint64_t kMaxServerGoawayId = kHttp3VarIntMax & ~std::uint64_t{3};
constexpr std::uint64_t kMaxRequestsPerConnection = kMaxServerGoawayId / 4;

[[nodiscard]] bool isRequestStreamId(std::uint64_t streamId) noexcept {
    return (Http3PeerStreams::acceptBidirectional(Http3PeerRole::kServer, streamId).index() == 0);
}

}  // namespace

std::variant<Http3ServerRequestAdmissionPlanner, Http3ServerRequestAdmissionError>
Http3ServerRequestAdmissionPlanner::create(Http3ServerRequestAdmissionConfig config) noexcept {
    if (config.max_requests_per_connection == 0) {
        return Http3ServerRequestAdmissionError::kZeroRequestLimit;
    }
    if (config.max_requests_per_connection > kMaxRequestsPerConnection) {
        return Http3ServerRequestAdmissionError::kRequestLimitOutOfRange;
    }
    return Http3ServerRequestAdmissionPlanner(config.max_requests_per_connection);
}

Http3ServerRequestAdmissionDecision Http3ServerRequestAdmissionPlanner::admit(
    std::uint64_t streamId) noexcept {
    if (!isRequestStreamId(streamId)) {
        return {.action = Http3ServerRequestAdmissionAction::kReject,
            .rejection = Http3ServerRequestAdmissionRejection::kInvalidStreamId,
            .goawayId = goawayId_};
    }
    if (streamId >= goawayId_) {
        const auto announcement = announceGoaway();
        return {.action = announcement.emitGoaway
                              ? Http3ServerRequestAdmissionAction::kAnnounceGoaway
                              : Http3ServerRequestAdmissionAction::kReject,
            .rejection = Http3ServerRequestAdmissionRejection::kRequestLimitReached,
            .goawayId = goawayId_,
            .emitGoaway = announcement.emitGoaway};
    }
    return {.action = Http3ServerRequestAdmissionAction::kAdmit, .goawayId = goawayId_};
}

Http3ServerRequestAdmissionDecision Http3ServerRequestAdmissionPlanner::announceGoaway() noexcept {
    const bool emitGoaway = !goawayAnnounced_;
    goawayAnnounced_ = true;
    return {.action = Http3ServerRequestAdmissionAction::kAnnounceGoaway,
        .goawayId = goawayId_,
        .emitGoaway = emitGoaway};
}

std::variant<std::size_t, Http3ServerRequestAdmissionError> encodeHttp3ServerGoawayFrame(
    std::span<char> output, std::uint64_t goawayId) noexcept {
    if (!isRequestStreamId(goawayId)) {
        return Http3ServerRequestAdmissionError::kInvalidStreamId;
    }

    std::array<char, kHttp3VarIntMaxBytes> payload{};
    const auto payloadSize = encodeHttp3VarInt(payload, goawayId);
    if ((payloadSize.index() != 0)) {
        return Http3ServerRequestAdmissionError::kInvalidStreamId;
    }
    const auto written = encodeHttp3Frame(output, static_cast<std::uint64_t>(Http3FrameType::kGoaway),
        std::span<const char>(payload).first(std::get<0>(payloadSize)));
    if ((written.index() != 0)) {
        return Http3ServerRequestAdmissionError::kOutputTooSmall;
    }
    return std::get<0>(written);
}

}  // namespace ruvia
