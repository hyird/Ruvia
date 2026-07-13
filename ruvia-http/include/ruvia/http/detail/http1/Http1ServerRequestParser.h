#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/HeaderAcceptUtils.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/http1/Http1RequestBodyPlan.h"
#include "ruvia/http/detail/http1/Http1ServerConnectionPlan.h"
#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/http/HttpParseError.h"

namespace ruvia::detail {

struct Http1RequestParseResultAccess final {
    [[nodiscard]] static Http1RequestParseResult needMore(
        std::optional<std::size_t> requiredTotalBytes) noexcept {
        return Http1RequestParseResult(
            Http1RequestNeedMore(requiredTotalBytes));
    }

    [[nodiscard]] static Http1RequestParseResult parsed(
        HttpRequest request,
        Http1RequestBodyPlan bodyPlan,
        std::string_view wireBody,
        std::size_t consumedBytes) noexcept {
        return Http1RequestParseResult(Http1ParsedRequest(
            std::move(request), bodyPlan, wireBody, consumedBytes));
    }

    [[nodiscard]] static Http1RequestParseResult failure(
        HttpParseError error) noexcept {
        return Http1RequestParseResult(Http1RequestParseFailure(error));
    }
};

// The server runtime deliberately dispatches as soon as a valid request head is
// available, while the public sans-I/O parser scans the complete framed message.
// These phases make those two readiness boundaries impossible to confuse.
enum class Http1ServerRequestParsePhase : std::uint8_t {
    kNeedRequestHead,
    kRequestHeadReady,
    kNeedRequestBody,
    kRequestMessageReady,
    kFailure
};

class Http1ServerRequestParseState final {
public:
    [[nodiscard]] Http1ServerRequestParsePhase phase() const noexcept {
        return phase_;
    }

    [[nodiscard]] bool headReady() const noexcept {
        return phase_ == Http1ServerRequestParsePhase::kRequestHeadReady;
    }

    [[nodiscard]] bool messageReady() const noexcept {
        return phase_ == Http1ServerRequestParsePhase::kRequestMessageReady;
    }

    [[nodiscard]] const HttpParseError* failure() const noexcept {
        return phase_ == Http1ServerRequestParsePhase::kFailure && error_
            ? &*error_
            : nullptr;
    }

    HttpRequest request{HttpRequestAccess::make()};
    std::size_t headerBytes{0};
    // Valid only in kRequestMessageReady. A future fixed-length buffer target is
    // a separate fact and is present only in kNeedRequestBody.
    std::size_t messageBytes{0};
    std::optional<std::size_t> requiredTotalBytes;
    Http1RequestBodyPlan bodyPlan{
        Http1RequestBodyPlan(HttpRequestExpectations{})};
    Http1ServerConnectionPlan connectionPlan{
        Http1ServerConnectionPlan::http11Close()};
    HttpContentCoding responseCoding{HttpContentCoding::kNone};

private:
    friend class Http1ServerRequestParser;

    std::optional<HttpParseError> error_;
    Http1ServerRequestParsePhase phase_{
        Http1ServerRequestParsePhase::kNeedRequestHead};
};

class Http1ServerRequestParser final {
public:
    // Hot-path entry point: `state` is reset and reused across read attempts, so
    // parsing an incomplete head never copies or re-zeroes the ~2.5KB state.
    void parseHead(
        std::string_view buffer,
        Http1ServerRequestParseState& state,
        std::size_t headerSearchOffset = 0) const noexcept;

    // Whole-message scanner used by the public sans-I/O API. It always advances
    // beyond kRequestHeadReady to an unambiguous message/failure/need-more phase.
    [[nodiscard]] Http1ServerRequestParseState parseMessage(
        std::string_view buffer) const noexcept;

private:
    static void parseRequestHead(
        std::string_view buffer,
        std::size_t headerSearchOffset,
        Http1ServerRequestParseState& state) noexcept;
    static void parseMessageBody(
        std::string_view buffer,
        Http1ServerRequestParseState& state) noexcept;
};

}  // namespace ruvia::detail
