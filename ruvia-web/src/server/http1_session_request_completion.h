#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http1_request_connection_plan.h"
#include "ruvia/http/http_status.h"

namespace ruvia::detail {

class http1_session_request_completion;

// The connection is closing, so no bytes from this request can be reused.
class http1_request_buffer_discarded final {
private:
    friend class http1_session_request_completion;

    constexpr http1_request_buffer_discarded() noexcept = default;
};

// The session still owns an unshifted read buffer and must remove exactly this
// request prefix before parsing the next pipelined request.
class http1_request_buffer_compaction final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http1_session_request_completion;

    explicit constexpr http1_request_buffer_compaction(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}

    std::size_t consumed_bytes_;
};

// A request-body runtime handed its pipelined suffix over instead of writing it
// into the connection read buffer, whose bytes still back every view in the
// request being completed. The session installs these bytes at its single
// buffer-cleanup point, once the response is written and the access log is
// recorded. The view borrows request-scoped storage that outlives the
// completion.
class http1_request_buffer_pipeline_restore final {
public:
    [[nodiscard]] constexpr std::string_view pipeline() const noexcept {
        return pipeline_;
    }

private:
    friend class http1_session_request_completion;

    explicit constexpr http1_request_buffer_pipeline_restore(std::string_view pipeline) noexcept
        : pipeline_(pipeline) {}

    std::string_view pipeline_;
};

class http1_request_buffer_completion final {
public:
    [[nodiscard]] constexpr const http1_request_buffer_discarded* discarded() const& noexcept {
        return std::get_if<http1_request_buffer_discarded>(&value_);
    }
    [[nodiscard]] constexpr const http1_request_buffer_discarded* discarded() const&& = delete;

    [[nodiscard]] constexpr const http1_request_buffer_compaction* compaction() const& noexcept {
        return std::get_if<http1_request_buffer_compaction>(&value_);
    }
    [[nodiscard]] constexpr const http1_request_buffer_compaction* compaction() const&& = delete;

    [[nodiscard]] constexpr const http1_request_buffer_pipeline_restore* pipeline_restore()
        const& noexcept {
        return std::get_if<http1_request_buffer_pipeline_restore>(&value_);
    }
    [[nodiscard]] constexpr const http1_request_buffer_pipeline_restore* pipeline_restore() const&& =
        delete;

private:
    friend class http1_session_request_completion;

    using value_type = std::variant<http1_request_buffer_discarded, http1_request_buffer_compaction,
        http1_request_buffer_pipeline_restore>;

    template <typename alternative_type>
    explicit constexpr http1_request_buffer_completion(alternative_type alternative) noexcept
        : value_(std::move(alternative)) {}

    value_type value_;
};

// A buffered response still needs the session's scatter-gather writer.
class http1_buffered_response_ready final {
private:
    friend class http1_session_request_completion;

    constexpr http1_buffered_response_ready() noexcept = default;
};

// A response-stream route already committed its final head. Only this
// alternative exposes the exact wire status used by access logging.
class http1_committed_stream_response final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class http1_session_request_completion;

    explicit constexpr http1_committed_stream_response(http_status_code status) noexcept
        : status_(status) {}

    http_status_code status_;
};

// One terminal result for a dispatched HTTP/1 request. Wire ownership,
// connection reuse, and read-buffer cleanup are committed together so the
// session never reconstructs them from an optional status plus scalar flags.
class http1_session_request_completion final {
public:
    [[nodiscard]] static http1_session_request_completion make_buffered_closing(
        http1_request_connection_plan connection_plan) noexcept {
        if (connection_plan.disposition() != http1_close_policy::close_after_response) {
            std::terminate();
        }
        return http1_session_request_completion(http1_buffered_response_ready{}, connection_plan,
            http1_request_buffer_completion(http1_request_buffer_discarded{}));
    }

    [[nodiscard]] static http1_session_request_completion make_buffered_unrestored(
        http1_request_connection_plan connection_plan, std::size_t consumed_bytes) noexcept {
        return http1_session_request_completion(http1_buffered_response_ready{}, connection_plan,
            unshifted_buffer_completion(connection_plan, consumed_bytes));
    }

    [[nodiscard]] static http1_session_request_completion make_buffered_pipeline_restore(
        http1_request_connection_plan connection_plan, borrowed_text pipeline) noexcept {
        if (connection_plan.disposition() != http1_close_policy::allow_reuse) {
            std::terminate();
        }
        return http1_session_request_completion(http1_buffered_response_ready{}, connection_plan,
            http1_request_buffer_completion(http1_request_buffer_pipeline_restore(pipeline.view())));
    }

    [[nodiscard]] static http1_session_request_completion make_committed_stream(
        http1_request_connection_plan connection_plan, http_status_code status,
        std::size_t consumed_bytes) noexcept {
        return http1_session_request_completion(http1_committed_stream_response(status), connection_plan,
            unshifted_buffer_completion(connection_plan, consumed_bytes));
    }

    [[nodiscard]] constexpr const http1_buffered_response_ready* buffered_response() const& noexcept {
        return std::get_if<http1_buffered_response_ready>(&value_);
    }
    [[nodiscard]] constexpr const http1_buffered_response_ready* buffered_response() const&& = delete;

    [[nodiscard]] constexpr const http1_committed_stream_response* committed_stream() const& noexcept {
        return std::get_if<http1_committed_stream_response>(&value_);
    }
    [[nodiscard]] constexpr const http1_committed_stream_response* committed_stream() const&& = delete;

    [[nodiscard]] constexpr http1_request_connection_plan connection_plan() const noexcept {
        return connection_plan_;
    }

    [[nodiscard]] constexpr const http1_request_buffer_completion& buffer_completion() const& noexcept {
        return buffer_completion_;
    }
    [[nodiscard]] constexpr const http1_request_buffer_completion& buffer_completion() const&& = delete;

    // A buffered response can be replaced by a pre-commit policy error after
    // representation preparation (for example a forbidden identity fallback).
    // Rebind the final response plan without losing the exact read-buffer
    // cleanup alternative already established by request-body dispatch.
    [[nodiscard]] http1_session_request_completion with_buffered_connection_plan(
        http1_request_connection_plan connection_plan) const& noexcept {
        if (std::get_if<http1_committed_stream_response>(&value_) != nullptr) {
            std::terminate();
        }
        if (buffer_completion_.discarded() != nullptr) {
            return make_buffered_closing(connection_plan.require_close());
        }
        if (const auto* compaction = buffer_completion_.compaction()) {
            return make_buffered_unrestored(connection_plan, compaction->consumed_bytes());
        }
        if (const auto* pipeline = buffer_completion_.pipeline_restore()) {
            if (connection_plan.disposition() == http1_close_policy::close_after_response) {
                return make_buffered_closing(connection_plan);
            }
            return make_buffered_pipeline_restore(connection_plan, pipeline->pipeline());
        }
        std::terminate();
    }

private:
    using value_type = std::variant<http1_buffered_response_ready, http1_committed_stream_response>;

    [[nodiscard]] static http1_request_buffer_completion unshifted_buffer_completion(
        http1_request_connection_plan connection_plan, std::size_t consumed_bytes) noexcept {
        if (connection_plan.disposition() == http1_close_policy::close_after_response) {
            return http1_request_buffer_completion(http1_request_buffer_discarded{});
        }
        return http1_request_buffer_completion(http1_request_buffer_compaction(consumed_bytes));
    }

    template <typename alternative_type>
    http1_session_request_completion(alternative_type alternative, http1_request_connection_plan connection_plan,
        http1_request_buffer_completion buffer_completion) noexcept
        : value_(std::move(alternative)),
          connection_plan_(connection_plan),
          buffer_completion_(std::move(buffer_completion)) {}

    value_type value_;
    http1_request_connection_plan connection_plan_;
    http1_request_buffer_completion buffer_completion_;
};

}  // namespace ruvia::detail
