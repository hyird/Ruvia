#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/Http1RequestBodyPlan.h"
#include "ruvia/http/HttpParseError.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia {

namespace detail {

struct Http1RequestParseResultAccess;

}  // namespace detail

// The parser cannot determine an exact final size while the header section or
// chunked body is incomplete. A Content-Length body does provide that exact
// total, allowing an I/O owner to reserve once without overloading a
// "consumed" field with two unrelated meanings.
class Http1RequestNeedMore final {
public:
    [[nodiscard]] constexpr std::optional<std::size_t> requiredTotalBytes() const noexcept {
        return requiredTotalBytes_;
    }

private:
    friend struct detail::Http1RequestParseResultAccess;

    constexpr Http1RequestNeedMore() noexcept = default;

    explicit constexpr Http1RequestNeedMore(std::size_t requiredTotalBytes) noexcept
        : requiredTotalBytes_(requiredTotalBytes) {
        if (requiredTotalBytes_ && *requiredTotalBytes_ == 0) {
            std::terminate();
        }
    }

    std::optional<std::size_t> requiredTotalBytes_{};
};

// One completely framed HTTP/1 request. Owns its compact header descriptor
// block in the parse resource. Field strings and body borrow the input passed
// to parse() and remain valid only while those bytes remain alive and unmoved.
class Http1ParsedRequest final {
public:
    [[nodiscard]] const HttpRequest& request() const& noexcept {
        return request_;
    }
    [[nodiscard]] const HttpRequest& request() const&& = delete;

    [[nodiscard]] const Http1RequestBodyPlan& bodyPlan() const& noexcept {
        return bodyPlan_;
    }
    [[nodiscard]] const Http1RequestBodyPlan& bodyPlan() const&& = delete;

    // Exact wire bytes after the header section and before the next message.
    // For Content-Length this is the payload. For chunked framing it retains
    // every chunk-size line, delimiter, and trailer field so a sans-I/O owner
    // can drive the shared decoder without the parser silently dropping data.
    [[nodiscard]] std::string_view wireBody() const noexcept {
        return wireBody_;
    }

    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend struct detail::Http1RequestParseResultAccess;

    // Transfer the descriptor block; header text continues to borrow input.
    Http1ParsedRequest(HttpRequest request, Http1RequestBodyPlan bodyPlan,
        std::string_view wireBody, std::size_t consumedBytes) noexcept
        : request_(std::move(request)),
          bodyPlan_(bodyPlan),
          wireBody_(wireBody),
          consumedBytes_(consumedBytes) {}

    HttpRequest request_;
    Http1RequestBodyPlan bodyPlan_;
    std::string_view wireBody_;
    std::size_t consumedBytes_{0};
};

enum class Http1RequestParseFailureSource : std::uint8_t { kRequestLine,
    kMessage };

class Http1RequestParseFailure final {
public:
    [[nodiscard]] HttpProtocolError protocolError() const noexcept {
        return httpParseProtocolError(error_);
    }

    [[nodiscard]] constexpr Http1RequestParseFailureSource source() const noexcept {
        return source_;
    }

private:
    friend struct detail::Http1RequestParseResultAccess;

    explicit constexpr Http1RequestParseFailure(
        HttpParseError error, Http1RequestParseFailureSource source) noexcept
        : error_(error),
          source_(source) {}

    HttpParseError error_;
    Http1RequestParseFailureSource source_;
};

// Cleartext listeners can silently discard inputs that failed before a valid
// HTTP-version token was recognized (for example, TLS bytes on an HTTP port).
[[nodiscard]] bool shouldDropInvalidCleartextHttp1Input(
    std::string_view buffer, Http1RequestParseFailureSource source) noexcept;

// A discriminated parse outcome. Request data exists only in Http1ParsedRequest,
// an error exists only in Http1RequestParseFailure, and input sizing exists only
// in Http1RequestNeedMore.
// Callers therefore cannot read a default request after an error or mistake a
// required buffer size for bytes already consumed.
class Http1RequestParseResult final {
public:
    [[nodiscard]] const Http1RequestNeedMore* needMore() const& noexcept {
        return std::get_if<Http1RequestNeedMore>(&state_);
    }
    const Http1RequestNeedMore* needMore() const&& = delete;

    [[nodiscard]] const Http1ParsedRequest* parsed() const& noexcept {
        return std::get_if<Http1ParsedRequest>(&state_);
    }
    const Http1ParsedRequest* parsed() const&& = delete;

    [[nodiscard]] const Http1RequestParseFailure* failure() const& noexcept {
        return std::get_if<Http1RequestParseFailure>(&state_);
    }
    const Http1RequestParseFailure* failure() const&& = delete;

private:
    friend struct detail::Http1RequestParseResultAccess;

    explicit Http1RequestParseResult(Http1RequestNeedMore state) noexcept
        : state_(state) {}

    explicit Http1RequestParseResult(Http1ParsedRequest state) noexcept
        : state_(std::move(state)) {}

    explicit Http1RequestParseResult(Http1RequestParseFailure state) noexcept
        : state_(state) {}

    std::variant<Http1RequestNeedMore, Http1ParsedRequest, Http1RequestParseFailure> state_;
};

struct Http1RequestParseOptions final {
    // Must outlive the result. nullptr uses the default PMR resource.
    std::pmr::memory_resource* resource{nullptr};
};

// Stateless whole-message scanner: field text is zero-copy, descriptors are
// allocated in the parse resource. Allocation failures propagate; syntax and
// framing failures use the typed result. No transfer decoding or input mutation.
class Http1RequestParser final {
public:
    [[nodiscard]] Http1RequestParseResult parse(std::string_view buffer,
        Http1RequestParseOptions options = {}) const;

    template <detail::HttpTemporaryOwningCharString Buffer>
    Http1RequestParseResult parse(Buffer&&, Http1RequestParseOptions = {}) const = delete;
};

}  // namespace ruvia
