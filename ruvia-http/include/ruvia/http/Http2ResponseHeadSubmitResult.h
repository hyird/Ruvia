#pragma once

#include <expected>
#include <utility>

#include "ruvia/http/Http2Types.h"
#include "ruvia/http/HttpResponseServer.h"

namespace ruvia {

class Http2Connection;
namespace detail {
class Http2Connection;
}  // namespace detail

class Http2ResponseHeadSubmitFailure final {
public:
    [[nodiscard]] constexpr Http2ResponseHeadSubmitError error() const noexcept {
        return error_;
    }

private:
    friend class Http2Connection;
    friend class detail::Http2Connection;
    friend class Http2ResponseHeadSubmitResult;
    friend class Http2StreamingResponseHeadSubmitResult;

    explicit constexpr Http2ResponseHeadSubmitFailure(Http2ResponseHeadSubmitError error) noexcept
        : error_(error) {}

    Http2ResponseHeadSubmitError error_;
};

class Http2ResponseHeadSubmitResult final {
public:
    [[nodiscard]] const HttpBufferedResponseWritePlan* submitted() const& noexcept {
        return value_ ? &*value_ : nullptr;
    }
    const HttpBufferedResponseWritePlan* submitted() const&& = delete;
    [[nodiscard]] constexpr const Http2ResponseHeadSubmitFailure* failure() const& noexcept {
        return value_ ? nullptr : &value_.error();
    }
    const Http2ResponseHeadSubmitFailure* failure() const&& = delete;

private:
    friend class Http2Connection;
    friend class detail::Http2Connection;
    using Value = std::expected<HttpBufferedResponseWritePlan, Http2ResponseHeadSubmitFailure>;

    explicit Http2ResponseHeadSubmitResult(HttpBufferedResponseWritePlan plan)
        : value_(std::move(plan)) {}
    explicit Http2ResponseHeadSubmitResult(Http2ResponseHeadSubmitFailure failure)
        : value_(std::unexpected(failure)) {}

    [[nodiscard]] static Http2ResponseHeadSubmitResult makeSubmitted(
        HttpBufferedResponseWritePlan plan) {
        return Http2ResponseHeadSubmitResult(std::move(plan));
    }
    [[nodiscard]] static Http2ResponseHeadSubmitResult makeFailure(
        Http2ResponseHeadSubmitError error) {
        return Http2ResponseHeadSubmitResult(Http2ResponseHeadSubmitFailure(error));
    }

    Value value_;
};

class Http2StreamingResponseHeadSubmitResult final {
public:
    [[nodiscard]] const ResponseStreamCommitPlan* submitted() const& noexcept {
        return value_ ? &*value_ : nullptr;
    }
    const ResponseStreamCommitPlan* submitted() const&& = delete;
    [[nodiscard]] constexpr const Http2ResponseHeadSubmitFailure* failure() const& noexcept {
        return value_ ? nullptr : &value_.error();
    }
    const Http2ResponseHeadSubmitFailure* failure() const&& = delete;

private:
    friend class Http2Connection;
    friend class detail::Http2Connection;
    using Value = std::expected<ResponseStreamCommitPlan, Http2ResponseHeadSubmitFailure>;

    explicit Http2StreamingResponseHeadSubmitResult(ResponseStreamCommitPlan plan)
        : value_(std::move(plan)) {}
    explicit Http2StreamingResponseHeadSubmitResult(Http2ResponseHeadSubmitFailure failure)
        : value_(std::unexpected(failure)) {}

    [[nodiscard]] static Http2StreamingResponseHeadSubmitResult makeSubmitted(
        ResponseStreamCommitPlan plan) {
        return Http2StreamingResponseHeadSubmitResult(std::move(plan));
    }
    [[nodiscard]] static Http2StreamingResponseHeadSubmitResult makeFailure(
        Http2ResponseHeadSubmitError error) {
        return Http2StreamingResponseHeadSubmitResult(Http2ResponseHeadSubmitFailure(error));
    }

    Value value_;
};

}  // namespace ruvia
