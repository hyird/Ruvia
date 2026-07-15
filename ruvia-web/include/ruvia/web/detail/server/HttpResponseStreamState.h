#pragma once

#include "ruvia/http/detail/server/HttpResponseStreamHead.h"

#include <stdexcept>
#include <utility>
#include <variant>

namespace ruvia {

class Context;

namespace detail {

class ResponseStreamState final {
public:
    [[nodiscard]] bool committed() const noexcept {
        return commitPlan() != nullptr;
    }

    [[nodiscard]] bool ended() const noexcept {
        return std::holds_alternative<Ended>(state_);
    }

    [[nodiscard]] const ResponseStreamCommitPlan*
    commitPlan() const & noexcept {
        if (const auto* value = std::get_if<BodyOpen>(&state_)) {
            return &value->plan;
        }
        if (const auto* value = std::get_if<TrailersOnly>(&state_)) {
            return &value->plan;
        }
        if (const auto* value = std::get_if<Ended>(&state_)) {
            return &value->plan;
        }
        return nullptr;
    }
    const ResponseStreamCommitPlan* commitPlan() const && = delete;

    using StreamingHeadThunk = HttpResponse (*)(Context&);

    void bindContext(Context* context, StreamingHeadThunk streamingHead) {
        if (!std::holds_alternative<Unbound>(state_)) {
            throw std::logic_error("response stream context is already bound");
        }
        if (context == nullptr || streamingHead == nullptr) {
            throw std::invalid_argument("response stream context binding is incomplete");
        }
        state_.emplace<Bound>(context, streamingHead);
    }

    void releaseContext() noexcept {
        if (std::holds_alternative<Bound>(state_)) {
            state_.emplace<Detached>();
        }
    }

    [[nodiscard]] HttpResponse streamingHead() const {
        const auto* bound = std::get_if<Bound>(&state_);
        if (bound == nullptr) {
            if (committed()) {
                throw std::logic_error("response stream is already committed");
            }
            throw std::logic_error("response stream context is not bound");
        }
        return bound->streamingHead(*bound->context);
    }

    void markCommitted(ResponseStreamCommitPlan plan) {
        if (committed()) {
            throw std::logic_error("response stream is already committed");
        }
        switch (plan.headDisposition()) {
            case ResponseStreamHeadDisposition::kBodyOpen:
                state_.emplace<BodyOpen>(std::move(plan));
                break;
            case ResponseStreamHeadDisposition::kTrailersOnly:
                state_.emplace<TrailersOnly>(std::move(plan));
                break;
            case ResponseStreamHeadDisposition::kMessageEnded:
                state_.emplace<Ended>(std::move(plan));
                break;
        }
    }

    void markEnded() {
        if (ended()) {
            return;
        }
        if (auto* value = std::get_if<BodyOpen>(&state_)) {
            auto plan = std::move(value->plan);
            state_.emplace<Ended>(std::move(plan));
            return;
        }
        if (auto* value = std::get_if<TrailersOnly>(&state_)) {
            auto plan = std::move(value->plan);
            state_.emplace<Ended>(std::move(plan));
            return;
        }
        throw std::logic_error("response stream is not committed");
    }

    void ensureBodyAllowed() const {
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        if (!std::holds_alternative<BodyOpen>(state_)) {
            throw std::logic_error("response does not allow a stream body");
        }
    }

    void ensureTrailersAllowed(ResponseStreamTrailerFraming requiredFraming) const {
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        const auto* plan = commitPlan();
        if (plan == nullptr || plan->trailerFraming() != requiredFraming) {
            throw std::logic_error("response framing does not support trailers");
        }
    }

private:
    struct Unbound final {};

    struct Detached final {};

    struct Bound final {
        Bound(Context* boundContext, StreamingHeadThunk head) noexcept
            : context(boundContext), streamingHead(head) {}

        Context* context;
        StreamingHeadThunk streamingHead;
    };

    struct BodyOpen final {
        explicit BodyOpen(ResponseStreamCommitPlan commitPlan) noexcept
            : plan(std::move(commitPlan)) {}

        ResponseStreamCommitPlan plan;
    };

    struct TrailersOnly final {
        explicit TrailersOnly(ResponseStreamCommitPlan commitPlan) noexcept
            : plan(std::move(commitPlan)) {}

        ResponseStreamCommitPlan plan;
    };

    struct Ended final {
        explicit Ended(ResponseStreamCommitPlan commitPlan) noexcept
            : plan(std::move(commitPlan)) {}

        ResponseStreamCommitPlan plan;
    };

    using State = std::variant<
        Unbound,
        Bound,
        Detached,
        BodyOpen,
        TrailersOnly,
        Ended>;

    State state_;
};

}  // namespace detail
}  // namespace ruvia
