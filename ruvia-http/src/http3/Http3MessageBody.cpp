#include "ruvia/http/Http3MessageBody.h"

#include <limits>

namespace ruvia {

Http3MessageBody::Http3MessageBody(std::optional<std::uint64_t> contentLength, bool payloadAllowed) noexcept
    : contentLength_(contentLength),
      payloadAllowed_(payloadAllowed) {}

Http3MessageBodyResult Http3MessageBody::feed(std::uint64_t dataLength, bool fin) noexcept {
    if (state_ == State::kComplete) {
        return Http3MessageBodyResult::kAlreadyComplete;
    }
    if (state_ == State::kFailed) {
        return Http3MessageBodyResult::kAlreadyFailed;
    }

    const auto fail = [this](Http3MessageBodyResult result) {
        state_ = State::kFailed;
        return result;
    };

    if (!payloadAllowed_ && dataLength != 0) {
        return fail(Http3MessageBodyResult::kPayloadNotAllowed);
    }
    if (dataLength > std::numeric_limits<std::uint64_t>::max() - receivedLength_) {
        return fail(Http3MessageBodyResult::kLengthOverflow);
    }

    const auto totalLength = receivedLength_ + dataLength;
    if (contentLength_ && totalLength > *contentLength_) {
        return fail(Http3MessageBodyResult::kContentLengthExceeded);
    }

    receivedLength_ = totalLength;
    if (!fin) {
        return Http3MessageBodyResult::kAccepted;
    }
    if (payloadAllowed_ && contentLength_ && receivedLength_ != *contentLength_) {
        return fail(Http3MessageBodyResult::kContentLengthMismatch);
    }

    state_ = State::kComplete;
    return Http3MessageBodyResult::kComplete;
}

Http3MessageBody::State Http3MessageBody::state() const noexcept {
    return state_;
}

std::uint64_t Http3MessageBody::receivedLength() const noexcept {
    return receivedLength_;
}

}  // namespace ruvia
