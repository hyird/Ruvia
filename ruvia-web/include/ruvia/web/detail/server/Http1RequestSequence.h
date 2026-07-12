#pragma once

#include "ruvia/http/detail/http1/Http1ServerSemantics.h"

#include <cstddef>
#include <exception>

namespace ruvia::detail {

// Connection-private ownership of the Web product's request-count policy.
// HTTP still owns persistence and response framing; this one-word state keeps
// a saturating remaining-response budget, contributes one typed close policy
// before a streamed head is committed, and records each completed route once.
class Http1RequestSequence final {
public:
    explicit constexpr Http1RequestSequence(
        std::size_t maxRequests) noexcept
        : requestsUntilClose_(maxRequests) {}

    Http1RequestSequence(const Http1RequestSequence&) = delete;
    Http1RequestSequence& operator=(const Http1RequestSequence&) = delete;
    Http1RequestSequence(Http1RequestSequence&&) = delete;
    Http1RequestSequence& operator=(Http1RequestSequence&&) = delete;

    [[nodiscard]] constexpr Http1ServerClosePolicy
    nextResponseClosePolicy() const noexcept {
        return requestsUntilClose_ == 1
            ? Http1ServerClosePolicy::kCloseAfterResponse
            : Http1ServerClosePolicy::kAllowReuse;
    }

    // Buffered response bytes have not been committed yet, so the request
    // budget may still tighten the protocol plan before Connection fields are
    // finalized.
    [[nodiscard]] constexpr Http1ServerConnectionPlan
    completeUncommittedResponse(
        Http1ServerConnectionPlan connectionPlan) noexcept {
        const auto closePolicy = nextResponseClosePolicy();
        recordCompletion();
        return closePolicy == Http1ServerClosePolicy::kCloseAfterResponse
            ? connectionPlan.requireClose()
            : connectionPlan;
    }

    // A streamed head already carries the pre-commit close policy. Once bytes
    // are committed the plan cannot be tightened; fail fast if a caller tries
    // to complete a limit-ending response whose wire plan still permits reuse.
    void completeCommittedResponse(
        Http1ServerConnectionPlan connectionPlan) noexcept {
        if (nextResponseClosePolicy() ==
                Http1ServerClosePolicy::kCloseAfterResponse &&
            connectionPlan.disposition() !=
                Http1ConnectionDisposition::kClose) {
            std::terminate();
        }
        recordCompletion();
    }

private:
    constexpr void recordCompletion() noexcept {
        // Zero is the configured unlimited state. One is deliberately
        // saturated so an impossible extra completion cannot reopen reuse.
        if (requestsUntilClose_ > 1) {
            --requestsUntilClose_;
        }
    }

    std::size_t requestsUntilClose_;
};

}  // namespace ruvia::detail
