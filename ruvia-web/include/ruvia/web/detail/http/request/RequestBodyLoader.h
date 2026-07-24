#pragma once

#include "ruvia/core/Task.h"

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace ruvia::detail {

class RequestBodyLoader final {
public:
    using ReadAll = Task<std::string_view> (*)(void*);
    using Discard = Task<void> (*)(void*);

    constexpr RequestBodyLoader(void* target, ReadAll readAll, Discard discard) noexcept
        : target_(target),
          readAll_(readAll),
          discard_(discard) {}

    RequestBodyLoader(const RequestBodyLoader&) = delete;
    RequestBodyLoader& operator=(const RequestBodyLoader&) = delete;

    [[nodiscard]] Task<std::string_view> readAll() {
        switch (state_) {
            case State::kAvailable:
                state_ = State::kReading;
                break;
            case State::kBuffered:
                co_return bufferedBody_;
            case State::kReading:
            case State::kDiscarding:
                throw std::logic_error("request body consumption is already in progress");
            case State::kDiscarded:
                throw std::logic_error("request body was discarded");
            case State::kFailed:
                throw std::logic_error("request body consumption previously failed");
        }
        OperationGuard operation(state_);
        bufferedBody_ = co_await readAll_(target_);
        operation.commit(State::kBuffered);
        co_return bufferedBody_;
    }

    Task<void> discard() {
        switch (state_) {
            case State::kAvailable:
                state_ = State::kDiscarding;
                break;
            case State::kBuffered:
            case State::kDiscarded:
                co_return;
            case State::kReading:
            case State::kDiscarding:
                throw std::logic_error("request body consumption is already in progress");
            case State::kFailed:
                throw std::logic_error("request body consumption previously failed");
        }
        OperationGuard operation(state_);
        co_await discard_(target_);
        operation.commit(State::kDiscarded);
    }

private:
    enum class State : std::uint8_t {
        kAvailable,
        kReading,
        kBuffered,
        kDiscarding,
        kDiscarded,
        kFailed,
    };

    class OperationGuard final {
    public:
        explicit OperationGuard(State& state) noexcept
            : state_(state) {}

        ~OperationGuard() {
            if (!committed_) {
                state_ = State::kFailed;
            }
        }

        void commit(State state) noexcept {
            state_ = state;
            committed_ = true;
        }

    private:
        State& state_;
        bool committed_{false};
    };

    void* target_;
    ReadAll readAll_;
    Discard discard_;
    std::string_view bufferedBody_;
    State state_{State::kAvailable};
};

}  // namespace ruvia::detail
