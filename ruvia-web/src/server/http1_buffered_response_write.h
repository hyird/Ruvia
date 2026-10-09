#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>
#include <type_traits>

#include "ruvia/http/http1_response_head_plan.h"

namespace ruvia::detail {

class http1_buffered_response_write_result;

class http1_buffered_response_write_completed final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class http1_buffered_response_write_result;

    explicit constexpr http1_buffered_response_write_completed(http_status_code status) noexcept
        : status_(status) {}

    http_status_code status_;
};

class http1_buffered_response_write_failed_before_commit final {
private:
    friend class http1_buffered_response_write_result;
    constexpr http1_buffered_response_write_failed_before_commit() noexcept = default;
};

class http1_buffered_response_write_failed_after_commit final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class http1_buffered_response_write_result;

    explicit constexpr http1_buffered_response_write_failed_after_commit(http_status_code status) noexcept
        : status_(status) {}

    http_status_code status_;
};

// The complete-head byte boundary produces exactly one terminal alternative.
// Only alternatives reached after a complete head own a committed status, so a
// failed-before-commit result cannot carry meaningless status storage.
class http1_buffered_response_write_result final {
public:
    [[nodiscard]] constexpr const http1_buffered_response_write_completed* completed() const& noexcept {
        return state_ == state_type::completed ? &value_.completed_ : nullptr;
    }
    const http1_buffered_response_write_completed* completed() const&& = delete;

    [[nodiscard]] constexpr const http1_buffered_response_write_failed_before_commit* failed_before_commit()
        const& noexcept {
        return state_ == state_type::failed_before_commit ? &value_.failed_before_commit_ : nullptr;
    }
    const http1_buffered_response_write_failed_before_commit* failed_before_commit() const&& = delete;

    [[nodiscard]] constexpr const http1_buffered_response_write_failed_after_commit* failed_after_commit()
        const& noexcept {
        return state_ == state_type::failed_after_commit ? &value_.failed_after_commit_ : nullptr;
    }
    const http1_buffered_response_write_failed_after_commit* failed_after_commit() const&& = delete;

    [[nodiscard]] constexpr std::optional<http_status_code> committed_status() const noexcept {
        if (const auto* value = completed()) {
            return value->status();
        }
        if (const auto* value = failed_after_commit()) {
            return value->status();
        }
        return std::nullopt;
    }

private:
    friend http1_buffered_response_write_result classify_http1_buffered_response_write(
        const http1_buffered_response_plan&, std::size_t, std::error_code, std::size_t) noexcept;

    enum class state_type : std::uint8_t { completed,
        failed_before_commit,
        failed_after_commit };

    union value {
        constexpr explicit value(http1_buffered_response_write_completed value) noexcept
            : completed_(value) {}
        constexpr explicit value(http1_buffered_response_write_failed_before_commit value) noexcept
            : failed_before_commit_(value) {}
        constexpr explicit value(http1_buffered_response_write_failed_after_commit value) noexcept
            : failed_after_commit_(value) {}

        http1_buffered_response_write_completed completed_;
        http1_buffered_response_write_failed_before_commit failed_before_commit_;
        http1_buffered_response_write_failed_after_commit failed_after_commit_;
    };

    [[nodiscard]] static constexpr http1_buffered_response_write_result make_completed(
        http_status_code status) noexcept {
        return http1_buffered_response_write_result(http1_buffered_response_write_completed(status));
    }

    [[nodiscard]] static constexpr http1_buffered_response_write_result
    make_failed_before_commit() noexcept {
        return http1_buffered_response_write_result(http1_buffered_response_write_failed_before_commit());
    }

    [[nodiscard]] static constexpr http1_buffered_response_write_result make_failed_after_commit(
        http_status_code status) noexcept {
        return http1_buffered_response_write_result(
            http1_buffered_response_write_failed_after_commit(status));
    }

    explicit constexpr http1_buffered_response_write_result(
        http1_buffered_response_write_completed value) noexcept
        : value_(value),
          state_(state_type::completed) {}

    explicit constexpr http1_buffered_response_write_result(
        http1_buffered_response_write_failed_before_commit value) noexcept
        : value_(value),
          state_(state_type::failed_before_commit) {}

    explicit constexpr http1_buffered_response_write_result(
        http1_buffered_response_write_failed_after_commit value) noexcept
        : value_(value),
          state_(state_type::failed_after_commit) {}

    value value_;
    state_type state_;
};

static_assert(std::is_trivially_copyable_v<http1_buffered_response_write_result>);
static_assert(sizeof(http1_buffered_response_write_result) <= 4);

// `bytes_transferred` is the composed async-write prefix accepted before `error`.
// Only a prefix containing the entire serialized head commits a final status.
[[nodiscard]] inline http1_buffered_response_write_result classify_http1_buffered_response_write(
    const http1_buffered_response_plan& plan, std::size_t response_head_bytes, std::error_code error,
    std::size_t bytes_transferred) noexcept {
    const auto status = plan.response_status();
    if (!error) {
        if (response_head_bytes == 0 || bytes_transferred < response_head_bytes) {
            return http1_buffered_response_write_result::make_failed_before_commit();
        }
        return http1_buffered_response_write_result::make_completed(status);
    }
    if (response_head_bytes != 0 && bytes_transferred >= response_head_bytes) {
        return http1_buffered_response_write_result::make_failed_after_commit(status);
    }
    return http1_buffered_response_write_result::make_failed_before_commit();
}

}  // namespace ruvia::detail
