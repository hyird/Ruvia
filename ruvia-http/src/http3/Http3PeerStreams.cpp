#include "ruvia/http/Http3PeerStreams.h"

#include <stdexcept>
#include <variant>

#include "ruvia/http/Http3VarInt.h"

namespace ruvia {

Http3PeerStreams::Http3PeerStreams(Http3PeerRole localRole, std::pmr::memory_resource* resource,
    Http3PeerStreamLimits limits)
    : localRole_(localRole),
      limits_(limits),
      streams_(resource) {
    if (resource == nullptr || limits_.maxActiveStreams == 0) {
        throw std::invalid_argument("HTTP/3 peer stream limits and resource must be valid");
    }
}

std::variant<std::monostate, Http3PeerStreamError> Http3PeerStreams::validatePeerUni(
    std::uint64_t streamId) const noexcept {
    if (!isHttp3PeerUnidirectionalStreamId(localRole_, streamId)) {
        return Http3PeerStreamError::kStreamCreationError;
    }
    return {};
}

bool Http3PeerStreams::isCritical(Http3PeerStreamKind kind) noexcept {
    return kind == Http3PeerStreamKind::kControl || kind == Http3PeerStreamKind::kQpackEncoder ||
           kind == Http3PeerStreamKind::kQpackDecoder;
}

std::variant<Http3PeerStreamFeed, Http3PeerStreamError> Http3PeerStreams::feed(
    std::uint64_t streamId, std::span<const char> bytes, bool fin, bool reset) {
    if (const auto valid = validatePeerUni(streamId); (valid.index() != 0)) {
        return std::get<1>(valid);
    }

    auto found = streams_.find(streamId);
    if (found == streams_.end()) {
        // A FIN/RESET before the stream header is explicitly tolerated.
        if ((fin || reset) && bytes.empty()) {
            return Http3PeerStreamFeed{.consumed = 0, .remaining = bytes, .fin = fin, .reset = reset, .closed = true};
        }
        if (streams_.size() >= limits_.maxActiveStreams) {
            return Http3PeerStreamError::kExcessiveLoad;
        }
        found = streams_.try_emplace(streamId).first;
    }

    auto& state = found->second;
    if (state.kind != Http3PeerStreamKind::kUnclassified) {
        if ((fin || reset) && isCritical(state.kind)) {
            return Http3PeerStreamError::kClosedCriticalStream;
        }
        const auto kind = state.kind;
        const auto streamType = state.streamType;
        if (fin || reset) {
            streams_.erase(found);
        }
        return Http3PeerStreamFeed{.kind = kind,
            .streamType = streamType,
            .consumed = 0,
            .remaining = bytes,
            .fin = fin,
            .reset = reset,
            .closed = fin || reset};
    }

    std::size_t consumed = 0;
    while (consumed < bytes.size() && state.typeSize < state.typeBytes.size()) {
        state.typeBytes[state.typeSize++] = bytes[consumed++];
        const auto decoded = decodeHttp3VarInt(
            std::span<const char>(state.typeBytes).first(state.typeSize));
        if ((decoded.index() != 0)) {
            continue;
        }

        state.streamType = std::get<0>(decoded).value;
        state.kind = std::get<0>(decoded).value == 0   ? Http3PeerStreamKind::kControl
                     : std::get<0>(decoded).value == 1 ? Http3PeerStreamKind::kPush
                     : std::get<0>(decoded).value == 2 ? Http3PeerStreamKind::kQpackEncoder
                     : std::get<0>(decoded).value == 3 ? Http3PeerStreamKind::kQpackDecoder
                                                       : Http3PeerStreamKind::kUnknown;
        if (localRole_ == Http3PeerRole::kServer && state.kind == Http3PeerStreamKind::kPush) {
            streams_.erase(found);
            return Http3PeerStreamError::kStreamCreationError;
        }

        bool* seen = state.kind == Http3PeerStreamKind::kControl        ? &controlSeen_
                     : state.kind == Http3PeerStreamKind::kQpackEncoder ? &encoderSeen_
                     : state.kind == Http3PeerStreamKind::kQpackDecoder ? &decoderSeen_
                                                                        : nullptr;
        if (seen != nullptr) {
            if (*seen) {
                streams_.erase(found);
                return Http3PeerStreamError::kStreamCreationError;
            }
            *seen = true;
        }
        break;
    }

    if (state.kind == Http3PeerStreamKind::kUnclassified) {
        if (fin || reset) {
            streams_.erase(found);
            return Http3PeerStreamFeed{.consumed = consumed, .remaining = bytes.subspan(consumed), .fin = fin, .reset = reset, .closed = true};
        }
        return Http3PeerStreamFeed{.consumed = consumed, .remaining = bytes.subspan(consumed)};
    }

    if ((fin || reset) && isCritical(state.kind)) {
        return Http3PeerStreamError::kClosedCriticalStream;
    }
    const auto kind = state.kind;
    const auto streamType = state.streamType;
    if (fin || reset) {
        streams_.erase(found);
    }
    return Http3PeerStreamFeed{.kind = kind,
        .streamType = streamType,
        .consumed = consumed,
        .remaining = bytes.subspan(consumed),
        .fin = fin,
        .reset = reset,
        .closed = fin || reset};
}

std::variant<std::monostate, Http3PeerStreamError> Http3PeerStreams::acceptBidirectional(
    Http3PeerRole localRole, std::uint64_t streamId) noexcept {
    if (!isHttp3PeerBidirectionalStreamId(localRole, streamId)) {
        return Http3PeerStreamError::kStreamCreationError;
    }
    return {};
}

bool Http3PeerStreams::retireNonCriticalStream(std::uint64_t streamId) noexcept {
    const auto found = streams_.find(streamId);
    if (found == streams_.end() || isCritical(found->second.kind)) {
        return false;
    }
    streams_.erase(found);
    return true;
}

std::size_t Http3PeerStreams::activeStreamCount() const noexcept {
    return streams_.size();
}

}  // namespace ruvia
