#pragma once

#include <cstddef>
#include <exception>
#include <optional>
#include <stdexcept>

#include "ruvia/http/http1_server_semantics.h"

namespace ruvia::detail {

// Connection-private ownership of the Web product's request-count policy.
// HTTP still owns persistence and response framing; this connection state keeps
// an optional saturating remaining-response budget, contributes one typed close policy
// before a streamed head is committed, and records each completed route once.
class http1_request_sequence final {
public:
    explicit http1_request_sequence(std::optional<std::size_t> max_requests)
        : requests_until_close_(max_requests) {
        if (max_requests.has_value() && *max_requests == 0) {
            throw std::invalid_argument(
                "configured requests-per-connection limit must be greater than zero");
        }
    }

    http1_request_sequence(std::size_t) = delete;

    http1_request_sequence(const http1_request_sequence&) = delete;
    http1_request_sequence& operator=(const http1_request_sequence&) = delete;
    http1_request_sequence(http1_request_sequence&&) = delete;
    http1_request_sequence& operator=(http1_request_sequence&&) = delete;

    [[nodiscard]] http1_close_policy next_response_close_policy() const noexcept {
        return requests_until_close_.has_value() && *requests_until_close_ == 1
                   ? http1_close_policy::close_after_response
                   : http1_close_policy::allow_reuse;
    }

    // Buffered response bytes have not been committed yet, so the request
    // budget may still tighten the protocol plan before Connection fields are
    // finalized.
    [[nodiscard]] http1_request_connection_plan complete_uncommitted_response(
        http1_request_connection_plan connection_plan) noexcept {
        const auto close_policy = next_response_close_policy();
        record_completion();
        return close_policy == http1_close_policy::close_after_response ? connection_plan.require_close()
                                                                        : connection_plan;
    }

    // A streamed head already carries the pre-commit close policy. Once bytes
    // are committed the plan cannot be tightened; fail fast if a caller tries
    // to complete a limit-ending response whose wire plan still permits reuse.
    void complete_committed_response(http1_request_connection_plan connection_plan) noexcept {
        if (next_response_close_policy() == http1_close_policy::close_after_response &&
            connection_plan.disposition() != http1_close_policy::close_after_response) {
            std::terminate();
        }
        record_completion();
    }

private:
    void record_completion() noexcept {
        // One is deliberately saturated so an impossible extra completion
        // cannot reopen reuse; absence remains unlimited.
        if (requests_until_close_.has_value() && *requests_until_close_ > 1) {
            --*requests_until_close_;
        }
    }

    std::optional<std::size_t> requests_until_close_;
};

}  // namespace ruvia::detail
