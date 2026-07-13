#pragma once

#include "ruvia/http/detail/server/HttpResponseStreamHead.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ruvia {

class Context;

namespace detail {

class ResponseStreamState final {
public:
    [[nodiscard]] bool committed() const noexcept {
        return committed_.has_value();
    }

    [[nodiscard]] bool ended() const noexcept {
        return committed_.has_value() &&
            committed_->phase == Phase::kEnded;
    }

    [[nodiscard]] const ResponseStreamCommitPlan* commitPlan() const noexcept {
        return committed_.has_value() ? &committed_->plan : nullptr;
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

    void markCommitted(ResponseStreamCommitPlan plan) {
        if (committed_.has_value()) {
            throw std::logic_error("response stream is already committed");
        }
        Phase phase = Phase::kEnded;
        switch (plan.headDisposition()) {
            case ResponseStreamHeadDisposition::kBodyOpen:
                phase = Phase::kBodyOpen;
                break;
            case ResponseStreamHeadDisposition::kTrailersOnly:
                phase = Phase::kTrailersOnly;
                break;
            case ResponseStreamHeadDisposition::kMessageEnded:
                phase = Phase::kEnded;
                break;
        }
        committed_.emplace(std::move(plan), phase);
    }

    void markEnded() {
        if (!committed_.has_value()) {
            throw std::logic_error("response stream is not committed");
        }
        committed_->phase = Phase::kEnded;
    }

    void ensureBodyAllowed() const {
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        if (!committed_.has_value() ||
            committed_->phase != Phase::kBodyOpen) {
            throw std::logic_error("response does not allow a stream body");
        }
    }

    void ensureTrailersAllowed(ResponseStreamTrailerFraming requiredFraming) const {
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        if (!committed_.has_value() ||
            committed_->plan.trailerFraming() != requiredFraming) {
            throw std::logic_error("response framing does not support trailers");
        }
    }

private:
    enum class Phase : std::uint8_t {
        kBodyOpen,
        kTrailersOnly,
        kEnded
    };

    struct CommittedState final {
        CommittedState(
            ResponseStreamCommitPlan commitPlan,
            Phase committedPhase) noexcept
            : plan(std::move(commitPlan)),
              phase(committedPhase) {}

        ResponseStreamCommitPlan plan;
        Phase phase;
    };

    Context* context_{nullptr};
    StreamingHeadThunk streamingHead_{nullptr};
    std::optional<CommittedState> committed_;
};

}  // namespace detail
}  // namespace ruvia
