#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/util/HttpOws.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"
#include "ruvia/http/detail/response/HttpResponseKnownHeaders.h"

namespace ruvia::detail {

// RFC 9110 section 5.6.1 deliberately gives senders and recipients different
// list obligations: senders cannot generate empty members, while recipients
// must ignore a reasonable number of them. The HTTP/1 head-size limit bounds
// recipient work, so the tolerant path remains allocation-free and O(head).
enum class HttpFieldListRole : std::uint8_t { kRecipient, kSender };

enum class HttpFieldListParseStatus : std::uint8_t { kOk, kMalformed, kRejected };

enum class HttpConnectionOption : std::uint8_t { kClose = 1U << 0, kKeepAlive = 1U << 1, kUpgrade = 1U << 2, kTe = 1U << 3 };

[[nodiscard]] inline bool httpConnectionOptionConflictsWithManagedField(std::string_view option) noexcept {
    if (httpAsciiEqualsIgnoreCase(option, "Host") || httpAsciiEqualsIgnoreCase(option, "Expect") || httpAsciiEqualsIgnoreCase(option, "Trailer")) {
        return true;
    }
    switch (classifyRequestHeader(option)) {
        case RequestHeaderKind::kOther:
        case RequestHeaderKind::kConnection:
        case RequestHeaderKind::kTransferEncoding:
        case RequestHeaderKind::kUpgrade:
            break;
        case RequestHeaderKind::kAccept:
        case RequestHeaderKind::kAcceptEncoding:
        case RequestHeaderKind::kAccessControlRequestHeaders:
        case RequestHeaderKind::kAccessControlRequestMethod:
        case RequestHeaderKind::kAuthorization:
        case RequestHeaderKind::kContentEncoding:
        case RequestHeaderKind::kContentLength:
        case RequestHeaderKind::kContentType:
        case RequestHeaderKind::kCookie:
        case RequestHeaderKind::kExpect:
        case RequestHeaderKind::kHost:
        case RequestHeaderKind::kIfMatch:
        case RequestHeaderKind::kIfModifiedSince:
        case RequestHeaderKind::kIfNoneMatch:
        case RequestHeaderKind::kIfRange:
        case RequestHeaderKind::kIfUnmodifiedSince:
        case RequestHeaderKind::kOrigin:
        case RequestHeaderKind::kRange:
        case RequestHeaderKind::kSecWebSocketKey:
        case RequestHeaderKind::kSecWebSocketProtocol:
        case RequestHeaderKind::kSecWebSocketVersion:
        case RequestHeaderKind::kUserAgent:
            return true;
    }
    const auto knownBit = classifyResponseHeaderName(option);
    return knownBit != 0 && knownBit != kResponseHeaderConnection && knownBit != kResponseHeaderTransferEncoding;
}

// Incremental parser for the logical Connection field value. Repeated field
// lines extend the same state, so a caller cannot accidentally let a later
// occurrence erase an earlier close/Upgrade/TE signal.
class HttpConnectionOptions final {
public:
    [[nodiscard]] HttpFieldListParseStatus parseField(std::string_view fieldValue, HttpFieldListRole role) noexcept {
        return parseField(fieldValue, role, [](std::string_view) noexcept { return true; });
    }

    template <typename Visitor>
    [[nodiscard]] HttpFieldListParseStatus parseField(std::string_view fieldValue, HttpFieldListRole role, Visitor&& visitor) noexcept {
        auto parsedBits = state_;
        std::size_t start = 0;
        while (start <= fieldValue.size()) {
            const auto comma = fieldValue.find(',', start);
            const auto end = comma == std::string_view::npos ? fieldValue.size() : comma;
            const auto option = httpTrimOws(fieldValue.substr(start, end - start));
            if (option.empty()) {
                // An explicitly present but empty Connection field is useless
                // and cannot be safely extended by a later generated option;
                // the strict writer contract therefore rejects it as well as
                // leading, trailing, and interior empty members.
                if (role == HttpFieldListRole::kSender) {
                    return HttpFieldListParseStatus::kMalformed;
                }
            } else {
                if (!isValidHttpHeaderName(option)) {
                    return HttpFieldListParseStatus::kMalformed;
                }
                if (!visitor(option)) {
                    return HttpFieldListParseStatus::kRejected;
                }
                if (httpAsciiEqualsIgnoreCase(option, "close")) {
                    parsedBits |= bit(HttpConnectionOption::kClose);
                } else if (httpAsciiEqualsIgnoreCase(option, "keep-alive")) {
                    parsedBits |= bit(HttpConnectionOption::kKeepAlive);
                } else if (httpAsciiEqualsIgnoreCase(option, "Upgrade")) {
                    parsedBits |= bit(HttpConnectionOption::kUpgrade);
                } else if (httpAsciiEqualsIgnoreCase(option, "TE")) {
                    parsedBits |= bit(HttpConnectionOption::kTe);
                }
            }
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }

        state_ = static_cast<std::uint8_t>(parsedBits | kFieldPresentBit);
        return HttpFieldListParseStatus::kOk;
    }

    [[nodiscard]] bool hasField() const noexcept {
        return (state_ & kFieldPresentBit) != 0;
    }

    [[nodiscard]] bool contains(HttpConnectionOption option) const noexcept {
        return (state_ & bit(option)) != 0;
    }

    [[nodiscard]] bool close() const noexcept {
        return contains(HttpConnectionOption::kClose);
    }

    [[nodiscard]] bool keepAlive() const noexcept {
        return contains(HttpConnectionOption::kKeepAlive);
    }

    [[nodiscard]] bool upgrade() const noexcept {
        return contains(HttpConnectionOption::kUpgrade);
    }

    [[nodiscard]] bool te() const noexcept {
        return contains(HttpConnectionOption::kTe);
    }

private:
    static constexpr std::uint8_t kFieldPresentBit = 1U << 7;

    [[nodiscard]] static constexpr std::uint8_t bit(HttpConnectionOption option) noexcept {
        return static_cast<std::uint8_t>(option);
    }

    // Connection owns four recognised-token bits plus one orthogonal field
    // presence bit. Keeping them in one committed byte makes absent, present
    // empty/unknown, and present with recognised options impossible to tear.
    std::uint8_t state_{0};
};

static_assert(std::is_trivially_copyable_v<HttpConnectionOptions>);
static_assert(sizeof(HttpConnectionOptions) == 1);

struct HttpUpgradeProtocol final {
    std::string_view name;
    std::string_view version;
};

[[nodiscard]] inline bool httpParseUpgradeProtocol(std::string_view value, HttpUpgradeProtocol& output) noexcept {
    const auto slash = value.find('/');
    const auto name = slash == std::string_view::npos ? value : value.substr(0, slash);
    const auto version = slash == std::string_view::npos ? std::string_view{} : value.substr(slash + 1);
    if (!isValidHttpHeaderName(name) || (slash != std::string_view::npos && (!isValidHttpHeaderName(version) || version.find('/') != std::string_view::npos))) {
        return false;
    }
    output = HttpUpgradeProtocol{.name = name, .version = version};
    return true;
}

template <HttpTemporaryOwningCharString Value>
bool httpParseUpgradeProtocol(Value&&, HttpUpgradeProtocol&) = delete;

[[nodiscard]] inline bool httpUpgradeProtocolEquals(const HttpUpgradeProtocol& left, const HttpUpgradeProtocol& right) noexcept {
    // RFC 9110 section 7.8: protocol-name is case-insensitive, while an
    // optional protocol-version remains an exact token.
    return httpAsciiEqualsIgnoreCase(left.name, right.name) && left.version == right.version;
}

// Incremental Upgrade list parser. It owns repeated-field/list syntax, while
// the visitor owns policy such as whether a selected protocol was offered.
enum class HttpUpgradeFieldState : std::uint8_t {
    kAbsent,
    kPresentWithoutProtocol,
    kPresentWithProtocol,
};

class HttpUpgradeProtocols final {
public:
    template <typename Visitor>
    [[nodiscard]] HttpFieldListParseStatus parseField(std::string_view fieldValue, HttpFieldListRole role, Visitor&& visitor) noexcept {
        bool parsedProtocol = false;
        std::size_t start = 0;
        while (start <= fieldValue.size()) {
            const auto comma = fieldValue.find(',', start);
            const auto end = comma == std::string_view::npos ? fieldValue.size() : comma;
            const auto item = httpTrimOws(fieldValue.substr(start, end - start));
            if (item.empty()) {
                if (role == HttpFieldListRole::kSender) {
                    return HttpFieldListParseStatus::kMalformed;
                }
            } else {
                HttpUpgradeProtocol protocol;
                if (!httpParseUpgradeProtocol(item, protocol)) {
                    return HttpFieldListParseStatus::kMalformed;
                }
                if (!visitor(protocol)) {
                    return HttpFieldListParseStatus::kRejected;
                }
                parsedProtocol = true;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }

        if (parsedProtocol) {
            state_ = HttpUpgradeFieldState::kPresentWithProtocol;
        } else if (state_ == HttpUpgradeFieldState::kAbsent) {
            state_ = HttpUpgradeFieldState::kPresentWithoutProtocol;
        }
        return HttpFieldListParseStatus::kOk;
    }

    [[nodiscard]] bool hasField() const noexcept {
        return state_ != HttpUpgradeFieldState::kAbsent;
    }

    [[nodiscard]] bool hasProtocol() const noexcept {
        return state_ == HttpUpgradeFieldState::kPresentWithProtocol;
    }

private:
    HttpUpgradeFieldState state_{HttpUpgradeFieldState::kAbsent};
};

static_assert(std::is_trivially_copyable_v<HttpUpgradeProtocols>);
static_assert(sizeof(HttpUpgradeProtocols) == 1);

}  // namespace ruvia::detail
