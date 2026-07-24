#pragma once

#include <memory_resource>
#include <utility>
#include <variant>

#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

class ContextPendingResponse final {
public:
    explicit ContextPendingResponse(std::pmr::memory_resource* resource)
        : response_(resource) {}

    [[nodiscard]] HttpResponse& response() noexcept {
        return response_;
    }
    [[nodiscard]] const HttpResponse& response() const noexcept {
        return response_;
    }

private:
    HttpResponse response_;
};

class ContextProvisionalResponse final {
public:
    explicit ContextProvisionalResponse(HttpResponse&& response)
        : response_(std::move(response)) {}

    [[nodiscard]] HttpResponse& response() noexcept {
        return response_;
    }
    [[nodiscard]] const HttpResponse& response() const noexcept {
        return response_;
    }

private:
    HttpResponse response_;
};

class ContextFinalResponse final {
public:
    explicit ContextFinalResponse(HttpResponse&& response)
        : response_(std::move(response)) {}

    [[nodiscard]] HttpResponse& response() noexcept {
        return response_;
    }
    [[nodiscard]] const HttpResponse& response() const noexcept {
        return response_;
    }

private:
    HttpResponse response_;
};

// One tagged value owns the response lifecycle, so phase and storage cannot
// disagree.
class ContextResponseState final {
public:
    explicit ContextResponseState(std::pmr::memory_resource* resource)
        : resource_(resource),
          value_(std::in_place_type<ContextPendingResponse>, resource) {}

    [[nodiscard]] const ContextPendingResponse* pending() const& noexcept {
        return std::get_if<ContextPendingResponse>(&value_);
    }
    const ContextPendingResponse* pending() const&& = delete;

    [[nodiscard]] const ContextProvisionalResponse* provisional() const& noexcept {
        return std::get_if<ContextProvisionalResponse>(&value_);
    }
    const ContextProvisionalResponse* provisional() const&& = delete;

    [[nodiscard]] const ContextFinalResponse* final() const& noexcept {
        return std::get_if<ContextFinalResponse>(&value_);
    }
    const ContextFinalResponse* final() const&& = delete;

    [[nodiscard]] HttpResponse& activeResponse() & noexcept {
        return std::visit([](auto& state) -> HttpResponse& { return state.response(); }, value_);
    }
    HttpResponse& activeResponse() && = delete;

    [[nodiscard]] const HttpResponse& activeResponse() const& noexcept {
        return std::visit([](const auto& state) -> const HttpResponse& { return state.response(); }, value_);
    }
    const HttpResponse& activeResponse() const&& = delete;

    [[nodiscard]] HttpResponse& materializeProvisional() {
        if (auto* pendingState = std::get_if<ContextPendingResponse>(&value_)) {
            auto response = std::move(pendingState->response());
            value_.template emplace<ContextProvisionalResponse>(std::move(response));
        }
        return activeResponse();
    }

    void finalize(HttpResponse&& response) {
        value_.template emplace<ContextFinalResponse>(std::move(response));
    }

    void finalizeActive() {
        auto response = std::move(activeResponse());
        value_.template emplace<ContextFinalResponse>(std::move(response));
    }

    [[nodiscard]] HttpResponse take() {
        if (pending() != nullptr) {
            return HttpResponse(resource_);
        }
        auto response = std::move(activeResponse());
        value_.template emplace<ContextPendingResponse>(resource_);
        return response;
    }

private:
    std::pmr::memory_resource* resource_;
    std::variant<ContextPendingResponse, ContextProvisionalResponse, ContextFinalResponse> value_;
};

}  // namespace ruvia::detail
