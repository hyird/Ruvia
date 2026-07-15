#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"

namespace ruvia::detail {

// Whether the framing/lifecycle owner has established that request content will
// follow the initial head. Keep this typed: HTTP/1 derives it from its body plan,
// while HTTP/2 derives it from END_STREAM rather than from Content-Length alone.
enum class HttpRequestContentIndication : std::uint8_t {
    kNoContent,
    kWillFollow
};

// Whether the product accepts unknown expectation extensions. Expect remains
// valid syntax either way; the HTTP contract owns the protocol response chosen
// by the explicit rejection policy.
enum class HttpUnsupportedExpectationPolicy : std::uint8_t {
    kIgnore,
    kReject
};

class HttpServerExpectationPlan;

class HttpNoServerExpectationAction final {
private:
    friend class HttpServerExpectationPlan;
    constexpr HttpNoServerExpectationAction() noexcept = default;
};

class HttpSend100Continue final {
private:
    friend class HttpServerExpectationPlan;
    constexpr HttpSend100Continue() noexcept = default;
};

class HttpUnsupportedExpectationRejection final {
public:
    [[nodiscard]] HttpProtocolError protocolError() const noexcept {
        return HttpProtocolError(417, "unsupported Expect header");
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
    [[nodiscard]] constexpr const HttpNoServerExpectationAction*
    noAction() const & noexcept {
        return std::get_if<HttpNoServerExpectationAction>(&value_);
    }
    const HttpNoServerExpectationAction* noAction() const && = delete;

    [[nodiscard]] constexpr const HttpSend100Continue*
    send100Continue() const & noexcept {
        return std::get_if<HttpSend100Continue>(&value_);
    }
    const HttpSend100Continue* send100Continue() const && = delete;

    [[nodiscard]] constexpr const HttpUnsupportedExpectationRejection*
    rejection() const & noexcept {
        return std::get_if<HttpUnsupportedExpectationRejection>(&value_);
    }
    const HttpUnsupportedExpectationRejection* rejection() const && = delete;

private:
    friend class HttpRequestExpectations;

    using Value = std::variant<
        HttpNoServerExpectationAction,
        HttpSend100Continue,
        HttpUnsupportedExpectationRejection>;

    template <typename Alternative>
    explicit constexpr HttpServerExpectationPlan(
        Alternative alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static constexpr HttpServerExpectationPlan noActionPlan()
        noexcept {
        return HttpServerExpectationPlan(HttpNoServerExpectationAction());
    }

    [[nodiscard]] static constexpr HttpServerExpectationPlan continuePlan()
        noexcept {
        return HttpServerExpectationPlan(HttpSend100Continue());
    }

    [[nodiscard]] static constexpr HttpServerExpectationPlan rejectionPlan()
        noexcept {
        return HttpServerExpectationPlan(
            HttpUnsupportedExpectationRejection());
    }

    Value value_;
};

// Incremental recipient-side state for the RFC 9110 Expect #list. Repeated field
// lines extend the same logical list, empty members are ignored, and all state is
// fixed-size. The only standardized member is 100-continue; every other non-empty
// member is retained as the single semantic fact "unsupported" for product policy.
class HttpRequestExpectations final {
public:
    void parseField(std::string_view value) noexcept {
        httpVisitCommaSeparatedQuoted(
            value,
            [this](std::string_view member) noexcept {
                if (httpAsciiEqualsIgnoreCase(member, "100-continue")) {
                    flags_ |= k100Continue;
                } else {
                    flags_ |= kUnsupported;
                }
                return true;
            });
    }

    [[nodiscard]] bool has100Continue() const noexcept {
        return (flags_ & k100Continue) != 0;
    }

    [[nodiscard]] bool hasUnsupported() const noexcept {
        return (flags_ & kUnsupported) != 0;
    }

    // RFC 9110 requires an HTTP/1.0 recipient to ignore 100-continue. Preserve
    // the independent unsupported-member fact so the Web product can still apply
    // its chosen extension-support policy.
    void ignore100Continue() noexcept {
        flags_ &= static_cast<std::uint8_t>(~k100Continue);
    }

    [[nodiscard]] HttpServerExpectationPlan serverPlan(
        HttpRequestContentIndication content,
        HttpUnsupportedExpectationPolicy unsupportedPolicy) const noexcept {
        if (hasUnsupported() &&
            unsupportedPolicy == HttpUnsupportedExpectationPolicy::kReject) {
            return HttpServerExpectationPlan::rejectionPlan();
        }
        if (has100Continue() &&
            content == HttpRequestContentIndication::kWillFollow) {
            return HttpServerExpectationPlan::continuePlan();
        }
        return HttpServerExpectationPlan::noActionPlan();
    }

private:
    static constexpr std::uint8_t k100Continue = 1U << 0;
    static constexpr std::uint8_t kUnsupported = 1U << 1;

    std::uint8_t flags_{0};
};

static_assert(std::is_trivially_copyable_v<HttpRequestExpectations>);
static_assert(sizeof(HttpRequestExpectations) <= 1);
static_assert(std::is_trivially_copyable_v<HttpServerExpectationPlan>);
static_assert(sizeof(HttpServerExpectationPlan) <= 2);

}  // namespace ruvia::detail
