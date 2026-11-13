#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/task.h"

#include "integration/operation_interrupt.h"

namespace ruvia::detail {

class request_body_loader final {
public:
    // `operation` bounds the wait for body bytes; an interrupted call keeps what
    // arrived so far and a later call resumes it.
    using read_all_type = task<std::string_view> (*)(void*, operation_options);
    using discard_type = task<void> (*)(void*, operation_options);

    constexpr request_body_loader(void* target, read_all_type read_all, discard_type discard) noexcept
        : target_(target),
          read_all_(read_all),
          discard_(discard) {}

    request_body_loader(const request_body_loader&) = delete;
    request_body_loader& operator=(const request_body_loader&) = delete;

    [[nodiscard]] task<std::string_view> read_all(operation_options operation) {
        switch (state_) {
            case state_type::available:
                state_ = state_type::consuming;
                break;
            case state_type::buffered:
                co_return buffered_body_;
            case state_type::consuming:
                throw std::logic_error("request body consumption is already in progress");
            case state_type::discarded:
                throw std::logic_error("request body was discarded");
            case state_type::failed:
                throw std::logic_error("request body consumption previously failed");
        }
        const operation_steps steps(operation);
        try {
            buffered_body_ = co_await read_all_(target_, std::move(operation));
        } catch (...) {
            settle_failure(steps);
            throw;
        }
        state_ = state_type::buffered;
        co_return buffered_body_;
    }

    task<void> discard(operation_options operation) {
        switch (state_) {
            case state_type::available:
                state_ = state_type::consuming;
                break;
            case state_type::buffered:
            case state_type::discarded:
                co_return;
            case state_type::consuming:
                throw std::logic_error("request body consumption is already in progress");
            case state_type::failed:
                throw std::logic_error("request body consumption previously failed");
        }
        const operation_steps steps(operation);
        try {
            co_await discard_(target_, std::move(operation));
        } catch (...) {
            settle_failure(steps);
            throw;
        }
        state_ = state_type::discarded;
    }

private:
    enum class state_type : std::uint8_t {
        available,
        consuming,
        buffered,
        discarded,
        failed,
    };

    // Called while handling a consumption failure: an interruption by the
    // operation's own bound returns the body to `available`, so a later call
    // resumes it; any other failure is final.
    void settle_failure(const operation_steps& steps) noexcept {
        state_ = state_type::failed;
        try {
            throw;
        } catch (const std::system_error& error) {
            if (steps.interrupted(error)) {
                state_ = state_type::available;
            }
        } catch (...) {
        }
    }

    void* target_;
    read_all_type read_all_;
    discard_type discard_;
    std::string_view buffered_body_;
    state_type state_{state_type::available};
};

}  // namespace ruvia::detail
