#pragma once

#include <stdexcept>
#include <variant>

#include "ruvia/http/http_response_stream.h"

namespace ruvia {

class context;

namespace detail {

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
    // message: a successful response that can deliver nothing more. Sinks
    // complete body writes, trailers and end() without sending anything,
    // report aborted(), and answer sleep() with stop_requested.
    [[nodiscard]] bool body_suppressed_complete() const noexcept {
        // An ended stream always holds its commit plan.
        return ended() && commit_plan()->body_plan().body_suppressed();
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

    void bind_context(context* context_value) {
        if (!std::holds_alternative<unbound_type>(state_)) {
            throw std::logic_error("response stream context is already bound");
        }
        if (context_value == nullptr) {
            throw std::invalid_argument("response stream context binding is incomplete");
        }
        state_.emplace<bound_type>(context_value);
    }

    void release_context() noexcept {
        if (std::holds_alternative<bound_type>(state_)) {
            state_.emplace<detached_type>();
        }
    }

    [[nodiscard]] context& bound_context() const {
        const auto* bound = std::get_if<bound_type>(&state_);
        if (bound == nullptr) {
            if (committed()) {
                throw std::logic_error("response stream is already committed");
            }
            throw std::logic_error("response stream context is not bound");
        }
        return *bound->context_;
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
        if (const auto* committed_plan = commit_plan()) {
            // A body-suppressed commit is already logically ended, but its head
            // can still fail during transport delivery. Preserve that commit.
            const auto plan = *committed_plan;
            state_.emplace<aborted_after_commit_type>(plan);
            return;
        }
        state_.emplace<aborted_before_commit_type>();
    }

    void ensure_body_allowed() const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        if (!std::holds_alternative<body_open_type>(state_)) {
            throw std::logic_error("response does not allow a stream body");
        }
    }

    // Trailers are optional metadata (RFC 9110 6.5): framing that cannot carry
    // them discards them, so only a status that can never carry content is a
    // handler error here. The caller writes trailers only if trailers_available().
    void ensure_trailers_allowed() const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        const auto* plan = commit_plan();
        if (plan == nullptr || !plan->status_permits_trailers()) {
            throw std::logic_error("response status does not permit trailers");
        }
    }

private:
    struct unbound_type final {};

    struct detached_type final {};

    struct bound_type final {
        explicit bound_type(context* bound_context) noexcept
            : context_(bound_context) {}

        context* context_;
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
