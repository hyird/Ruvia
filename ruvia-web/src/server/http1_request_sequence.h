#pragma once

#include <cstddef>
#include <exception>
#include <optional>
#include <stdexcept>

#include "ruvia/http/http1_server_semantics.h"

#include "server/http_server_worker_state.h"

namespace ruvia::detail {

// Connection-private ownership of the Web product's request-count policy.
// HTTP still owns persistence and response framing; this connection state keeps
// an optional saturating remaining-response budget, contributes one typed close policy
// before a streamed head is committed, and records each completed route once.
// A draining worker also closes the connection after its current response.
class http1_request_sequence final {
public:
    http1_request_sequence(
        std::optional<std::size_t> max_requests, const http_server_worker_state& worker_state)
        : requests_until_close_(max_requests),
          worker_state_(worker_state) {
        if (max_requests.has_value() && *max_requests == 0) {
            throw std::invalid_argument(
                "configured requests-per-connection limit must be greater than zero");
        }
    }

    http1_request_sequence(std::size_t, const http_server_worker_state&) = delete;

    http1_request_sequence(const http1_request_sequence&) = delete;
    http1_request_sequence& operator=(const http1_request_sequence&) = delete;
    http1_request_sequence(http1_request_sequence&&) = delete;
    http1_request_sequence& operator=(http1_request_sequence&&) = delete;

    [[nodiscard]] http1_close_policy next_response_close_policy() const noexcept {
        return budget_ends_with_next_response() || !http_server_worker_running(worker_state_)
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
    // A drain that began after the commit closes the connection instead.
    void complete_committed_response(http1_request_connection_plan connection_plan) noexcept {
        if (budget_ends_with_next_response() &&
            connection_plan.disposition() != http1_close_policy::close_after_response) {
            std::terminate();
        }
        record_completion();
    }

private:
    [[nodiscard]] bool budget_ends_with_next_response() const noexcept {
        return requests_until_close_.has_value() && *requests_until_close_ == 1;
    }

    void record_completion() noexcept {
        // One is deliberately saturated so an impossible extra completion
        // cannot reopen reuse; absence remains unlimited.
        if (requests_until_close_.has_value() && *requests_until_close_ > 1) {
            --*requests_until_close_;
        }
    }

    std::optional<std::size_t> requests_until_close_;
    const http_server_worker_state& worker_state_;
};

}  // namespace ruvia::detail
