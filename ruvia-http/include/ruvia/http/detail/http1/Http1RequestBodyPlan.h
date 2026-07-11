#pragma once

#include "ruvia/http/detail/HttpExpectations.h"
#include "ruvia/http/detail/HttpTransferCoding.h"

#include <cstddef>
#include <cstdint>
#include <variant>

namespace ruvia::detail {

class Http1ServerRequestParseState;
class Http1ServerRequestParser;

// Whether the runtime consumed the complete request message before attempting
// to reuse its transport. This is a protocol lifecycle fact, not a product
// policy bool: RFC 9112 section 9.3 forbids reuse with unread request content.
enum class Http1RequestBodyConsumption : std::uint8_t {
    kComplete,
    kIncomplete
};

class Http1RequestWithoutBody final {
private:
    friend class Http1RequestBodyPlan;
    constexpr Http1RequestWithoutBody() noexcept = default;
};

class Http1KnownLengthRequestBody final {
public:
    [[nodiscard]] constexpr std::size_t contentLength() const noexcept {
        return contentLength_;
    }

private:
    friend class Http1RequestBodyPlan;

    explicit constexpr Http1KnownLengthRequestBody(
        std::size_t contentLength) noexcept
        : contentLength_(contentLength) {}

    std::size_t contentLength_;
};

class Http1ChunkedRequestBody final {
public:
    [[nodiscard]] constexpr const HttpTransferCodings&
    transferCodings() const noexcept {
        return transferCodings_;
    }

private:
    friend class Http1RequestBodyPlan;

    explicit constexpr Http1ChunkedRequestBody(
        HttpTransferCodings transferCodings) noexcept
        : transferCodings_(transferCodings) {}

    HttpTransferCodings transferCodings_;
};

// Immutable framing contract produced only by the HTTP/1 parser and consumed
// by any runtime driver. Content-Length belongs only to the known-length
// alternative; transfer-coding order belongs only to the final-chunked
// alternative. A caller cannot synthesize a plan that bypasses wire validation.
class Http1RequestBodyPlan final {
public:
    [[nodiscard]] constexpr const Http1RequestWithoutBody*
    withoutBody() const noexcept {
        return std::get_if<Http1RequestWithoutBody>(&framing_);
    }

    [[nodiscard]] constexpr const Http1KnownLengthRequestBody*
    knownLength() const noexcept {
        return std::get_if<Http1KnownLengthRequestBody>(&framing_);
    }

    [[nodiscard]] constexpr const Http1ChunkedRequestBody*
    chunked() const noexcept {
        return std::get_if<Http1ChunkedRequestBody>(&framing_);
    }

    // Chunked framing requires consuming the terminating zero chunk even when
    // the decoded content is empty.
    [[nodiscard]] constexpr bool requiresConsumption() const noexcept {
        if (const auto* known = knownLength()) {
            return known->contentLength() != 0;
        }
        return chunked() != nullptr;
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
    friend class Http1ServerRequestParseState;
    friend class Http1ServerRequestParser;

    using Framing = std::variant<
        Http1RequestWithoutBody,
        Http1KnownLengthRequestBody,
        Http1ChunkedRequestBody>;

    [[nodiscard]] static Http1RequestBodyPlan makeWithoutBody(
        HttpRequestExpectations expectations = {}) noexcept {
        return Http1RequestBodyPlan(
            Framing(Http1RequestWithoutBody()),
            expectations);
    }

    [[nodiscard]] static Http1RequestBodyPlan makeKnownLength(
        std::size_t contentLength,
        HttpRequestExpectations expectations = {}) noexcept {
        return Http1RequestBodyPlan(
            Framing(Http1KnownLengthRequestBody(contentLength)),
            expectations);
    }

    [[nodiscard]] static Http1RequestBodyPlan makeChunked(
        HttpTransferCodings transferCodings,
        HttpRequestExpectations expectations = {}) noexcept {
        return Http1RequestBodyPlan(
            Framing(Http1ChunkedRequestBody(transferCodings)),
            expectations);
    }

    Http1RequestBodyPlan(
        Framing framing,
        HttpRequestExpectations expectations) noexcept
        : framing_(framing),
          expectations_(expectations) {}

    Framing framing_;
    HttpRequestExpectations expectations_;
};

}  // namespace ruvia::detail
