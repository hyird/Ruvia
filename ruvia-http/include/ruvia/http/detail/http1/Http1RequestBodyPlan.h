#pragma once

#include "ruvia/http/detail/HttpExpectations.h"
#include "ruvia/http/detail/HttpTransferCoding.h"

#include <cstddef>
#include <cstdint>

namespace ruvia::detail {

enum class Http1RequestBodyMode : std::uint8_t {
    kNone,
    kContentLength,
    kChunked
};

// Whether the runtime consumed the complete request message before attempting
// to reuse its transport. This is a protocol lifecycle fact, not a product
// policy bool: RFC 9112 section 9.3 forbids reuse with unread request content.
enum class Http1RequestBodyConsumption : std::uint8_t {
    kComplete,
    kIncomplete
};

// Immutable framing contract produced by the HTTP/1 parser and consumed by any
// runtime driver. Content-Length, chunked framing, transfer-coding decode order,
// and 100-continue eligibility cannot drift as independent caller arguments.
class Http1RequestBodyPlan final {
public:
    Http1RequestBodyPlan() = delete;

    [[nodiscard]] static Http1RequestBodyPlan none(
        HttpRequestExpectations expectations = {}) noexcept {
        return Http1RequestBodyPlan(
            Http1RequestBodyMode::kNone,
            0,
            {},
            expectations);
    }

    [[nodiscard]] static Http1RequestBodyPlan knownLength(
        std::size_t contentLength,
        HttpRequestExpectations expectations = {}) noexcept {
        return Http1RequestBodyPlan(
            Http1RequestBodyMode::kContentLength,
            contentLength,
            {},
            expectations);
    }

    [[nodiscard]] static Http1RequestBodyPlan chunked(
        HttpTransferCodings transferCodings = {},
        HttpRequestExpectations expectations = {}) noexcept {
        return Http1RequestBodyPlan(
            Http1RequestBodyMode::kChunked,
            0,
            transferCodings,
            expectations);
    }

    [[nodiscard]] Http1RequestBodyMode mode() const noexcept {
        return mode_;
    }

    [[nodiscard]] bool hasContentLength() const noexcept {
        return mode_ == Http1RequestBodyMode::kContentLength;
    }

    [[nodiscard]] std::size_t contentLength() const noexcept {
        return contentLength_;
    }

    [[nodiscard]] bool isChunked() const noexcept {
        return mode_ == Http1RequestBodyMode::kChunked;
    }

    // Chunked framing requires consuming the terminating zero chunk even when
    // the decoded content is empty.
    [[nodiscard]] bool requiresConsumption() const noexcept {
        return isChunked() || (hasContentLength() && contentLength_ != 0);
    }

    [[nodiscard]] const HttpTransferCodings& transferCodings() const noexcept {
        return transferCodings_;
    }

    [[nodiscard]] const HttpRequestExpectations& expectations() const noexcept {
        return expectations_;
    }

    [[nodiscard]] HttpServerExpectationAction expectationAction() const noexcept {
        return expectations_.serverAction(
            requiresConsumption()
                ? HttpRequestContentIndication::kWillFollow
                : HttpRequestContentIndication::kNone);
    }

private:
    Http1RequestBodyPlan(
        Http1RequestBodyMode mode,
        std::size_t contentLength,
        HttpTransferCodings transferCodings,
        HttpRequestExpectations expectations) noexcept
        : mode_(mode),
          contentLength_(contentLength),
          transferCodings_(transferCodings),
          expectations_(expectations) {}

    Http1RequestBodyMode mode_{Http1RequestBodyMode::kNone};
    std::size_t contentLength_{0};
    HttpTransferCodings transferCodings_;
    HttpRequestExpectations expectations_;
};

}  // namespace ruvia::detail
