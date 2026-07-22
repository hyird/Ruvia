#pragma once

#include <type_traits>
#include <variant>

#include "ruvia/web/Error.h"
#include "ruvia/web/detail/ratelimit/RateLimiter.h"

namespace ruvia::detail {

class Http1ClosingError final {
public:
    [[nodiscard]] constexpr const HttpErrorInfo& error() const noexcept {
        return error_;
    }

private:
    friend class Http1ClosingRejection;

    explicit constexpr Http1ClosingError(HttpErrorInfo error) noexcept
        : error_(error) {}

    HttpErrorInfo error_;
};

class Http1ClosingRateLimitRejection final {
public:
    [[nodiscard]] constexpr const HttpErrorInfo& error() const noexcept {
        return error_;
    }

    [[nodiscard]] constexpr const RateLimitRejection& rejection() const noexcept {
        return rejection_;
    }

private:
    friend class Http1ClosingRejection;

    constexpr Http1ClosingRateLimitRejection(
        HttpErrorInfo error,
        RateLimitRejection rejection) noexcept
        : error_(error), rejection_(rejection) {}

    HttpErrorInfo error_;
    RateLimitRejection rejection_;
};

class Http1ClosingRejection final {
public:
    constexpr Http1ClosingRejection() noexcept = default;

    [[nodiscard]] static constexpr Http1ClosingRejection error(
        HttpErrorInfo error) noexcept {
        return Http1ClosingRejection(Http1ClosingError(error));
    }

    [[nodiscard]] static constexpr Http1ClosingRejection rateLimit(
        HttpErrorInfo error,
        RateLimitRejection rejection) noexcept {
        return Http1ClosingRejection(
            Http1ClosingRateLimitRejection(error, rejection));
    }

    [[nodiscard]] constexpr const HttpErrorInfo* error() const & noexcept {
        if (const auto* closing = std::get_if<Http1ClosingError>(&value_)) {
            return &closing->error();
        }
        if (const auto* rateLimit =
                std::get_if<Http1ClosingRateLimitRejection>(&value_)) {
            return &rateLimit->error();
        }
        return nullptr;
    }
    const HttpErrorInfo* error() const && = delete;

    [[nodiscard]] constexpr const RateLimitRejection* rateLimit() const & noexcept {
        const auto* rejection =
            std::get_if<Http1ClosingRateLimitRejection>(&value_);
        return rejection == nullptr ? nullptr : &rejection->rejection();
    }
    const RateLimitRejection* rateLimit() const && = delete;

private:
    explicit constexpr Http1ClosingRejection(Http1ClosingError error) noexcept
        : value_(error) {}

    explicit constexpr Http1ClosingRejection(
        Http1ClosingRateLimitRejection rejection) noexcept
        : value_(rejection) {}

    std::variant<
        std::monostate,
        Http1ClosingError,
        Http1ClosingRateLimitRejection> value_;
};

static_assert(std::is_trivially_copyable_v<Http1ClosingRejection>);

}  // namespace ruvia::detail
