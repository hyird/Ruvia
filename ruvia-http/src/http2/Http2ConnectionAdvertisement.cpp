#include <variant>

#include "ruvia/http/HttpConnectionAdvertisement.h"

#include "http2/Http2Connection.h"

namespace ruvia::detail {
bool Http2Connection::processAdvertisement(const Http2FrameHeader& header, std::string_view payload) {
    if (role_ != Http2Role::kClient) {
        return true;
    }
    if (header.type == static_cast<std::uint8_t>(Http2FrameType::kOrigin)) {
        if (!receiveOriginAdvertisements_ || header.streamId != 0 || (header.flags & 0xf) != 0) {
            return true;
        }
        auto advertisement = decodeHttpOriginAdvertisement({payload.data(), payload.size()}, resource_);
        if ((advertisement.index() == 0)) {
            reserveEventSlots(1);
            events_.push_back(Http2Event::originAdvertisement(std::move(std::get<0>(advertisement))));
        }
    } else {
        auto advertisement = decodeHttp2AlternativeService(header.streamId, {payload.data(), payload.size()}, resource_);
        if ((advertisement.index() == 0)) {
            reserveEventSlots(1);
            events_.push_back(Http2Event::alternativeServiceAdvertisement(std::move(std::get<0>(advertisement))));
        }
    }
    return true;
}
Http2SubmitStatus Http2Connection::submitOriginAdvertisement(std::span<const std::string_view> origins) {
    if (role_ != Http2Role::kServer) {
        return Http2SubmitStatus::kInvalidState;
    }
    if (localConnectionState_.fatalFailure() != nullptr || peerGoaway_) {
        return Http2SubmitStatus::kClosed;
    }
    if (prefacePhase_ != PrefacePhase::kReady) {
        return Http2SubmitStatus::kInvalidState;
    }
    const auto bytes = encodeHttp2OriginFrame(origins, peerSettings_.maxFrameSize(), resource_);
    if ((bytes.index() != 0)) {
        return Http2SubmitStatus::kInvalidMessage;
    }
    output_.appendBytes({std::get<0>(bytes).data(), std::get<0>(bytes).size()});
    return Http2SubmitStatus::kAccepted;
}
Http2SubmitStatus Http2Connection::submitAlternativeServiceAdvertisement(std::uint32_t streamId,
    std::string_view origin, std::string_view value) {
    if (role_ != Http2Role::kServer) {
        return Http2SubmitStatus::kInvalidState;
    }
    if (localConnectionState_.fatalFailure() != nullptr || peerGoaway_) {
        return Http2SubmitStatus::kClosed;
    }
    if (prefacePhase_ != PrefacePhase::kReady || (streamId != 0 && findStream(streamId) == nullptr)) {
        return Http2SubmitStatus::kInvalidState;
    }
    const auto bytes = encodeHttp2AlternativeServiceFrame(streamId, origin, value, peerSettings_.maxFrameSize(), resource_);
    if ((bytes.index() != 0)) {
        return Http2SubmitStatus::kInvalidMessage;
    }
    output_.appendBytes({std::get<0>(bytes).data(), std::get<0>(bytes).size()});
    return Http2SubmitStatus::kAccepted;
}
}  // namespace ruvia::detail
