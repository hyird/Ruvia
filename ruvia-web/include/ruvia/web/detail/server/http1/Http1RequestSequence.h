#pragma once

#include "ruvia/http/detail/http1/Http1ServerSemantics.h"

#include <cstddef>
#include <exception>
#include <optional>
#include <stdexcept>

namespace ruvia::detail {

// Connection-private ownership of the Web product's request-count policy.
// HTTP still owns persistence and response framing; this connection state keeps
// an optional saturating remaining-response budget, contributes one typed close policy
// before a streamed head is committed, and records each completed route once.
class Http1RequestSequence final {
public:
    explicit Http1RequestSequence(std::optional<std::size_t> maxRequests)
        : requestsUntilClose_(maxRequests) {
        if (maxRequests.has_value() && *maxRequests == 0) {
            throw std::invalid_argument(
                "configured requests-per-connection limit must be greater than zero");
        }
    }

    Http1RequestSequence(std::size_t) = delete;

    Http1RequestSequence(const Http1RequestSequence&) = delete;
    Http1RequestSequence& operator=(const Http1RequestSequence&) = delete;
    Http1RequestSequence(Http1RequestSequence&&) = delete;
    Http1RequestSequence& operator=(Http1RequestSequence&&) = delete;

    [[nodiscard]] Http1ClosePolicy nextResponseClosePolicy() const noexcept {
        return requestsUntilClose_.has_value() && *requestsUntilClose_ == 1
                   ? Http1ClosePolicy::kCloseAfterResponse
                   : Http1ClosePolicy::kAllowReuse;
    }

    // Buffered response bytes have not been committed yet, so the request
    // budget may still tighten the protocol plan before Connection fields are
    // finalized.
    [[nodiscard]] Http1ServerConnectionPlan completeUncommittedResponse(
        Http1ServerConnectionPlan connectionPlan) noexcept {
        const auto closePolicy = nextResponseClosePolicy();
        recordCompletion();
        return closePolicy == Http1ClosePolicy::kCloseAfterResponse ? connectionPlan.requireClose()
                                                                    : connectionPlan;
    }

    // A streamed head already carries the pre-commit close policy. Once bytes
    // are committed the plan cannot be tightened; fail fast if a caller tries
    // to complete a limit-ending response whose wire plan still permits reuse.
    void completeCommittedResponse(Http1ServerConnectionPlan connectionPlan) noexcept {
        if (nextResponseClosePolicy() == Http1ClosePolicy::kCloseAfterResponse &&
            connectionPlan.disposition() != Http1ClosePolicy::kCloseAfterResponse) {
            std::terminate();
        }
        recordCompletion();
    }

private:
    void recordCompletion() noexcept {
        // One is deliberately saturated so an impossible extra completion
        // cannot reopen reuse; absence remains unlimited.
        if (requestsUntilClose_.has_value() && *requestsUntilClose_ > 1) {
            --*requestsUntilClose_;
        }
    }

    std::optional<std::size_t> requestsUntilClose_;
};

}  // namespace ruvia::detail
