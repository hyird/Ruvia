#pragma once

#include <type_traits>
#include <variant>

#include "ruvia/web/error.h"

#include "ratelimit/rate_limiter.h"

namespace ruvia::detail {

class http1_closing_error final {
public:
    [[nodiscard]] constexpr const http_error_info& error() const noexcept {
        return error_;
    }

private:
    friend class http1_closing_rejection;

    explicit constexpr http1_closing_error(http_error_info error) noexcept
        : error_(error) {}

    http_error_info error_;
};

class http1_closing_rate_limit_rejection final {
public:
    [[nodiscard]] constexpr const http_error_info& error() const noexcept {
        return error_;
    }

    [[nodiscard]] constexpr const rate_limit_rejection& rejection() const noexcept {
        return rejection_;
    }

private:
    friend class http1_closing_rejection;

    constexpr http1_closing_rate_limit_rejection(
        http_error_info error, rate_limit_rejection rejection) noexcept
        : error_(error),
          rejection_(rejection) {}

    http_error_info error_;
    rate_limit_rejection rejection_;
};

class http1_closing_rejection final {
public:
    constexpr http1_closing_rejection() noexcept = default;

    [[nodiscard]] static constexpr http1_closing_rejection error(http_error_info error) noexcept {
        return http1_closing_rejection(http1_closing_error(error));
    }

    [[nodiscard]] static constexpr http1_closing_rejection get_rate_limit(
        http_error_info error, rate_limit_rejection rejection) noexcept {
        return http1_closing_rejection(http1_closing_rate_limit_rejection(error, rejection));
    }

    [[nodiscard]] constexpr const http_error_info* error() const& noexcept {
        if (const auto* closing = std::get_if<http1_closing_error>(&value_)) {
            return &closing->error();
        }
        if (const auto* rate_limit = std::get_if<http1_closing_rate_limit_rejection>(&value_)) {
            return &rate_limit->error();
        }
        return nullptr;
    }
    const http_error_info* error() const&& = delete;

    [[nodiscard]] constexpr const rate_limit_rejection* get_rate_limit() const& noexcept {
        const auto* rejection = std::get_if<http1_closing_rate_limit_rejection>(&value_);
        return rejection == nullptr ? nullptr : &rejection->rejection();
    }
    const rate_limit_rejection* get_rate_limit() const&& = delete;

private:
    explicit constexpr http1_closing_rejection(http1_closing_error error) noexcept
        : value_(error) {}

    explicit constexpr http1_closing_rejection(http1_closing_rate_limit_rejection rejection) noexcept
        : value_(rejection) {}

    std::variant<std::monostate, http1_closing_error, http1_closing_rate_limit_rejection> value_;
};

static_assert(std::is_trivially_copyable_v<http1_closing_rejection>);

}  // namespace ruvia::detail
