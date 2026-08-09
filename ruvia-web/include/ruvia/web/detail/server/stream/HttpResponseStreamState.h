#pragma once

#include "ruvia/http/detail/server/HttpResponseStreamHead.h"

#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>

namespace ruvia {

class Context;

namespace detail {

// Raised by a body write on a stream whose committed head already completed
// the message because the request method/response status suppresses content
// (an explicit HEAD streaming route, a 304, ...). This is a
// control signal, not an error: the response head on the wire is complete and
// correct. It deterministically stops the handler -- including an infinite
// SSE loop -- at its first body write; dispatch recognizes the type and
// finishes the stream as a normal head-only success.
class ResponseStreamHeadOnlyComplete final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "response stream completed head-only; the body is suppressed";
    }
};

class ResponseStreamState final {
public:
    [[nodiscard]] bool committed() const noexcept {
        return commitPlan() != nullptr;
    }

    [[nodiscard]] bool ended() const noexcept {
        return std::holds_alternative<Ended>(state_);
    }

    [[nodiscard]] bool aborted() const noexcept {
        return std::holds_alternative<AbortedBeforeCommit>(state_) || std::holds_alternative<AbortedAfterCommit>(state_);
    }

    // True once a body-suppressed head (HEAD / 304 semantics) has completed the
    // message: the next body write is the one ensureBodyAllowed() answers with
    // ResponseStreamHeadOnlyComplete. Sinks check this to suspend once before
    // that synchronous throw, so a handler that catches the control signal and
    // keeps writing yields the worker thread each pass instead of hard-spinning
    // the event loop -- other connections on the worker keep being served.
    [[nodiscard]] bool bodySuppressedComplete() const noexcept {
        if (!ended()) {
            return false;
        }
        const auto* plan = commitPlan();
        return plan != nullptr && plan->bodyPlan().bodySuppressed();
    }

    [[nodiscard]] const ResponseStreamCommitPlan* commitPlan() const& noexcept {
        if (const auto* value = std::get_if<BodyOpen>(&state_)) {
            return &value->plan;
        }
        if (const auto* value = std::get_if<TrailersOnly>(&state_)) {
            return &value->plan;
        }
        if (const auto* value = std::get_if<Ended>(&state_)) {
            return &value->plan;
        }
        if (const auto* value = std::get_if<AbortedAfterCommit>(&state_)) {
            return &value->plan;
        }
        return nullptr;
    }
    const ResponseStreamCommitPlan* commitPlan() const&& = delete;

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
        if (committed() || aborted()) {
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

    void markAborted() noexcept {
        if (aborted()) {
            return;
        }
        if (auto* value = std::get_if<BodyOpen>(&state_)) {
            auto plan = std::move(value->plan);
            state_.emplace<AbortedAfterCommit>(std::move(plan));
            return;
        }
        if (auto* value = std::get_if<TrailersOnly>(&state_)) {
            auto plan = std::move(value->plan);
            state_.emplace<AbortedAfterCommit>(std::move(plan));
            return;
        }
        if (std::holds_alternative<Ended>(state_)) {
            return;
        }
        state_.emplace<AbortedBeforeCommit>();
    }

    void ensureBodyAllowed() const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
        if (ended()) {
            // A body-suppressed commit (HEAD/304 semantics) lands in Ended
            // directly, so the handler's first write arrives here. Writing
            // the body a GET would have is correct handler behavior, not a
            // sequencing bug -- signal head-only completion instead.
            const auto* plan = commitPlan();
            if (plan != nullptr && plan->bodyPlan().bodySuppressed()) {
                throw ResponseStreamHeadOnlyComplete();
            }
            throw std::logic_error("response stream is already ended");
        }
        if (!std::holds_alternative<BodyOpen>(state_)) {
            throw std::logic_error("response does not allow a stream body");
        }
    }

    void ensureTrailersAllowed(ResponseStreamTrailerFraming requiredFraming) const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
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
            : context(boundContext),
              streamingHead(head) {}

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

    struct AbortedBeforeCommit final {};

    struct AbortedAfterCommit final {
        explicit AbortedAfterCommit(ResponseStreamCommitPlan commitPlan) noexcept
            : plan(std::move(commitPlan)) {}

        ResponseStreamCommitPlan plan;
    };

    using State = std::variant<Unbound, Bound, Detached, BodyOpen, TrailersOnly, Ended, AbortedBeforeCommit, AbortedAfterCommit>;

    State state_;
};

}  // namespace detail
}  // namespace ruvia
