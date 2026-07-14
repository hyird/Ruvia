#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

#include "ruvia/http/HttpInterimResponse.h"

namespace ruvia {

namespace detail {
struct Http1InterimResponsePrepareResultAccess;
}

enum class Http1InterimResponsePrepareError : std::uint8_t {
    kInvalidHeader,
    kTooManyHeaders,
    kContentLengthForbidden,
    kTransferEncodingForbidden,
    kTrailerForbidden,
    kTeFieldForbidden,
    kRepeatedSingleton,
    kInvalidConnection,
    kInvalidUpgrade,
    kUpgradeConnectionOptionRequired,
    kHeaderTooLarge,
};

[[nodiscard]] std::string_view http1InterimResponsePrepareErrorMessage(
    Http1InterimResponsePrepareError error) noexcept;

class Http1InterimResponseBufferTooSmall final {
public:
    [[nodiscard]] constexpr std::size_t requiredHeadBytes() const noexcept {
        return requiredHeadBytes_;
    }

private:
    friend struct detail::Http1InterimResponsePrepareResultAccess;

    explicit constexpr Http1InterimResponseBufferTooSmall(
        std::size_t requiredHeadBytes) noexcept
        : requiredHeadBytes_(requiredHeadBytes) {}

    std::size_t requiredHeadBytes_;
};

// A transactionally encoded HTTP/1.1 interim head. The byte view points into
// the caller's output buffer. A Connection: close option cannot terminate the
// interim message; the owner must remember it and close after the required
// final response instead.
class PreparedHttp1InterimResponse final {
public:
    [[nodiscard]] constexpr std::string_view head() const noexcept {
        return head_;
    }

    [[nodiscard]] constexpr bool requiresFinalConnectionClose() const noexcept {
        return requiresFinalConnectionClose_;
    }

private:
    friend struct detail::Http1InterimResponsePrepareResultAccess;

    constexpr PreparedHttp1InterimResponse(
        std::string_view head,
        bool requiresFinalConnectionClose) noexcept
        : head_(head),
          requiresFinalConnectionClose_(requiresFinalConnectionClose) {}

    std::string_view head_;
    bool requiresFinalConnectionClose_{false};
};

class Http1InterimResponsePrepareFailure final {
public:
    [[nodiscard]] constexpr Http1InterimResponsePrepareError error() const noexcept {
        return error_;
    }

private:
    friend struct detail::Http1InterimResponsePrepareResultAccess;

    explicit constexpr Http1InterimResponsePrepareFailure(
        Http1InterimResponsePrepareError error) noexcept
        : error_(error) {}

    Http1InterimResponsePrepareError error_;
};

enum class Http1InterimResponsePrepareKind : std::uint8_t {
    kBufferTooSmall,
    kPrepared,
    kFailure,
};

class Http1InterimResponsePrepareResult final {
public:
    [[nodiscard]] constexpr Http1InterimResponsePrepareKind kind() const noexcept {
        if (std::holds_alternative<PreparedHttp1InterimResponse>(state_)) {
            return Http1InterimResponsePrepareKind::kPrepared;
        }
        return std::holds_alternative<Http1InterimResponsePrepareFailure>(state_)
            ? Http1InterimResponsePrepareKind::kFailure
            : Http1InterimResponsePrepareKind::kBufferTooSmall;
    }

    [[nodiscard]] constexpr const Http1InterimResponseBufferTooSmall*
    bufferTooSmall() const & noexcept {
        return std::get_if<Http1InterimResponseBufferTooSmall>(&state_);
    }
    const Http1InterimResponseBufferTooSmall* bufferTooSmall() const && = delete;

    [[nodiscard]] constexpr const PreparedHttp1InterimResponse*
    prepared() const & noexcept {
        return std::get_if<PreparedHttp1InterimResponse>(&state_);
    }
    const PreparedHttp1InterimResponse* prepared() const && = delete;

    [[nodiscard]] constexpr const Http1InterimResponsePrepareFailure*
    failure() const & noexcept {
        return std::get_if<Http1InterimResponsePrepareFailure>(&state_);
    }
    const Http1InterimResponsePrepareFailure* failure() const && = delete;

private:
    friend struct detail::Http1InterimResponsePrepareResultAccess;

    explicit constexpr Http1InterimResponsePrepareResult(
        Http1InterimResponseBufferTooSmall state) noexcept
        : state_(state) {}

    explicit constexpr Http1InterimResponsePrepareResult(
        PreparedHttp1InterimResponse state) noexcept
        : state_(state) {}

    explicit constexpr Http1InterimResponsePrepareResult(
        Http1InterimResponsePrepareFailure state) noexcept
        : state_(state) {}

    std::variant<
        Http1InterimResponseBufferTooSmall,
        PreparedHttp1InterimResponse,
        Http1InterimResponsePrepareFailure> state_;
};

// Allocation-free HTTP/1.1 interim-head writer. It validates the complete
// borrowed field set and exact 64-field/64-KiB bounds before touching the
// output buffer. No Server or Date field is injected: the encoded fields are
// exactly those represented by HttpInterimResponseHead.
class Http1InterimResponseWriter final {
public:
    [[nodiscard]] Http1InterimResponsePrepareResult prepare(
        const HttpInterimResponseHead& response,
        std::span<char> headBuffer) const noexcept;
};

}  // namespace ruvia
