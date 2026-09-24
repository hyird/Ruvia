#pragma once

#include <cstddef>
#include <exception>
#include <optional>
#include <string_view>
#include <variant>

#include "ruvia/http/Http1RequestBodyPlan.h"
#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/http/HttpAcceptEncoding.h"
#include "ruvia/http/HttpParseError.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/Http1ServerConnectionPlan.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"

namespace ruvia {

class Http1ServerRequestParseFailure;

namespace detail {
struct Http1RequestParseResultAccess;
}

enum class Http1ServerRequestParseFailureSource : std::uint8_t { kRequestLine,
    kMessage };

namespace detail {

struct Http1RequestParseResultAccess final {
    [[nodiscard]] static Http1RequestParseResult needMore() noexcept {
        return Http1RequestParseResult(Http1RequestNeedMore());
    }

    [[nodiscard]] static Http1RequestParseResult needMore(std::size_t requiredTotalBytes) noexcept {
        return Http1RequestParseResult(Http1RequestNeedMore(requiredTotalBytes));
    }

    // HttpRequest is a fixed-size collection of borrowed views. The result
    // copies it once into its own value object; no owning data is transferred.
    [[nodiscard]] static Http1RequestParseResult parsed(HttpRequest request,
        Http1RequestBodyPlan bodyPlan, std::string_view wireBody,
        std::size_t consumedBytes) noexcept {
        return Http1RequestParseResult(
            Http1ParsedRequest(std::move(request), bodyPlan, wireBody, consumedBytes));
    }

    [[nodiscard]] static Http1RequestParseResult failure(HttpParseError error,
        Http1RequestParseFailureSource source) noexcept {
        return Http1RequestParseResult(Http1RequestParseFailure(error, source));
    }

    [[nodiscard]] static Http1RequestParseResult failure(
        const ::ruvia::Http1ServerRequestParseFailure& failure) noexcept;
};

}  // namespace detail

class Http1ServerNeedRequestHead final {};

class Http1ServerRequestHeadReady final {
public:
    [[nodiscard]] constexpr std::size_t headerBytes() const noexcept {
        return headerBytes_;
    }

private:
    friend class Http1ServerRequestParser;

    explicit constexpr Http1ServerRequestHeadReady(std::size_t headerBytes) noexcept
        : headerBytes_(headerBytes) {
        if (headerBytes_ == 0) {
            std::terminate();
        }
    }

    std::size_t headerBytes_;
};

class Http1ServerNeedRequestBody final {
public:
    [[nodiscard]] constexpr std::size_t headerBytes() const noexcept {
        return headerBytes_;
    }

    [[nodiscard]] constexpr std::optional<std::size_t> requiredTotalBytes() const noexcept {
        return requiredTotalBytes_;
    }

private:
    friend class Http1ServerRequestParser;

    explicit constexpr Http1ServerNeedRequestBody(std::size_t headerBytes) noexcept
        : headerBytes_(headerBytes) {
        if (headerBytes_ == 0) {
            std::terminate();
        }
    }

    constexpr Http1ServerNeedRequestBody(
        std::size_t headerBytes, std::size_t requiredTotalBytes) noexcept
        : headerBytes_(headerBytes),
          requiredTotalBytes_(requiredTotalBytes) {
        if (headerBytes_ == 0 || (requiredTotalBytes_ && *requiredTotalBytes_ <= headerBytes_)) {
            std::terminate();
        }
    }

    std::size_t headerBytes_;
    std::optional<std::size_t> requiredTotalBytes_{};
};

class Http1ServerRequestMessageReady final {
public:
    [[nodiscard]] constexpr std::size_t headerBytes() const noexcept {
        return headerBytes_;
    }

    [[nodiscard]] constexpr std::size_t messageBytes() const noexcept {
        return messageBytes_;
    }

private:
    friend class Http1ServerRequestParser;

    constexpr Http1ServerRequestMessageReady(
        std::size_t headerBytes, std::size_t messageBytes) noexcept
        : headerBytes_(headerBytes),
          messageBytes_(messageBytes) {
        if (headerBytes_ == 0 || messageBytes_ < headerBytes_) {
            std::terminate();
        }
    }

    std::size_t headerBytes_;
    std::size_t messageBytes_;
};

class Http1ServerRequestParseFailure final {
public:
    [[nodiscard]] HttpProtocolError protocolError() const noexcept {
        return httpParseProtocolError(error_);
    }

    [[nodiscard]] constexpr Http1ServerRequestParseFailureSource source() const noexcept {
        return error_ == HttpParseError::kInvalidRequestLine ||
                       error_ == HttpParseError::kUnsupportedHttpVersion
                   ? Http1ServerRequestParseFailureSource::kRequestLine
                   : Http1ServerRequestParseFailureSource::kMessage;
    }

private:
    friend class Http1ServerRequestParser;
    friend struct detail::Http1RequestParseResultAccess;

    explicit constexpr Http1ServerRequestParseFailure(HttpParseError error) noexcept
        : error_(error) {}

    HttpParseError error_;
};

namespace detail {
inline Http1RequestParseResult Http1RequestParseResultAccess::failure(
    const ::ruvia::Http1ServerRequestParseFailure& failure) noexcept {
    const auto source = failure.source() == Http1ServerRequestParseFailureSource::kRequestLine
                            ? Http1RequestParseFailureSource::kRequestLine
                            : Http1RequestParseFailureSource::kMessage;
    return Http1RequestParseResult(Http1RequestParseFailure(failure.error_, source));
}

}  // namespace detail

class Http1ServerRequestParseState final {
public:
    [[nodiscard]] const Http1ServerNeedRequestHead* needRequestHead() const& noexcept {
        return std::get_if<Http1ServerNeedRequestHead>(&progress_);
    }
    [[nodiscard]] const Http1ServerNeedRequestHead* needRequestHead() const&& = delete;

    [[nodiscard]] const Http1ServerRequestHeadReady* headReady() const& noexcept {
        return std::get_if<Http1ServerRequestHeadReady>(&progress_);
    }
    [[nodiscard]] const Http1ServerRequestHeadReady* headReady() const&& = delete;

    [[nodiscard]] const Http1ServerNeedRequestBody* needRequestBody() const& noexcept {
        return std::get_if<Http1ServerNeedRequestBody>(&progress_);
    }
    [[nodiscard]] const Http1ServerNeedRequestBody* needRequestBody() const&& = delete;

    [[nodiscard]] const Http1ServerRequestMessageReady* messageReady() const& noexcept {
        return std::get_if<Http1ServerRequestMessageReady>(&progress_);
    }
    [[nodiscard]] const Http1ServerRequestMessageReady* messageReady() const&& = delete;

    [[nodiscard]] const Http1ServerRequestParseFailure* failure() const& noexcept {
        return std::get_if<Http1ServerRequestParseFailure>(&progress_);
    }
    [[nodiscard]] const Http1ServerRequestParseFailure* failure() const&& = delete;

    HttpRequest request{detail::HttpRequestAccess::make()};
    Http1RequestBodyPlan bodyPlan{Http1RequestBodyPlan(HttpRequestExpectations{})};
    Http1ServerConnectionPlan connectionPlan{Http1ServerConnectionPlan::http11Close()};

    // Parsed once from the request head. Representation policy consumes this
    // complete client preference later, without rescanning Accept-Encoding.
    [[nodiscard]] HttpResponseCodingSelectionResult responseCodingSelection() const noexcept {
        return HttpResponseCodingSelection::select(responseCodingQualities);
    }

private:
    friend class Http1ServerRequestParser;

    // The compact request owns its descriptor block across parse attempts.
    // Only this small progress value changes, so head/message/required/error
    // metadata exists solely in the alternative where it is meaningful.
    using Progress = std::variant<Http1ServerNeedRequestHead, Http1ServerRequestHeadReady,
        Http1ServerNeedRequestBody, Http1ServerRequestMessageReady, Http1ServerRequestParseFailure>;

    Progress progress_{Http1ServerNeedRequestHead{}};
    HttpResponseCodingQualities responseCodingQualities;
};

class Http1ServerRequestParser final {
public:
    // Allocate the exact header descriptor block only after accepting the head.
    // The resource must outlive state; field text still borrows buffer.
    void parseHead(std::string_view buffer, Http1ServerRequestParseState& state,
        std::size_t headerSearchOffset = 0, std::pmr::memory_resource* resource = nullptr) const;

    template <detail::HttpTemporaryOwningCharString Buffer>
    void parseHead(Buffer&&, Http1ServerRequestParseState&, std::size_t = 0,
        std::pmr::memory_resource* = nullptr) const = delete;

    // Whole-message scanner used by the public sans-I/O API. It always advances
    // beyond kRequestHeadReady to an unambiguous message/failure/need-more phase.
    [[nodiscard]] Http1ServerRequestParseState parseMessage(std::string_view buffer,
        std::pmr::memory_resource* resource = nullptr) const;

    template <detail::HttpTemporaryOwningCharString Buffer>
    Http1ServerRequestParseState parseMessage(Buffer&&, std::pmr::memory_resource* = nullptr) const = delete;

private:
    static void parseRequestHead(std::string_view buffer, std::size_t headerSearchOffset,
        Http1ServerRequestParseState& state, std::pmr::memory_resource* resource);
    static void parseMessageBody(
        std::string_view buffer, Http1ServerRequestParseState& state) noexcept;
};

}  // namespace ruvia
