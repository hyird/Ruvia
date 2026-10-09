#pragma once

#include <cstdint>
#include <exception>
#include <variant>

#include "ruvia/http/Http2Types.h"

namespace ruvia {

class Http2RequestHeadSubmitResult;
namespace detail {
class Http2Connection;
}

// A committed client HEADERS transaction always owns a nonzero, odd stream ID.
// Failure cannot expose connection-control stream zero as a sentinel.
class Http2SubmittedRequestHead final {
public:
    [[nodiscard]] constexpr std::uint32_t streamId() const noexcept {
        return streamId_;
    }

private:
    friend class Http2RequestHeadSubmitResult;
    explicit constexpr Http2SubmittedRequestHead(std::uint32_t streamId) noexcept
        : streamId_(streamId) {
        if (streamId_ == 0 || streamId_ > 0x7fffffffU || (streamId_ & 1U) == 0) {
            std::terminate();
        }
    }
    std::uint32_t streamId_;
};

class Http2RequestHeadSubmitFailure final {
public:
    [[nodiscard]] constexpr Http2RequestHeadSubmitError error() const noexcept {
        return error_;
    }

private:
    friend class Http2RequestHeadSubmitResult;
    explicit constexpr Http2RequestHeadSubmitFailure(Http2RequestHeadSubmitError error) noexcept
        : error_(error) {}
    Http2RequestHeadSubmitError error_;
};

// The engine and public connection share this exclusive result contract. The
// public connection additionally pins a successful stream before returning it.
class Http2RequestHeadSubmitResult final {
public:
    [[nodiscard]] constexpr const Http2SubmittedRequestHead* submitted() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const Http2SubmittedRequestHead* submitted() const&& = delete;
    [[nodiscard]] constexpr const Http2RequestHeadSubmitFailure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const Http2RequestHeadSubmitFailure* failure() const&& = delete;

private:
    friend class detail::Http2Connection;
    using Value = std::variant<Http2SubmittedRequestHead, Http2RequestHeadSubmitFailure>;
    explicit constexpr Http2RequestHeadSubmitResult(Http2SubmittedRequestHead value) noexcept
        : value_(value) {}
    explicit constexpr Http2RequestHeadSubmitResult(Http2RequestHeadSubmitFailure value) noexcept
        : value_(value) {}
    [[nodiscard]] static constexpr Http2RequestHeadSubmitResult makeSubmitted(std::uint32_t streamId) noexcept {
        return Http2RequestHeadSubmitResult(Http2SubmittedRequestHead(streamId));
    }
    [[nodiscard]] static constexpr Http2RequestHeadSubmitResult makeFailure(Http2RequestHeadSubmitError error) noexcept {
        return Http2RequestHeadSubmitResult(Http2RequestHeadSubmitFailure(error));
    }
    Value value_;
};

}  // namespace ruvia
