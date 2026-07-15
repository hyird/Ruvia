#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/detail/HttpContentCoding.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia::detail {

[[nodiscard]] inline HttpContentCodingFieldResult requestContentCoding(
    const HttpRequest& request) noexcept {
    return httpContentCodingFromHeaders(request.headers());
}

class HttpRequestContentDecodeFailure final {
public:
    [[nodiscard]] std::optional<HttpProtocolError>
    protocolError() const noexcept {
        switch (error_) {
            case HttpContentDecodeError::kUnsupportedCoding:
                return HttpProtocolError(
                    415, "request Content-Encoding is not supported");
            case HttpContentDecodeError::kInvalidContent:
                return HttpProtocolError(
                    400, "failed to decode request body");
            case HttpContentDecodeError::kDecodedSizeExceeded:
                return HttpProtocolError(
                    413, "request body is too large");
            case HttpContentDecodeError::kDecoderFailure:
                return std::nullopt;
        }
        return std::nullopt;
    }

private:
    friend class HttpRequestContentDecodeResult;

    explicit constexpr HttpRequestContentDecodeFailure(
        HttpContentDecodeError error) noexcept
        : error_(error) {}

    HttpContentDecodeError error_;
};

// Request decoding is role-specific: success owns the decoded representation;
// wire, coding, and body-limit failures expose an HTTP response status, while
// an unavailable decoder remains an internal runtime failure. Client response
// decoding keeps using the role-neutral HttpContentDecodeResult.
class HttpRequestContentDecodeResult final {
public:
    HttpRequestContentDecodeResult(
        const HttpRequestContentDecodeResult&) = delete;
    HttpRequestContentDecodeResult& operator=(
        const HttpRequestContentDecodeResult&) = delete;
    HttpRequestContentDecodeResult(
        HttpRequestContentDecodeResult&&) noexcept = default;
    HttpRequestContentDecodeResult& operator=(
        HttpRequestContentDecodeResult&&) = delete;

    [[nodiscard]] HttpDecodedContent* decoded() & noexcept {
        return std::get_if<HttpDecodedContent>(&value_);
    }
    [[nodiscard]] const HttpDecodedContent* decoded() const & noexcept {
        return std::get_if<HttpDecodedContent>(&value_);
    }
    HttpDecodedContent* decoded() && = delete;
    const HttpDecodedContent* decoded() const && = delete;

    [[nodiscard]] const HttpRequestContentDecodeFailure*
    failure() const & noexcept {
        return std::get_if<HttpRequestContentDecodeFailure>(&value_);
    }
    const HttpRequestContentDecodeFailure* failure() const && = delete;

private:
    friend HttpRequestContentDecodeResult decodeHttpRequestContent(
        HttpContentCoding,
        std::string_view,
        std::size_t,
        std::pmr::memory_resource*);

    using Value = std::variant<
        HttpDecodedContent,
        HttpRequestContentDecodeFailure>;

    explicit HttpRequestContentDecodeResult(
        HttpDecodedContent decoded) noexcept
        : value_(std::move(decoded)) {}

    explicit HttpRequestContentDecodeResult(
        HttpContentDecodeError error) noexcept
        : value_(HttpRequestContentDecodeFailure(error)) {}

    Value value_;
};

[[nodiscard]] inline HttpRequestContentDecodeResult decodeHttpRequestContent(
    HttpContentCoding coding,
    std::string_view input,
    std::size_t maxDecodedBytes,
    std::pmr::memory_resource* resource) {
    auto result = decodeHttpContent(
        coding, input, maxDecodedBytes, resource);
    if (auto* decoded = result.decoded()) {
        return HttpRequestContentDecodeResult(std::move(*decoded));
    }
    if (const auto* failure = result.failure()) {
        return HttpRequestContentDecodeResult(failure->error());
    }
    return HttpRequestContentDecodeResult(
        HttpContentDecodeError::kDecoderFailure);
}

}  // namespace ruvia::detail
