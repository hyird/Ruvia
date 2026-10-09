#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "ruvia/core/task.h"

namespace ruvia::detail {

class request_body_loader final {
public:
    using read_all_type = task<std::string_view> (*)(void*);
    using discard_type = task<void> (*)(void*);

    constexpr request_body_loader(void* target, read_all_type read_all, discard_type discard) noexcept
        : target_(target),
          read_all_(read_all),
          discard_(discard) {}

    request_body_loader(const request_body_loader&) = delete;
    request_body_loader& operator=(const request_body_loader&) = delete;

    [[nodiscard]] task<std::string_view> read_all() {
        switch (state_) {
            case state_type::available:
                state_ = state_type::reading;
                break;
            case state_type::buffered:
                co_return buffered_body_;
            case state_type::reading:
            case state_type::discarding:
                throw std::logic_error("request body consumption is already in progress");
            case state_type::discarded:
                throw std::logic_error("request body was discarded");
            case state_type::failed:
                throw std::logic_error("request body consumption previously failed");
        }
        operation_guard_type operation(state_);
        buffered_body_ = co_await read_all_(target_);
        operation.commit(state_type::buffered);
        co_return buffered_body_;
    }

    task<void> discard() {
        switch (state_) {
            case state_type::available:
                state_ = state_type::discarding;
                break;
            case state_type::buffered:
            case state_type::discarded:
                co_return;
            case state_type::reading:
            case state_type::discarding:
                throw std::logic_error("request body consumption is already in progress");
            case state_type::failed:
                throw std::logic_error("request body consumption previously failed");
        }
        operation_guard_type operation(state_);
        co_await discard_(target_);
        operation.commit(state_type::discarded);
    }

private:
    enum class state_type : std::uint8_t {
        available,
        reading,
        buffered,
        discarding,
        discarded,
        failed,
    };

    class operation_guard_type final {
    public:
        explicit operation_guard_type(state_type& state_value) noexcept
            : state_(state_value) {}

        ~operation_guard_type() {
            if (!committed_) {
                state_ = state_type::failed;
            }
        }

        void commit(state_type state_value) noexcept {
            state_ = state_value;
            committed_ = true;
        }

    private:
        state_type& state_;
        bool committed_{false};
    };

    void* target_;
    read_all_type read_all_;
    discard_type discard_;
    std::string_view buffered_body_;
    state_type state_{state_type::available};
};

}  // namespace ruvia::detail
