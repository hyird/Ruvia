#pragma once

#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/task.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"

namespace ruvia {

class context;

namespace detail {

// Raised by a body write on a stream whose committed head already completed
// the message because the request method/response status suppresses content
// (an explicit HEAD streaming route, a 304, ...). This is a
// control signal, not an error: the response head on the wire is complete and
// correct. It deterministically stops the handler -- including an infinite
// SSE loop -- at its first body write; dispatch recognizes the type and
// finishes the stream as a normal head-only success.
class response_stream_head_only_complete final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "response stream completed head-only; the body is suppressed";
    }
};

class response_stream_state final {
public:
    [[nodiscard]] bool committed() const noexcept {
        return commit_plan() != nullptr;
    }

    [[nodiscard]] bool ended() const noexcept {
        return std::holds_alternative<ended_type>(state_);
    }

    [[nodiscard]] bool aborted() const noexcept {
        return std::holds_alternative<aborted_before_commit_type>(state_) ||
               std::holds_alternative<aborted_after_commit_type>(state_);
    }

    // True once a body-suppressed head (HEAD / 304 semantics) has completed the
    // message: the next body write is the one ensure_body_allowed() answers with
    // response_stream_head_only_complete. Sinks check this to suspend once before
    // that synchronous throw, so a handler that catches the control signal and
    // keeps writing yields the worker thread each pass instead of hard-spinning
    // the event loop -- other connections on the worker keep being served.
    [[nodiscard]] bool body_suppressed_complete() const noexcept {
        if (!ended()) {
            return false;
        }
        const auto* plan = commit_plan();
        return plan != nullptr && plan->body_plan().body_suppressed();
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept {
        if (const auto* value = std::get_if<body_open_type>(&state_)) {
            return &value->plan_;
        }
        if (const auto* value = std::get_if<trailers_only_type>(&state_)) {
            return &value->plan_;
        }
        if (const auto* value = std::get_if<ended_type>(&state_)) {
            return &value->plan_;
        }
        if (const auto* value = std::get_if<aborted_after_commit_type>(&state_)) {
            return &value->plan_;
        }
        return nullptr;
    }
    const http_response_stream_commit_plan* commit_plan() const&& = delete;

    using streaming_head_thunk_type = task<http_response> (*)(context&);

    void bind_context(context* context_value, streaming_head_thunk_type streaming_head) {
        if (!std::holds_alternative<unbound_type>(state_)) {
            throw std::logic_error("response stream context is already bound");
        }
        if (context_value == nullptr || streaming_head == nullptr) {
            throw std::invalid_argument("response stream context binding is incomplete");
        }
        state_.emplace<bound_type>(context_value, streaming_head);
    }

    void release_context() noexcept {
        if (std::holds_alternative<bound_type>(state_)) {
            state_.emplace<detached_type>();
        }
    }

    [[nodiscard]] task<http_response> streaming_head() const {
        const auto* bound = std::get_if<bound_type>(&state_);
        if (bound == nullptr) {
            if (committed()) {
                throw std::logic_error("response stream is already committed");
            }
            throw std::logic_error("response stream context is not bound");
        }
        return bound->streaming_head_(*bound->context_);
    }

    void mark_committed(http_response_stream_commit_plan plan) {
        if (committed() || aborted()) {
            throw std::logic_error("response stream is already committed");
        }
        switch (plan.head_disposition()) {
            case http_response_stream_head_disposition::body_open:
                state_.emplace<body_open_type>(plan);
                break;
            case http_response_stream_head_disposition::trailers_only:
                state_.emplace<trailers_only_type>(plan);
                break;
            case http_response_stream_head_disposition::message_ended:
                state_.emplace<ended_type>(plan);
                break;
        }
    }

    void mark_ended() {
        if (ended()) {
            return;
        }
        if (auto* value = std::get_if<body_open_type>(&state_)) {
            auto plan = value->plan_;
            state_.emplace<ended_type>(plan);
            return;
        }
        if (auto* value = std::get_if<trailers_only_type>(&state_)) {
            auto plan = value->plan_;
            state_.emplace<ended_type>(plan);
            return;
        }
        throw std::logic_error("response stream is not committed");
    }

    void mark_aborted() noexcept {
        if (aborted()) {
            return;
        }
        if (auto* value = std::get_if<body_open_type>(&state_)) {
            auto plan = value->plan_;
            state_.emplace<aborted_after_commit_type>(plan);
            return;
        }
        if (auto* value = std::get_if<trailers_only_type>(&state_)) {
            auto plan = value->plan_;
            state_.emplace<aborted_after_commit_type>(plan);
            return;
        }
        if (std::holds_alternative<ended_type>(state_)) {
            return;
        }
        state_.emplace<aborted_before_commit_type>();
    }

    void ensure_body_allowed() const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
        if (ended()) {
            // A body-suppressed commit (HEAD/304 semantics) lands in Ended
            // directly, so the handler's first write arrives here. Writing
            // the body a GET would have is correct handler behavior, not a
            // sequencing bug -- signal head-only completion instead.
            const auto* plan = commit_plan();
            if (plan != nullptr && plan->body_plan().body_suppressed()) {
                throw response_stream_head_only_complete();
            }
            throw std::logic_error("response stream is already ended");
        }
        if (!std::holds_alternative<body_open_type>(state_)) {
            throw std::logic_error("response does not allow a stream body");
        }
    }

    void ensure_trailers_allowed(http_response_stream_trailer_framing required_framing) const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        const auto* plan = commit_plan();
        if (plan == nullptr || plan->trailer_framing() != required_framing) {
            throw std::logic_error("response framing does not support trailers");
        }
    }

private:
    struct unbound_type final {};

    struct detached_type final {};

    struct bound_type final {
        bound_type(context* bound_context, streaming_head_thunk_type head) noexcept
            : context_(bound_context),
              streaming_head_(head) {}

        context* context_;
        streaming_head_thunk_type streaming_head_;
    };

    struct body_open_type final {
        explicit body_open_type(http_response_stream_commit_plan commit_plan) noexcept
            : plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    struct trailers_only_type final {
        explicit trailers_only_type(http_response_stream_commit_plan commit_plan) noexcept
            : plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    struct ended_type final {
        explicit ended_type(http_response_stream_commit_plan commit_plan) noexcept
            : plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    struct aborted_before_commit_type final {};

    struct aborted_after_commit_type final {
        explicit aborted_after_commit_type(http_response_stream_commit_plan commit_plan) noexcept
            : plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    using state_type = std::variant<unbound_type, bound_type, detached_type, body_open_type, trailers_only_type, ended_type,
        aborted_before_commit_type, aborted_after_commit_type>;

    state_type state_;
};

}  // namespace detail
}  // namespace ruvia
