#pragma once

#include <utility>
#include <variant>

#include "ruvia/http/Http2Types.h"
#include "ruvia/http/HttpResponseServer.h"
#include "ruvia/http/HttpResponseStream.h"

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
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const HttpBufferedResponseWritePlan* submitted() const&& = delete;
    [[nodiscard]] constexpr const Http2ResponseHeadSubmitFailure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const Http2ResponseHeadSubmitFailure* failure() const&& = delete;

private:
    friend class Http2Connection;
    friend class detail::Http2Connection;
    using Value = std::variant<HttpBufferedResponseWritePlan, Http2ResponseHeadSubmitFailure>;

    explicit Http2ResponseHeadSubmitResult(HttpBufferedResponseWritePlan plan)
        : value_(std::move(plan)) {}
    explicit Http2ResponseHeadSubmitResult(Http2ResponseHeadSubmitFailure failure)
        : value_(failure) {}

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
    [[nodiscard]] const http_response_stream_commit_plan* submitted() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const http_response_stream_commit_plan* submitted() const&& = delete;
    [[nodiscard]] constexpr const Http2ResponseHeadSubmitFailure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const Http2ResponseHeadSubmitFailure* failure() const&& = delete;

private:
    friend class Http2Connection;
    friend class detail::Http2Connection;
    using Value = std::variant<http_response_stream_commit_plan, Http2ResponseHeadSubmitFailure>;

    explicit Http2StreamingResponseHeadSubmitResult(http_response_stream_commit_plan plan)
        : value_(std::move(plan)) {}
    explicit Http2StreamingResponseHeadSubmitResult(Http2ResponseHeadSubmitFailure failure)
        : value_(failure) {}

    [[nodiscard]] static Http2StreamingResponseHeadSubmitResult makeSubmitted(
        http_response_stream_commit_plan plan) {
        return Http2StreamingResponseHeadSubmitResult(std::move(plan));
    }
    [[nodiscard]] static Http2StreamingResponseHeadSubmitResult makeFailure(
        Http2ResponseHeadSubmitError error) {
        return Http2StreamingResponseHeadSubmitResult(Http2ResponseHeadSubmitFailure(error));
    }

    Value value_;
};

}  // namespace ruvia
