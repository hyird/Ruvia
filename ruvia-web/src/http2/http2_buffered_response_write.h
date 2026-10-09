#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"

#include "http2/http2_data_output_budget.h"

namespace ruvia {
class http_response;
class http_buffered_response_write_plan;
class worker_memory;
}  // namespace ruvia

namespace ruvia::detail {
class http2_sans_io_stream_runtime_table;

class http2_buffered_response_write_completed final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class http2_buffered_response_write_result;

    explicit constexpr http2_buffered_response_write_completed(http_status_code status) noexcept
        : status_(status) {}

    http_status_code status_;
};

class http2_buffered_response_write_peer_aborted_before_commit final {
private:
    friend class http2_buffered_response_write_result;
    constexpr http2_buffered_response_write_peer_aborted_before_commit() noexcept = default;
};

class http2_buffered_response_write_peer_aborted_after_commit final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class http2_buffered_response_write_result;

    explicit constexpr http2_buffered_response_write_peer_aborted_after_commit(
        http_status_code status) noexcept
        : status_(status) {}

    http_status_code status_;
};

class http2_buffered_response_write_failed_before_commit final {
private:
    friend class http2_buffered_response_write_result;
    constexpr http2_buffered_response_write_failed_before_commit() noexcept = default;
};

class http2_buffered_response_write_failed_after_commit final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class http2_buffered_response_write_result;

    explicit constexpr http2_buffered_response_write_failed_after_commit(http_status_code status) noexcept
        : status_(status) {}

    http_status_code status_;
};

// The writer completes transport recovery (including RESET_STREAM) before
// returning. Each terminal alternative preserves both the response commit
// boundary and whether termination came from the peer or the local write path.
class http2_buffered_response_write_result final {
public:
    [[nodiscard]] static constexpr http2_buffered_response_write_result make_completed(
        http_status_code status) noexcept {
        return http2_buffered_response_write_result(http2_buffered_response_write_completed(status));
    }

    [[nodiscard]] static constexpr http2_buffered_response_write_result
    make_peer_aborted_before_commit() noexcept {
        return http2_buffered_response_write_result(
            http2_buffered_response_write_peer_aborted_before_commit{});
    }

    [[nodiscard]] static constexpr http2_buffered_response_write_result make_peer_aborted_after_commit(
        http_status_code status) noexcept {
        return http2_buffered_response_write_result(
            http2_buffered_response_write_peer_aborted_after_commit(status));
    }

    [[nodiscard]] static constexpr http2_buffered_response_write_result
    make_failed_before_commit() noexcept {
        return http2_buffered_response_write_result(http2_buffered_response_write_failed_before_commit{});
    }

    [[nodiscard]] static constexpr http2_buffered_response_write_result make_failed_after_commit(
        http_status_code status) noexcept {
        return http2_buffered_response_write_result(
            http2_buffered_response_write_failed_after_commit(status));
    }

    [[nodiscard]] constexpr const http2_buffered_response_write_completed* completed() const& noexcept {
        return state_ == state_type::completed ? &value_.completed_ : nullptr;
    }
    const http2_buffered_response_write_completed* completed() const&& = delete;

    [[nodiscard]] constexpr const http2_buffered_response_write_peer_aborted_before_commit*
    peer_aborted_before_commit() const& noexcept {
        return state_ == state_type::peer_aborted_before_commit ? &value_.peer_aborted_before_commit_
                                                                : nullptr;
    }
    const http2_buffered_response_write_peer_aborted_before_commit* peer_aborted_before_commit() const&& =
        delete;

    [[nodiscard]] constexpr const http2_buffered_response_write_peer_aborted_after_commit*
    peer_aborted_after_commit() const& noexcept {
        return state_ == state_type::peer_aborted_after_commit ? &value_.peer_aborted_after_commit_ : nullptr;
    }
    const http2_buffered_response_write_peer_aborted_after_commit* peer_aborted_after_commit() const&& =
        delete;

    [[nodiscard]] constexpr const http2_buffered_response_write_failed_before_commit* failed_before_commit()
        const& noexcept {
        return state_ == state_type::failed_before_commit ? &value_.failed_before_commit_ : nullptr;
    }
    const http2_buffered_response_write_failed_before_commit* failed_before_commit() const&& = delete;

    [[nodiscard]] constexpr const http2_buffered_response_write_failed_after_commit* failed_after_commit()
        const& noexcept {
        return state_ == state_type::failed_after_commit ? &value_.failed_after_commit_ : nullptr;
    }
    const http2_buffered_response_write_failed_after_commit* failed_after_commit() const&& = delete;

    [[nodiscard]] constexpr std::optional<http_status_code> committed_status() const noexcept {
        if (const auto* value = completed()) {
            return value->status();
        }
        if (const auto* value = peer_aborted_after_commit()) {
            return value->status();
        }
        if (const auto* value = failed_after_commit()) {
            return value->status();
        }
        return std::nullopt;
    }

private:
    enum class state_type : std::uint8_t {
        completed,
        peer_aborted_before_commit,
        peer_aborted_after_commit,
        failed_before_commit,
        failed_after_commit
    };

    union value {
        constexpr explicit value(http2_buffered_response_write_completed value) noexcept
            : completed_(value) {}
        constexpr explicit value(http2_buffered_response_write_peer_aborted_before_commit value) noexcept
            : peer_aborted_before_commit_(value) {}
        constexpr explicit value(http2_buffered_response_write_peer_aborted_after_commit value) noexcept
            : peer_aborted_after_commit_(value) {}
        constexpr explicit value(http2_buffered_response_write_failed_before_commit value) noexcept
            : failed_before_commit_(value) {}
        constexpr explicit value(http2_buffered_response_write_failed_after_commit value) noexcept
            : failed_after_commit_(value) {}

        http2_buffered_response_write_completed completed_;
        http2_buffered_response_write_peer_aborted_before_commit peer_aborted_before_commit_;
        http2_buffered_response_write_peer_aborted_after_commit peer_aborted_after_commit_;
        http2_buffered_response_write_failed_before_commit failed_before_commit_;
        http2_buffered_response_write_failed_after_commit failed_after_commit_;
    };

    explicit constexpr http2_buffered_response_write_result(
        http2_buffered_response_write_completed value) noexcept
        : value_(value),
          state_(state_type::completed) {}
    explicit constexpr http2_buffered_response_write_result(
        http2_buffered_response_write_peer_aborted_before_commit value) noexcept
        : value_(value),
          state_(state_type::peer_aborted_before_commit) {}
    explicit constexpr http2_buffered_response_write_result(
        http2_buffered_response_write_peer_aborted_after_commit value) noexcept
        : value_(value),
          state_(state_type::peer_aborted_after_commit) {}
    explicit constexpr http2_buffered_response_write_result(
        http2_buffered_response_write_failed_before_commit value) noexcept
        : value_(value),
          state_(state_type::failed_before_commit) {}
    explicit constexpr http2_buffered_response_write_result(
        http2_buffered_response_write_failed_after_commit value) noexcept
        : value_(value),
          state_(state_type::failed_after_commit) {}

    value value_;
    state_type state_;
};

static_assert(std::is_trivially_copyable_v<http2_buffered_response_write_result>);
static_assert(sizeof(http2_buffered_response_write_result) <= 4);

// Non-transport HTTP/2 buffered/file response driver. The session template owns
// socket reads/writes and event dispatch; this object owns the single response
// commit, DATA backpressure, file I/O, and typed terminal result chain. Keeping it
// non-template compiles that policy once for plain and TLS sessions.
class http2_buffered_response_writer final {
public:
    http2_buffered_response_writer(ruvia::http2_connection& connection,
        http2_sans_io_stream_runtime_table& stream_runtimes, worker_memory& worker_value,
        worker_signal& write_signal, http2_data_output_budget& output_budget) noexcept;
    http2_buffered_response_writer(ruvia::http2_connection& connection,
        http2_sans_io_stream_runtime_table& stream_runtimes, worker_memory& worker_value,
        worker_signal& write_signal) noexcept;

    http2_buffered_response_writer(const http2_buffered_response_writer&) = delete;
    http2_buffered_response_writer& operator=(const http2_buffered_response_writer&) = delete;
    http2_buffered_response_writer(http2_buffered_response_writer&&) = delete;
    http2_buffered_response_writer& operator=(http2_buffered_response_writer&&) = delete;

    [[nodiscard]] task<http2_buffered_response_write_result> write(std::uint32_t stream_id,
        const http_response& response, http_buffered_response_write_plan write_plan);

private:
    enum class data_write_result_type : std::uint8_t { completed,
        peer_aborted,
        failed };

    [[nodiscard]] task<data_write_result_type> write_data(
        std::uint32_t stream_id, std::string_view chunk, http2_end_stream end_stream);
    void wake_writer() noexcept;

    ruvia::http2_connection& connection_;
    http2_sans_io_stream_runtime_table& stream_runtimes_;
    worker_memory& worker_;
    worker_signal& write_signal_;
    http2_data_output_budget* output_budget_{nullptr};
};

}  // namespace ruvia::detail
