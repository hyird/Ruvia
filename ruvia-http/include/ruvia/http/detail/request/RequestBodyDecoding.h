#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/request/HttpRequestBodyFailure.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia::detail {

[[nodiscard]] inline HttpContentCodingFieldResult requestContentCoding(
    const HttpRequest& request) noexcept {
    return httpContentCodingFromHeaders(request.headers());
}

class HttpRequestContentDecodeProtocolFailure final {
public:
    [[nodiscard]] HttpProtocolError protocolError() const noexcept {
        switch (error_) {
            case HttpContentDecodeError::kUnsupportedCoding:
                return HttpProtocolError(http_status::kUnsupportedMediaType,
                    "request Content-Encoding is not supported");
            case HttpContentDecodeError::kInvalidContent:
                return HttpProtocolError(http_status::kBadRequest, "failed to decode request body");
            case HttpContentDecodeError::kDecodedSizeExceeded:
                return HttpRequestBodyFailure::tooLarge().protocolError();
            case HttpContentDecodeError::kDecoderFailure:
                std::terminate();
        }
        std::terminate();
    }

private:
    friend class HttpRequestContentDecodeResult;

    explicit constexpr HttpRequestContentDecodeProtocolFailure(
        HttpContentDecodeError error) noexcept
        : error_(error) {}

    HttpContentDecodeError error_;
};

class HttpRequestContentDecoderFailure final {
private:
    friend class HttpRequestContentDecodeResult;

    constexpr HttpRequestContentDecoderFailure() noexcept = default;
};

// Request decoding is role-specific: success, an HTTP protocol failure, and an
// internal decoder failure are mutually exclusive alternatives. Client
// response decoding keeps using the role-neutral HttpContentDecodeResult.
class HttpRequestContentDecodeResult final {
public:
    HttpRequestContentDecodeResult(const HttpRequestContentDecodeResult&) = delete;
    HttpRequestContentDecodeResult& operator=(const HttpRequestContentDecodeResult&) = delete;
    HttpRequestContentDecodeResult(HttpRequestContentDecodeResult&&) noexcept = default;
    HttpRequestContentDecodeResult& operator=(HttpRequestContentDecodeResult&&) = delete;

    [[nodiscard]] HttpDecodedContent* decoded() & noexcept {
        return std::get_if<HttpDecodedContent>(&value_);
    }
    [[nodiscard]] const HttpDecodedContent* decoded() const& noexcept {
        return std::get_if<HttpDecodedContent>(&value_);
    }
    HttpDecodedContent* decoded() && = delete;
    const HttpDecodedContent* decoded() const&& = delete;

    [[nodiscard]] const HttpRequestContentDecodeProtocolFailure* protocolFailure() const& noexcept {
        return std::get_if<HttpRequestContentDecodeProtocolFailure>(&value_);
    }
    const HttpRequestContentDecodeProtocolFailure* protocolFailure() const&& = delete;

    [[nodiscard]] const HttpRequestContentDecoderFailure* decoderFailure() const& noexcept {
        return std::get_if<HttpRequestContentDecoderFailure>(&value_);
    }
    const HttpRequestContentDecoderFailure* decoderFailure() const&& = delete;

private:
    friend HttpRequestContentDecodeResult decodeHttpRequestContent(
        HttpContentCoding, std::string_view, HttpContentDecodeOptions);

    using Value = std::variant<HttpDecodedContent, HttpRequestContentDecodeProtocolFailure,
        HttpRequestContentDecoderFailure>;

    explicit HttpRequestContentDecodeResult(HttpDecodedContent decoded) noexcept
        : value_(std::move(decoded)) {}

    explicit HttpRequestContentDecodeResult(HttpContentDecodeError error) noexcept
        : value_(HttpRequestContentDecodeProtocolFailure(error)) {}

    explicit constexpr HttpRequestContentDecodeResult(
        HttpRequestContentDecoderFailure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static constexpr HttpRequestContentDecodeResult makeDecoderFailure() noexcept {
        return HttpRequestContentDecodeResult(HttpRequestContentDecoderFailure());
    }

    Value value_;
};

[[nodiscard]] inline HttpRequestContentDecodeResult decodeHttpRequestContent(
    HttpContentCoding coding, std::string_view input, HttpContentDecodeOptions options) {
    auto result = decodeHttpContent(coding, input, options);
    if (auto* decoded = result.decoded()) {
        return HttpRequestContentDecodeResult(std::move(*decoded));
    }
    if (const auto* failure = result.failure()) {
        if (failure->error() == HttpContentDecodeError::kDecoderFailure) {
            return HttpRequestContentDecodeResult::makeDecoderFailure();
        }
        return HttpRequestContentDecodeResult(failure->error());
    }
    throw std::logic_error("unexpected HTTP content decode result");
}

}  // namespace ruvia::detail
