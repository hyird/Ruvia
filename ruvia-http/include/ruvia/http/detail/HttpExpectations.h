#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

#include "ruvia/http/detail/HeaderTokenUtils.h"

namespace ruvia::detail {

// Whether the framing/lifecycle owner has established that request content will
// follow the initial head. Keep this typed: HTTP/1 derives it from its body plan,
// while HTTP/2 derives it from END_STREAM rather than from Content-Length alone.
enum class HttpRequestContentIndication : std::uint8_t {
    kNoContent,
    kWillFollow
};

// A protocol action consumed by a server runtime. Absence is returned as an
// empty optional. kUnsupported is deliberately not a parse error: Expect is
// extensible and RFC 9110 permits (rather than requires) a server to answer an
// unknown expectation with 417.
enum class HttpServerExpectationAction : std::uint8_t {
    kSend100Continue,
    kUnsupported
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

    [[nodiscard]] std::optional<HttpServerExpectationAction> serverAction(
        HttpRequestContentIndication content) const noexcept {
        if (hasUnsupported()) {
            return HttpServerExpectationAction::kUnsupported;
        }
        if (has100Continue() &&
            content == HttpRequestContentIndication::kWillFollow) {
            return HttpServerExpectationAction::kSend100Continue;
        }
        return std::nullopt;
    }

private:
    static constexpr std::uint8_t k100Continue = 1U << 0;
    static constexpr std::uint8_t kUnsupported = 1U << 1;

    std::uint8_t flags_{0};
};

static_assert(std::is_trivially_copyable_v<HttpRequestExpectations>);
static_assert(sizeof(HttpRequestExpectations) <= 1);

}  // namespace ruvia::detail
