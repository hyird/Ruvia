#pragma once

#include <memory_resource>
#include <utility>
#include <variant>

#include "ruvia/http/http_response.h"

namespace ruvia::detail {

class context_pending_response final {
public:
    explicit context_pending_response(std::pmr::memory_resource* resource)
        : response_({.resource_ = resource}) {}

    [[nodiscard]] http_response& response() noexcept {
        return response_;
    }
    [[nodiscard]] const http_response& response() const noexcept {
        return response_;
    }

private:
    http_response response_;
};

class context_provisional_response final {
public:
    explicit context_provisional_response(http_response&& response)
        : response_(std::move(response)) {}

    [[nodiscard]] http_response& response() noexcept {
        return response_;
    }
    [[nodiscard]] const http_response& response() const noexcept {
        return response_;
    }

private:
    http_response response_;
};

class context_final_response final {
public:
    explicit context_final_response(http_response&& response)
        : response_(std::move(response)) {}

    [[nodiscard]] http_response& response() noexcept {
        return response_;
    }
    [[nodiscard]] const http_response& response() const noexcept {
        return response_;
    }

private:
    http_response response_;
};

// One tagged value owns the response lifecycle, so phase and storage cannot
// disagree.
class context_response_state final {
public:
    explicit context_response_state(std::pmr::memory_resource* resource)
        : resource_(resource),
          value_(std::in_place_type<context_pending_response>, resource) {}

    [[nodiscard]] const context_pending_response* pending() const& noexcept {
        return std::get_if<context_pending_response>(&value_);
    }
    const context_pending_response* pending() const&& = delete;

    [[nodiscard]] const context_provisional_response* provisional() const& noexcept {
        return std::get_if<context_provisional_response>(&value_);
    }
    const context_provisional_response* provisional() const&& = delete;

    [[nodiscard]] const context_final_response* final() const& noexcept {
        return std::get_if<context_final_response>(&value_);
    }
    const context_final_response* final() const&& = delete;

    [[nodiscard]] http_response& active_response() & noexcept {
        return std::visit([](auto& state_value) -> http_response& { return state_value.response(); }, value_);
    }
    http_response& active_response() && = delete;

    [[nodiscard]] const http_response& active_response() const& noexcept {
        return std::visit(
            [](const auto& state_value) -> const http_response& { return state_value.response(); }, value_);
    }
    const http_response& active_response() const&& = delete;

    [[nodiscard]] http_response& materialize_provisional() {
        if (auto* pending_state = std::get_if<context_pending_response>(&value_)) {
            auto response = std::move(pending_state->response());
            value_.template emplace<context_provisional_response>(std::move(response));
        }
        return active_response();
    }

    void finalize(http_response&& response) {
        value_.template emplace<context_final_response>(std::move(response));
    }

    void finalize_active() {
        auto response = std::move(active_response());
        value_.template emplace<context_final_response>(std::move(response));
    }

    [[nodiscard]] http_response take() {
        if (pending() != nullptr) {
            return http_response({.resource_ = resource_});
        }
        auto response = std::move(active_response());
        value_.template emplace<context_pending_response>(resource_);
        return response;
    }

private:
    std::pmr::memory_resource* resource_;
    std::variant<context_pending_response, context_provisional_response, context_final_response> value_;
};

}  // namespace ruvia::detail
