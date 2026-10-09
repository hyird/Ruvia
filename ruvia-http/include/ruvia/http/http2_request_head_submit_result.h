#pragma once

#include <cstdint>
#include <exception>
#include <variant>

#include "ruvia/http/http2_types.h"

namespace ruvia {

class http2_request_head_submit_result;
namespace detail {
class http2_connection;
}

// A committed client HEADERS transaction always owns a nonzero, odd stream ID.
// Failure cannot expose connection-control stream zero as a sentinel.
class http2_submitted_request_head final {
public:
    [[nodiscard]] constexpr std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

private:
    friend class http2_request_head_submit_result;
    explicit constexpr http2_submitted_request_head(std::uint32_t stream_id) noexcept
        : stream_id_(stream_id) {
        if (stream_id_ == 0 || stream_id_ > 0x7fffffffU || (stream_id_ & 1U) == 0) {
            std::terminate();
        }
    }
    std::uint32_t stream_id_;
};

class http2_request_head_submit_failure final {
public:
    [[nodiscard]] constexpr http2_request_head_submit_error error() const noexcept {
        return error_;
    }

private:
    friend class http2_request_head_submit_result;
    explicit constexpr http2_request_head_submit_failure(http2_request_head_submit_error error) noexcept
        : error_(error) {}
    http2_request_head_submit_error error_;
};

// The engine and public connection share this exclusive result contract. The
// public connection additionally pins a successful stream before returning it.
class http2_request_head_submit_result final {
public:
    [[nodiscard]] constexpr const http2_submitted_request_head* submitted() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const http2_submitted_request_head* submitted() const&& = delete;
    [[nodiscard]] constexpr const http2_request_head_submit_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const http2_request_head_submit_failure* failure() const&& = delete;

private:
    friend class detail::http2_connection;
    using value_type = std::variant<http2_submitted_request_head, http2_request_head_submit_failure>;
    explicit constexpr http2_request_head_submit_result(http2_submitted_request_head value) noexcept
        : value_(value) {}
    explicit constexpr http2_request_head_submit_result(http2_request_head_submit_failure value) noexcept
        : value_(value) {}
    [[nodiscard]] static constexpr http2_request_head_submit_result make_submitted(std::uint32_t stream_id) noexcept {
        return http2_request_head_submit_result(http2_submitted_request_head(stream_id));
    }
    [[nodiscard]] static constexpr http2_request_head_submit_result make_failure(http2_request_head_submit_error error) noexcept {
        return http2_request_head_submit_result(http2_request_head_submit_failure(error));
    }
    value_type value_;
};

}  // namespace ruvia
