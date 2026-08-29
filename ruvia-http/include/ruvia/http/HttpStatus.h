#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace ruvia {

// A validated HTTP status-code value. The complete 100..599 extension space is
// representable, while arbitrary integers cannot leak into protocol APIs.
class HttpStatusCode final {
public:
    [[nodiscard]] static constexpr HttpStatusCode fromValue(std::uint16_t value) {
        if (!isValidValue(value)) {
            throw std::invalid_argument("HTTP status code must be in 100..599");
        }
        return HttpStatusCode(value, ValidatedTag{});
    }

    [[nodiscard]] static constexpr std::optional<HttpStatusCode> tryFromValue(
        std::uint16_t value) noexcept {
        if (!isValidValue(value)) {
            return std::nullopt;
        }
        return HttpStatusCode(value, ValidatedTag{});
    }

    [[nodiscard]] static constexpr bool isValidValue(std::uint16_t value) noexcept {
        return value >= 100 && value <= 599;
    }

    [[nodiscard]] constexpr std::uint16_t value() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr bool isInformational() const noexcept {
        return value_ < 200;
    }

    [[nodiscard]] constexpr bool isSuccessful() const noexcept {
        return value_ >= 200 && value_ < 300;
    }

    [[nodiscard]] constexpr bool isRedirection() const noexcept {
        return value_ >= 300 && value_ < 400;
    }

    [[nodiscard]] constexpr bool isClientError() const noexcept {
        return value_ >= 400 && value_ < 500;
    }

    [[nodiscard]] constexpr bool isServerError() const noexcept {
        return value_ >= 500;
    }

    [[nodiscard]] constexpr bool isError() const noexcept {
        return value_ >= 400;
    }

    [[nodiscard]] constexpr bool isFinal() const noexcept {
        return value_ >= 200;
    }

    friend constexpr auto operator<=>(HttpStatusCode, HttpStatusCode) = default;

private:
    struct ValidatedTag {};

    constexpr HttpStatusCode(std::uint16_t value, ValidatedTag) noexcept
        : value_(value) {}

    std::uint16_t value_;
};

static_assert(std::is_trivially_copyable_v<HttpStatusCode>);
static_assert(sizeof(HttpStatusCode) == sizeof(std::uint16_t));

namespace detail {

inline constexpr std::size_t kHttpStatusCodeTokenSize = 3;
using HttpStatusCodeToken = std::array<char, kHttpStatusCodeTokenSize>;

[[nodiscard]] inline constexpr HttpStatusCodeToken httpStatusCodeToken(
    HttpStatusCode status) noexcept {
    const auto value = status.value();
    return {static_cast<char>('0' + value / 100), static_cast<char>('0' + (value / 10) % 10),
        static_cast<char>('0' + value % 10)};
}

[[nodiscard]] inline constexpr std::string_view httpStatusCodeTokenView(
    const HttpStatusCodeToken& token) noexcept {
    return std::string_view(token.data(), token.size());
}

[[nodiscard]] std::string_view httpStatusCodeTokenView(HttpStatusCodeToken&&) = delete;

}  // namespace detail

// Stable RFC-assigned names are defined once here. Unknown extension codes and
// temporary draft registrations remain available through fromValue() and
// tryFromValue() without turning an unstable name into framework API.
namespace http_status {

inline constexpr auto kContinue = HttpStatusCode::fromValue(100);
inline constexpr auto kSwitchingProtocols = HttpStatusCode::fromValue(101);
inline constexpr auto kProcessing = HttpStatusCode::fromValue(102);
inline constexpr auto kEarlyHints = HttpStatusCode::fromValue(103);

inline constexpr auto kOk = HttpStatusCode::fromValue(200);
inline constexpr auto kCreated = HttpStatusCode::fromValue(201);
inline constexpr auto kAccepted = HttpStatusCode::fromValue(202);
inline constexpr auto kNonAuthoritativeInformation = HttpStatusCode::fromValue(203);
inline constexpr auto kNoContent = HttpStatusCode::fromValue(204);
inline constexpr auto kResetContent = HttpStatusCode::fromValue(205);
inline constexpr auto kPartialContent = HttpStatusCode::fromValue(206);
inline constexpr auto kMultiStatus = HttpStatusCode::fromValue(207);
inline constexpr auto kAlreadyReported = HttpStatusCode::fromValue(208);
inline constexpr auto kImUsed = HttpStatusCode::fromValue(226);

inline constexpr auto kMultipleChoices = HttpStatusCode::fromValue(300);
inline constexpr auto kMovedPermanently = HttpStatusCode::fromValue(301);
inline constexpr auto kFound = HttpStatusCode::fromValue(302);
inline constexpr auto kSeeOther = HttpStatusCode::fromValue(303);
inline constexpr auto kNotModified = HttpStatusCode::fromValue(304);
inline constexpr auto kUseProxy = HttpStatusCode::fromValue(305);
inline constexpr auto kTemporaryRedirect = HttpStatusCode::fromValue(307);
inline constexpr auto kPermanentRedirect = HttpStatusCode::fromValue(308);

inline constexpr auto kBadRequest = HttpStatusCode::fromValue(400);
inline constexpr auto kUnauthorized = HttpStatusCode::fromValue(401);
inline constexpr auto kPaymentRequired = HttpStatusCode::fromValue(402);
inline constexpr auto kForbidden = HttpStatusCode::fromValue(403);
inline constexpr auto kNotFound = HttpStatusCode::fromValue(404);
inline constexpr auto kMethodNotAllowed = HttpStatusCode::fromValue(405);
inline constexpr auto kNotAcceptable = HttpStatusCode::fromValue(406);
inline constexpr auto kProxyAuthenticationRequired = HttpStatusCode::fromValue(407);
inline constexpr auto kRequestTimeout = HttpStatusCode::fromValue(408);
inline constexpr auto kConflict = HttpStatusCode::fromValue(409);
inline constexpr auto kGone = HttpStatusCode::fromValue(410);
inline constexpr auto kLengthRequired = HttpStatusCode::fromValue(411);
inline constexpr auto kPreconditionFailed = HttpStatusCode::fromValue(412);
inline constexpr auto kContentTooLarge = HttpStatusCode::fromValue(413);
inline constexpr auto kUriTooLong = HttpStatusCode::fromValue(414);
inline constexpr auto kUnsupportedMediaType = HttpStatusCode::fromValue(415);
inline constexpr auto kRangeNotSatisfiable = HttpStatusCode::fromValue(416);
inline constexpr auto kExpectationFailed = HttpStatusCode::fromValue(417);
inline constexpr auto kMisdirectedRequest = HttpStatusCode::fromValue(421);
inline constexpr auto kUnprocessableContent = HttpStatusCode::fromValue(422);
inline constexpr auto kLocked = HttpStatusCode::fromValue(423);
inline constexpr auto kFailedDependency = HttpStatusCode::fromValue(424);
inline constexpr auto kTooEarly = HttpStatusCode::fromValue(425);
inline constexpr auto kUpgradeRequired = HttpStatusCode::fromValue(426);
inline constexpr auto kPreconditionRequired = HttpStatusCode::fromValue(428);
inline constexpr auto kTooManyRequests = HttpStatusCode::fromValue(429);
inline constexpr auto kRequestHeaderFieldsTooLarge = HttpStatusCode::fromValue(431);
inline constexpr auto kUnavailableForLegalReasons = HttpStatusCode::fromValue(451);

inline constexpr auto kInternalServerError = HttpStatusCode::fromValue(500);
inline constexpr auto kNotImplemented = HttpStatusCode::fromValue(501);
inline constexpr auto kBadGateway = HttpStatusCode::fromValue(502);
inline constexpr auto kServiceUnavailable = HttpStatusCode::fromValue(503);
inline constexpr auto kGatewayTimeout = HttpStatusCode::fromValue(504);
inline constexpr auto kHttpVersionNotSupported = HttpStatusCode::fromValue(505);
inline constexpr auto kVariantAlsoNegotiates = HttpStatusCode::fromValue(506);
inline constexpr auto kInsufficientStorage = HttpStatusCode::fromValue(507);
inline constexpr auto kLoopDetected = HttpStatusCode::fromValue(508);
inline constexpr auto kNotExtended = HttpStatusCode::fromValue(510);
inline constexpr auto kNetworkAuthenticationRequired = HttpStatusCode::fromValue(511);

}  // namespace http_status

// RFC 9112 reason-phrase is optional HTTP/1 presentation text, not response
// semantics. Stable RFC-assigned codes get a conventional phrase; temporary,
// extension, and unassigned codes deliberately get an empty phrase.
[[nodiscard]] inline constexpr std::string_view httpReasonPhrase(HttpStatusCode status) noexcept {
    switch (status.value()) {
        case http_status::kContinue.value():
            return "Continue";
        case http_status::kSwitchingProtocols.value():
            return "Switching Protocols";
        case http_status::kProcessing.value():
            return "Processing";
        case http_status::kEarlyHints.value():
            return "Early Hints";
        case http_status::kOk.value():
            return "OK";
        case http_status::kCreated.value():
            return "Created";
        case http_status::kAccepted.value():
            return "Accepted";
        case http_status::kNonAuthoritativeInformation.value():
            return "Non-Authoritative Information";
        case http_status::kNoContent.value():
            return "No Content";
        case http_status::kResetContent.value():
            return "Reset Content";
        case http_status::kPartialContent.value():
            return "Partial Content";
        case http_status::kMultiStatus.value():
            return "Multi-Status";
        case http_status::kAlreadyReported.value():
            return "Already Reported";
        case http_status::kImUsed.value():
            return "IM Used";
        case http_status::kMultipleChoices.value():
            return "Multiple Choices";
        case http_status::kMovedPermanently.value():
            return "Moved Permanently";
        case http_status::kFound.value():
            return "Found";
        case http_status::kSeeOther.value():
            return "See Other";
        case http_status::kNotModified.value():
            return "Not Modified";
        case http_status::kUseProxy.value():
            return "Use Proxy";
        case http_status::kTemporaryRedirect.value():
            return "Temporary Redirect";
        case http_status::kPermanentRedirect.value():
            return "Permanent Redirect";
        case http_status::kBadRequest.value():
            return "Bad Request";
        case http_status::kUnauthorized.value():
            return "Unauthorized";
        case http_status::kPaymentRequired.value():
            return "Payment Required";
        case http_status::kForbidden.value():
            return "Forbidden";
        case http_status::kNotFound.value():
            return "Not Found";
        case http_status::kMethodNotAllowed.value():
            return "Method Not Allowed";
        case http_status::kNotAcceptable.value():
            return "Not Acceptable";
        case http_status::kProxyAuthenticationRequired.value():
            return "Proxy Authentication Required";
        case http_status::kRequestTimeout.value():
            return "Request Timeout";
        case http_status::kConflict.value():
            return "Conflict";
        case http_status::kGone.value():
            return "Gone";
        case http_status::kLengthRequired.value():
            return "Length Required";
        case http_status::kPreconditionFailed.value():
            return "Precondition Failed";
        case http_status::kContentTooLarge.value():
            return "Content Too Large";
        case http_status::kUriTooLong.value():
            return "URI Too Long";
        case http_status::kUnsupportedMediaType.value():
            return "Unsupported Media Type";
        case http_status::kRangeNotSatisfiable.value():
            return "Range Not Satisfiable";
        case http_status::kExpectationFailed.value():
            return "Expectation Failed";
        case http_status::kMisdirectedRequest.value():
            return "Misdirected Request";
        case http_status::kUnprocessableContent.value():
            return "Unprocessable Content";
        case http_status::kLocked.value():
            return "Locked";
        case http_status::kFailedDependency.value():
            return "Failed Dependency";
        case http_status::kTooEarly.value():
            return "Too Early";
        case http_status::kUpgradeRequired.value():
            return "Upgrade Required";
        case http_status::kPreconditionRequired.value():
            return "Precondition Required";
        case http_status::kTooManyRequests.value():
            return "Too Many Requests";
        case http_status::kRequestHeaderFieldsTooLarge.value():
            return "Request Header Fields Too Large";
        case http_status::kUnavailableForLegalReasons.value():
            return "Unavailable For Legal Reasons";
        case http_status::kInternalServerError.value():
            return "Internal Server Error";
        case http_status::kNotImplemented.value():
            return "Not Implemented";
        case http_status::kBadGateway.value():
            return "Bad Gateway";
        case http_status::kServiceUnavailable.value():
            return "Service Unavailable";
        case http_status::kGatewayTimeout.value():
            return "Gateway Timeout";
        case http_status::kHttpVersionNotSupported.value():
            return "HTTP Version Not Supported";
        case http_status::kVariantAlsoNegotiates.value():
            return "Variant Also Negotiates";
        case http_status::kInsufficientStorage.value():
            return "Insufficient Storage";
        case http_status::kLoopDetected.value():
            return "Loop Detected";
        case http_status::kNotExtended.value():
            return "Not Extended";
        case http_status::kNetworkAuthenticationRequired.value():
            return "Network Authentication Required";
        default:
            return {};
    }
}

namespace detail {

[[nodiscard]] inline constexpr bool httpFinalStatusCodeValid(HttpStatusCode status) noexcept {
    return status.isFinal();
}

// 101 is a protocol transition rather than an interim progress head. It is
// intentionally owned by a dedicated Upgrade driver instead of either generic
// response-head type.
[[nodiscard]] inline constexpr bool httpInterimStatusCodeValid(HttpStatusCode status) noexcept {
    return status.isInformational() && status != http_status::kSwitchingProtocols;
}

}  // namespace detail

}  // namespace ruvia
