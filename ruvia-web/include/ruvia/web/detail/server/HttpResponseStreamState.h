#pragma once

#include "ruvia/http/detail/server/HttpResponseStreamHead.h"

#include <cstdint>
#include <stdexcept>

namespace ruvia {

class Context;

namespace detail {

class ResponseStreamState final {
public:
    [[nodiscard]] bool committed() const noexcept {
        return phase_ != Phase::kUncommitted;
    }

    [[nodiscard]] bool ended() const noexcept {
        return phase_ == Phase::kEnded;
    }

    using StreamingHeadThunk = HttpResponse (*)(Context&);

    void bindContext(Context* context, StreamingHeadThunk streamingHead) noexcept {
        context_ = context;
        streamingHead_ = streamingHead;
    }

    [[nodiscard]] HttpResponse streamingHead() const {
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        if (context_ == nullptr || streamingHead_ == nullptr) {
            throw std::logic_error("response stream context is not bound");
        }
        return streamingHead_(*context_);
    }

    void markCommitted(const ResponseStreamCommitPlan& plan) noexcept {
        trailerFraming_ = plan.trailerFraming();
        switch (plan.headDisposition()) {
            case ResponseStreamHeadDisposition::kBodyOpen:
                phase_ = Phase::kBodyOpen;
                break;
            case ResponseStreamHeadDisposition::kTrailersOnly:
                phase_ = Phase::kTrailersOnly;
                break;
            case ResponseStreamHeadDisposition::kMessageEnded:
                phase_ = Phase::kEnded;
                break;
        }
    }

    void markEnded() noexcept {
        phase_ = Phase::kEnded;
    }

    void ensureBodyAllowed() const {
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        if (phase_ != Phase::kBodyOpen) {
            throw std::logic_error("response does not allow a stream body");
        }
    }

    void ensureTrailersAllowed(ResponseStreamTrailerFraming requiredFraming) const {
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        if (phase_ == Phase::kUncommitted || trailerFraming_ != requiredFraming) {
            throw std::logic_error("response framing does not support trailers");
        }
    }

private:
    enum class Phase : std::uint8_t {
        kUncommitted,
        kBodyOpen,
        kTrailersOnly,
        kEnded
    };

    Context* context_{nullptr};
    StreamingHeadThunk streamingHead_{nullptr};
    ResponseStreamTrailerFraming trailerFraming_{
        ResponseStreamTrailerFraming::kUnavailable};
    Phase phase_{Phase::kUncommitted};
};

}  // namespace detail
}  // namespace ruvia
