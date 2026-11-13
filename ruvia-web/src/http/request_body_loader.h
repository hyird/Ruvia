#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/task.h"
#include "ruvia/web/streaming.h"

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

namespace ruvia::detail {

template <typename reader_type>
[[nodiscard]] task<std::optional<std::span<const std::byte>>> body_reader_read_thunk(void* target, operation_options operation) {
    return static_cast<reader_type*>(target)->read(std::move(operation));
}

template <typename reader_type>
[[nodiscard]] body_reader make_body_reader_facade(reader_type& reader_value) noexcept {
    return streaming_access::make_body_reader(&reader_value, &body_reader_read_thunk<reader_type>);
}

template <typename reader_type>
class body_reader_binding final {
public:
    template <typename... args_type>
    explicit body_reader_binding(args_type&&... args)
        : reader_(std::forward<args_type>(args)...),
          facade_(make_body_reader_facade(reader_)) {}

    body_reader_binding(const body_reader_binding&) = delete;
    body_reader_binding& operator=(const body_reader_binding&) = delete;
    body_reader_binding(body_reader_binding&&) = delete;
    body_reader_binding& operator=(body_reader_binding&&) = delete;

    [[nodiscard]] reader_type& reader() noexcept {
        return reader_;
    }
    [[nodiscard]] body_reader& facade() noexcept {
        return facade_;
    }

private:
    reader_type reader_;
    body_reader facade_;
};

template <typename loader_type>
[[nodiscard]] task<std::string_view> request_body_loader_read_all_thunk(void* target, operation_options operation) {
    return static_cast<loader_type*>(target)->read_all(std::move(operation));
}

template <typename loader_type>
task<void> request_body_loader_discard_thunk(void* target, operation_options operation) {
    return static_cast<loader_type*>(target)->discard(std::move(operation));
}

template <typename loader_type>
[[nodiscard]] request_body_loader make_request_body_loader_facade(loader_type& loader) noexcept {
    return request_body_loader(
        &loader, &request_body_loader_read_all_thunk<loader_type>, &request_body_loader_discard_thunk<loader_type>);
}

template <typename loader_type>
class request_body_loader_binding final {
public:
    template <typename... args_type>
    explicit request_body_loader_binding(args_type&&... args)
        : loader_(std::forward<args_type>(args)...),
          facade_(make_request_body_loader_facade(loader_)) {}

    request_body_loader_binding(const request_body_loader_binding&) = delete;
    request_body_loader_binding& operator=(const request_body_loader_binding&) = delete;
    request_body_loader_binding(request_body_loader_binding&&) = delete;
    request_body_loader_binding& operator=(request_body_loader_binding&&) = delete;

    [[nodiscard]] loader_type& loader() noexcept {
        return loader_;
    }
    [[nodiscard]] const loader_type& loader() const noexcept {
        return loader_;
    }
    [[nodiscard]] request_body_loader& facade() noexcept {
        return facade_;
    }

private:
    loader_type loader_;
    request_body_loader facade_;
};

}  // namespace ruvia::detail
