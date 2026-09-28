#include "ruvia/http/Http3DataWritePlan.h"

#include "ruvia/http/Http3VarInt.h"

namespace ruvia {

Http3DataWritePlan::Http3DataWritePlan(HttpResponseBodyPlan bodyPlan,
    std::optional<std::uint64_t> declaredContentLength) noexcept
    : responseBodyPlan_(bodyPlan),
      declaredContentLength_(declaredContentLength) {}

Http3DataWritePlan::Http3DataWritePlan(Http3ClientRequestBodyPlan bodyPlan) noexcept
    : declaredContentLength_(bodyPlan.expectedLength),
      requestBody_(true) {}

bool Http3DataWritePlan::bodyAllowed() const noexcept {
    return requestBody_ ||
           responseBodyPlan_->contentSemantics() == HttpResponseContentSemantics::kConnectTunnel ||
           (responseBodyPlan_->statusAllowsBody() && !responseBodyPlan_->bodySuppressed());
}

bool Http3DataWritePlan::finAllowed() const noexcept {
    return !finished_ && !writePending_ &&
           (requestBody_ ||
               !(responseBodyPlan_->contentSemantics() == HttpResponseContentSemantics::kConnectTunnel &&
                   declaredContentLength_)) &&
           (!bodyAllowed() || !declaredContentLength_ ||
               committedPayloadBytes_ == *declaredContentLength_);
}

std::uint64_t Http3DataWritePlan::committedPayloadBytes() const noexcept {
    return committedPayloadBytes_;
}

bool Http3DataWritePlan::finished() const noexcept {
    return finished_;
}

std::expected<Http3DataWritePlan::Chunk, Http3DataWriteError> Http3DataWritePlan::planChunk(
    std::span<const char> payload, bool finishing) noexcept {
    if (finished_) {
        return std::unexpected(Http3DataWriteError::kAlreadyFinished);
    }
    if (writePending_) {
        return std::unexpected(Http3DataWriteError::kWriteAlreadyPending);
    }
    if (!requestBody_ &&
        responseBodyPlan_->contentSemantics() == HttpResponseContentSemantics::kConnectTunnel &&
        declaredContentLength_) {
        return std::unexpected(Http3DataWriteError::kContentLengthForbidden);
    }
    if (!payload.empty() && !bodyAllowed()) {
        return std::unexpected(Http3DataWriteError::kBodyNotAllowed);
    }
    if (payload.size() > kHttp3VarIntMax) {
        return std::unexpected(Http3DataWriteError::kVarIntOutOfRange);
    }
    const auto bytes = static_cast<std::uint64_t>(payload.size());
    if (bytes > UINT64_MAX - committedPayloadBytes_) {
        return std::unexpected(Http3DataWriteError::kContentLengthOverflow);
    }
    const auto total = committedPayloadBytes_ + bytes;
    if (declaredContentLength_ && total > *declaredContentLength_) {
        return std::unexpected(Http3DataWriteError::kContentLengthMismatch);
    }
    if (finishing && declaredContentLength_ && total != *declaredContentLength_ &&
        (requestBody_ || bodyAllowed())) {
        return std::unexpected(Http3DataWriteError::kContentLengthMismatch);
    }

    Chunk chunk{.payload = payload,
        .emitsData = !payload.empty(),
        .finishing = finishing};
    if (chunk.emitsData) {
        const auto encoded = encodeHttp3FrameHeader(chunk.frameHeader,
            static_cast<std::uint64_t>(Http3FrameType::kData), bytes);
        if (!encoded) {
            return std::unexpected(encoded.error() == Http3CodecError::kValueOutOfRange
                                       ? Http3DataWriteError::kVarIntOutOfRange
                                       : Http3DataWriteError::kFrameHeaderEncoding);
        }
        chunk.frameHeaderSize = *encoded;
    }
    pendingPayloadBytes_ = bytes;
    pendingFinishing_ = finishing;
    writePending_ = true;
    return chunk;
}

std::expected<void, Http3DataWriteError> Http3DataWritePlan::commitPayload(
    std::uint64_t bytes, bool finishing) noexcept {
    if (finished_) {
        return std::unexpected(Http3DataWriteError::kAlreadyFinished);
    }
    if (!writePending_) {
        return std::unexpected(Http3DataWriteError::kNoWritePending);
    }
    if (bytes != pendingPayloadBytes_ || finishing != pendingFinishing_) {
        return std::unexpected(Http3DataWriteError::kCommitDoesNotMatchPlan);
    }
    // The planned total was checked before any bytes were committed.
    committedPayloadBytes_ += bytes;
    writePending_ = false;
    pendingPayloadBytes_ = 0;
    pendingFinishing_ = false;
    finished_ = finishing;
    return {};
}

}  // namespace ruvia
