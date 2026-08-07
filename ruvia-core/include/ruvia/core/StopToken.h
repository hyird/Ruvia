#pragma once

#include <exception>
#include <optional>
#include <stop_token>
#include <utility>

#include <ruvia/core/MoveOnlyFunction.h>

namespace ruvia {

namespace detail {

class StopCallback final {
public:
    explicit StopCallback(MoveOnlyFunction<void()> callback) noexcept
        : callback_(std::move(callback)) {}

    void operator()() noexcept {
        try {
            if (callback_) {
                callback_();
            }
        } catch (...) {
            std::terminate();
        }
    }

private:
    MoveOnlyFunction<void()> callback_;
};

class StopSource;

}  // namespace detail

class StopRegistration final {
public:
    StopRegistration() noexcept = default;
    ~StopRegistration() = default;

    StopRegistration(const StopRegistration&) = delete;
    StopRegistration& operator=(const StopRegistration&) = delete;
    StopRegistration(StopRegistration&&) = delete;
    StopRegistration& operator=(StopRegistration&&) = delete;

    void reset() noexcept {
        callback_.reset();
        registered_ = false;
    }

    [[nodiscard]] bool registered() const noexcept {
        return registered_;
    }

private:
    friend class StopToken;

    StopRegistration(std::stop_token token, MoveOnlyFunction<void()> callback) {
        if (!token.stop_possible() || !callback) {
            return;
        }
        if (token.stop_requested()) {
            detail::StopCallback(std::move(callback))();
            return;
        }
        callback_.emplace(std::move(token), detail::StopCallback(std::move(callback)));
        registered_ = true;
    }

    std::optional<std::stop_callback<detail::StopCallback>> callback_;
    bool registered_{false};
};

class StopToken final {
public:
    StopToken() noexcept = default;

    [[nodiscard]] bool stopRequested() const noexcept {
        return token_.stop_requested();
    }

    [[nodiscard]] bool stoppable() const noexcept {
        return token_.stop_possible();
    }

    // std::stop_callback embeds its registration node in StopRegistration, so
    // registering a cancellation callback performs no callback-state allocation.
    // requestStop() may invoke the callback on the requesting thread; callbacks
    // that affect worker-owned state must post only an id/generation to that
    // worker and let the owner validate it there.
    [[nodiscard]] StopRegistration registerCallback(MoveOnlyFunction<void()> callback) const {
        return StopRegistration(token_, std::move(callback));
    }

private:
    friend class detail::StopSource;

    explicit StopToken(std::stop_token token) noexcept
        : token_(std::move(token)) {}

    std::stop_token token_;
};

namespace detail {

class StopSource final {
public:
    StopSource() = default;
    StopSource(const StopSource&) = delete;
    StopSource& operator=(const StopSource&) = delete;
    StopSource(StopSource&&) = delete;
    StopSource& operator=(StopSource&&) = delete;

    void requestStop() noexcept {
        (void)source_.request_stop();
    }

    [[nodiscard]] bool stopRequested() const noexcept {
        return source_.stop_requested();
    }

    [[nodiscard]] StopToken token() const noexcept {
        return StopToken(source_.get_token());
    }

private:
    std::stop_source source_;
};

}  // namespace detail

}  // namespace ruvia
