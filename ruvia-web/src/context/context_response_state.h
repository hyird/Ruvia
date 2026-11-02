#pragma once

#include <cassert>
#include <memory_resource>
#include <optional>
#include <utility>

#include "ruvia/http/http_response.h"

namespace ruvia::detail {

// Draft and selected response have disjoint ownership. Building a response
// consumes the draft; selecting or replacing a response never merges metadata.
class context_response_state final {
public:
    explicit context_response_state(std::pmr::memory_resource* resource)
        : resource_(resource),
          draft_(resource),
          policy_(resource),
          defaults_(resource) {}

    [[nodiscard]] http_response& draft() noexcept {
        return draft_;
    }
    [[nodiscard]] const http_response& draft() const noexcept {
        return draft_;
    }
    [[nodiscard]] http_response* final() noexcept {
        return final_ ? &*final_ : nullptr;
    }
    [[nodiscard]] const http_response* final() const noexcept {
        return final_ ? &*final_ : nullptr;
    }
    [[nodiscard]] http_response& policy() noexcept {
        return policy_;
    }
    [[nodiscard]] http_response& defaults() noexcept {
        return defaults_;
    }
    void policy_status(http_status_code status) {
        policy_status_ = status;
    }
    void apply_policy(http_response& response) const {
        if (!defaults_.headers().empty()) {
            response.transfer_headers_from(defaults_, http_response_header_transfer::merge);
        }
        if (!policy_.headers().empty()) {
            response.transfer_headers_from(policy_, http_response_header_transfer::apply);
        }
        if (policy_status_) {
            response.status(*policy_status_);
        }
    }

    [[nodiscard]] http_response take_draft() {
        auto response = std::move(draft_);
        draft_ = http_response(resource_);
        return response;
    }

    void finalize(http_response&& response) {
        final_.emplace(std::move(response));
    }

    [[nodiscard]] http_response&& release_final() & {
        assert(final_);
        apply_policy(*final_);
        return std::move(*final_);
    }
    http_response&& release_final() && = delete;

private:
    std::pmr::memory_resource* resource_;
    http_response draft_;
    http_response policy_;
    http_response defaults_;
    std::optional<http_status_code> policy_status_;
    std::optional<http_response> final_;
};

}  // namespace ruvia::detail
