#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

namespace ruvia::detail {

// The standardized Expect field member is defined once so parsers and writers
// cannot drift on its wire spelling.
inline constexpr std::string_view kHttpContinueExpectationToken = "100-continue";

[[nodiscard]] inline bool httpExpectationToken(std::string_view token) noexcept {
    if (token.empty()) {
        return false;
    }
    for (const auto ch : token) {
        if (!isHttpTokenChar(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool httpExpectationQuotedString(std::string_view value) noexcept {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        return false;
    }
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        auto ch = static_cast<unsigned char>(value[i]);
        if (ch == '\\') {
            ++i;
            if (i + 1 >= value.size()) {
                return false;
            }
            ch = static_cast<unsigned char>(value[i]);
        } else if (ch == '"') {
            return false;
        }
        if (!isHttpFieldValueChar(ch)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool httpExpectationValue(std::string_view value) noexcept {
    return httpExpectationToken(value) || httpExpectationQuotedString(value);
}

[[nodiscard]] inline bool httpExpectationItem(std::string_view item) noexcept {
    item = httpTrimOws(item);
    if (item.empty()) {
        return false;
    }

    const auto parametersStart = httpFindUnquotedDelimiter(item, 0, ';');
    const auto leading = httpTrimOws(item.substr(0, parametersStart));
    const auto equals = leading.find('=');
    if (equals == std::string_view::npos) {
        return parametersStart == item.size() && httpExpectationToken(leading);
    }

    const auto name = httpTrimOws(leading.substr(0, equals));
    const auto value = httpTrimOws(leading.substr(equals + 1));
    if (!httpExpectationToken(name) || !httpExpectationValue(value)) {
        return false;
    }

    std::size_t start = parametersStart;
    while (start < item.size()) {
        ++start;
        const auto end = httpFindUnquotedDelimiter(item, start, ';');
        const auto parameter = httpTrimOws(item.substr(start, end - start));
        const auto parameterEquals = parameter.find('=');
        if (parameterEquals == std::string_view::npos) {
            return false;
        }
        const auto parameterName = httpTrimOws(parameter.substr(0, parameterEquals));
        const auto parameterValue = httpTrimOws(parameter.substr(parameterEquals + 1));
        if (!httpExpectationToken(parameterName) || !httpExpectationValue(parameterValue)) {
            return false;
        }
        start = end;
    }
    return true;
}

// Sender-side Expect grammar. Recipients may apply a looser parsing policy for
// empty list members and unsupported extensions; a client must not generate
// syntactically invalid expectation values.
[[nodiscard]] inline bool isValidHttpExpectFieldValue(std::string_view value) noexcept {
    bool valid = true;
    bool sawItem = false;
    httpVisitCommaSeparatedQuotedItems(value, [&valid, &sawItem](std::string_view item) noexcept {
        sawItem = true;
        if (!httpExpectationItem(item)) {
            valid = false;
            return false;
        }
        return true;
    });
    return valid && sawItem;
}

// Recipient-side Expect grammar. Empty list members are tolerated by the generic
// field-list parser, but every non-empty member still has to be a syntactically
// valid expectation before product policy decides whether unsupported extensions
// are rejected with 417 or ignored.
[[nodiscard]] inline bool isValidReceivedHttpExpectFieldValue(std::string_view value) noexcept {
    bool valid = true;
    httpVisitCommaSeparatedQuotedItems(value, [&valid](std::string_view item) noexcept {
        item = httpTrimOws(item);
        if (item.empty()) {
            return true;
        }
        if (!httpExpectationItem(item)) {
            valid = false;
            return false;
        }
        return true;
    });
    return valid;
}

}  // namespace ruvia::detail

namespace ruvia {

// Whether the framing/lifecycle owner has established that request content will
// follow the initial head. Keep this typed: HTTP/1 derives it from its body plan,
// while HTTP/2 combines its receive-half and remaining-content states so an open
// metadata-only or known-empty stream cannot masquerade as pending content.
enum class HttpRequestContentIndication : std::uint8_t { kNoContent, kWillFollow };

// RFC 9110 Section 10.1.1 forbids a client from generating 100-continue when
// the request has no content. Keep this sender check next to the recipient-side
// expectation state so HTTP/1 and HTTP/2 cannot derive different answers.
[[nodiscard]] constexpr bool httpClientExpectationIsValid(
    bool hasContinue, HttpRequestContentIndication content) noexcept {
    return !hasContinue || content == HttpRequestContentIndication::kWillFollow;
}

// Whether the product accepts unknown expectation extensions. Expect remains
// valid syntax either way; the HTTP contract owns the protocol response chosen
// by the explicit rejection policy.
enum class HttpUnsupportedExpectationPolicy : std::uint8_t { kIgnore, kReject };

class HttpServerExpectationPlan;

class HttpNoServerExpectationAction final {
private:
    friend class HttpServerExpectationPlan;
    constexpr HttpNoServerExpectationAction() noexcept = default;
};

class HttpSendContinue final {
private:
    friend class HttpServerExpectationPlan;
    constexpr HttpSendContinue() noexcept = default;
};

class HttpUnsupportedExpectationRejection final {
public:
    [[nodiscard]] HttpProtocolError protocolError() const noexcept {
        return HttpProtocolError(http_status::kExpectationFailed, "unsupported Expect header");
    }

private:
    friend class HttpServerExpectationPlan;
    constexpr HttpUnsupportedExpectationRejection() noexcept = default;
};

// No action, an interim response, and a final rejection are mutually exclusive
// protocol outcomes. Keep them typed so runtimes cannot compare a semantic enum
// and then reconstruct the required status themselves.
class HttpServerExpectationPlan final {
public:
    [[nodiscard]] constexpr const HttpNoServerExpectationAction* noAction() const& noexcept {
        return state_ == State::kNoAction ? &kNoAction : nullptr;
    }
    const HttpNoServerExpectationAction* noAction() const&& = delete;

    [[nodiscard]] constexpr const HttpSendContinue* sendContinue() const& noexcept {
        return state_ == State::kSendContinue ? &kSendContinue : nullptr;
    }
    const HttpSendContinue* sendContinue() const&& = delete;

    [[nodiscard]] constexpr const HttpUnsupportedExpectationRejection* rejection() const& noexcept {
        return state_ == State::kRejection ? &kRejection : nullptr;
    }
    const HttpUnsupportedExpectationRejection* rejection() const&& = delete;

private:
    friend class HttpRequestExpectations;

    enum class State : std::uint8_t { kNoAction, kSendContinue, kRejection };

    explicit constexpr HttpServerExpectationPlan(State state) noexcept
        : state_(state) {}

    [[nodiscard]] static constexpr HttpServerExpectationPlan noActionPlan() noexcept {
        return HttpServerExpectationPlan(State::kNoAction);
    }

    [[nodiscard]] static constexpr HttpServerExpectationPlan continuePlan() noexcept {
        return HttpServerExpectationPlan(State::kSendContinue);
    }

    [[nodiscard]] static constexpr HttpServerExpectationPlan rejectionPlan() noexcept {
        return HttpServerExpectationPlan(State::kRejection);
    }

    static inline constexpr HttpNoServerExpectationAction kNoAction{};
    static inline constexpr HttpSendContinue kSendContinue{};
    static inline constexpr HttpUnsupportedExpectationRejection kRejection{};

    State state_;
};

// Incremental recipient-side state for the RFC 9110 Expect #list. Repeated field
// lines extend the same logical list, empty members are ignored, and all state is
// fixed-size. The only standardized member is 100-continue; every other non-empty
// member is retained as the single semantic fact "unsupported" for product policy.
class HttpRequestExpectations final {
public:
    void parseField(std::string_view value) noexcept {
        detail::httpVisitCommaSeparatedQuoted(value, [this](std::string_view member) noexcept {
            if (detail::httpAsciiEqualsIgnoreCase(member, detail::kHttpContinueExpectationToken)) {
                flags_ |= kContinue;
            } else {
                flags_ |= kUnsupported;
            }
            return true;
        });
    }

    [[nodiscard]] bool hasContinue() const noexcept {
        return (flags_ & kContinue) != 0;
    }

    [[nodiscard]] bool hasUnsupported() const noexcept {
        return (flags_ & kUnsupported) != 0;
    }

    // RFC 9110 requires an HTTP/1.0 recipient to ignore 100-continue. Preserve
    // the independent unsupported-member fact so the Web product can still apply
    // its chosen extension-support policy.
    void ignoreContinue() noexcept {
        flags_ &= static_cast<std::uint8_t>(~kContinue);
    }

    [[nodiscard]] HttpServerExpectationPlan serverPlan(HttpRequestContentIndication content,
        HttpUnsupportedExpectationPolicy unsupportedPolicy) const noexcept {
        if (hasUnsupported() && unsupportedPolicy == HttpUnsupportedExpectationPolicy::kReject) {
            return HttpServerExpectationPlan::rejectionPlan();
        }
        if (hasContinue() && content == HttpRequestContentIndication::kWillFollow) {
            return HttpServerExpectationPlan::continuePlan();
        }
        return HttpServerExpectationPlan::noActionPlan();
    }

private:
    static constexpr std::uint8_t kContinue = 1U << 0;
    static constexpr std::uint8_t kUnsupported = 1U << 1;

    std::uint8_t flags_{0};
};

static_assert(std::is_trivially_copyable_v<HttpRequestExpectations>);
static_assert(sizeof(HttpRequestExpectations) <= 1);
static_assert(std::is_trivially_copyable_v<HttpServerExpectationPlan>);
static_assert(sizeof(HttpServerExpectationPlan) <= 2);

}  // namespace ruvia
