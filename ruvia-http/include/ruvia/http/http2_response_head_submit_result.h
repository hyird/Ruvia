#pragma once

#include <utility>
#include <variant>

#include "ruvia/http/http2_types.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"

namespace ruvia {

class http2_connection;
namespace detail {
class http2_connection;
}  // namespace detail

class http2_response_head_submit_failure final {
public:
    [[nodiscard]] constexpr http2_response_head_submit_error error() const noexcept {
        return error_;
    }

private:
    friend class http2_connection;
    friend class detail::http2_connection;
    friend class http2_response_head_submit_result;
    friend class http2_streaming_response_head_submit_result;

    explicit constexpr http2_response_head_submit_failure(http2_response_head_submit_error error) noexcept
        : error_(error) {}

    http2_response_head_submit_error error_;
};

class http2_response_head_submit_result final {
public:
    [[nodiscard]] const http_buffered_response_write_plan* submitted() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const http_buffered_response_write_plan* submitted() const&& = delete;
    [[nodiscard]] constexpr const http2_response_head_submit_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const http2_response_head_submit_failure* failure() const&& = delete;

private:
    friend class http2_connection;
    friend class detail::http2_connection;
    using value_type = std::variant<http_buffered_response_write_plan, http2_response_head_submit_failure>;

    explicit http2_response_head_submit_result(http_buffered_response_write_plan plan)
        : value_(std::move(plan)) {}
    explicit http2_response_head_submit_result(http2_response_head_submit_failure failure)
        : value_(failure) {}

    [[nodiscard]] static http2_response_head_submit_result make_submitted(
        http_buffered_response_write_plan plan) {
        return http2_response_head_submit_result(std::move(plan));
    }
    [[nodiscard]] static http2_response_head_submit_result make_failure(
        http2_response_head_submit_error error) {
        return http2_response_head_submit_result(http2_response_head_submit_failure(error));
    }

    value_type value_;
};

class http2_streaming_response_head_submit_result final {
public:
    [[nodiscard]] const http_response_stream_commit_plan* submitted() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const http_response_stream_commit_plan* submitted() const&& = delete;
    [[nodiscard]] constexpr const http2_response_head_submit_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const http2_response_head_submit_failure* failure() const&& = delete;

private:
    friend class http2_connection;
    friend class detail::http2_connection;
    using value_type = std::variant<http_response_stream_commit_plan, http2_response_head_submit_failure>;

    explicit http2_streaming_response_head_submit_result(http_response_stream_commit_plan plan)
        : value_(std::move(plan)) {}
    explicit http2_streaming_response_head_submit_result(http2_response_head_submit_failure failure)
        : value_(failure) {}

    [[nodiscard]] static http2_streaming_response_head_submit_result make_submitted(
        http_response_stream_commit_plan plan) {
        return http2_streaming_response_head_submit_result(std::move(plan));
    }
    [[nodiscard]] static http2_streaming_response_head_submit_result make_failure(
        http2_response_head_submit_error error) {
        return http2_streaming_response_head_submit_result(http2_response_head_submit_failure(error));
    }

    value_type value_;
};

}  // namespace ruvia
