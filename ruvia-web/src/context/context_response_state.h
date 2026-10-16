#pragma once

#include <cassert>
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

// A final response plus the context metadata assigned after it became final.
// The overlay starts empty for every final response: a response replacing this
// one starts from what was set afterwards, never from the discarded response.
class context_final_response final {
public:
    context_final_response(http_response&& response, std::pmr::memory_resource* resource)
        : response_(std::move(response)),
          metadata_({.resource_ = resource}) {}

    [[nodiscard]] http_response& response() noexcept {
        return response_;
    }
    [[nodiscard]] const http_response& response() const noexcept {
        return response_;
    }

    [[nodiscard]] http_response& metadata() noexcept {
        return metadata_;
    }
    [[nodiscard]] const http_response& metadata() const noexcept {
        return metadata_;
    }

private:
    http_response response_;
    http_response metadata_;
};

// One tagged value owns the response lifecycle, so phase and storage cannot
// disagree.
class context_response_state final {
public:
    explicit context_response_state(std::pmr::memory_resource* resource)
        : resource_(resource),
          value_(std::in_place_type<context_pending_response>, resource) {}

    [[nodiscard]] const context_final_response* final() const& noexcept {
        return std::get_if<context_final_response>(&value_);
    }
    const context_final_response* final() const&& = delete;
    [[nodiscard]] context_final_response* final() & noexcept {
        return std::get_if<context_final_response>(&value_);
    }
    context_final_response* final() && = delete;

    [[nodiscard]] http_response& active_response() & noexcept {
        return std::visit([](auto& state_value) -> http_response& { return state_value.response(); }, value_);
    }
    http_response& active_response() && = delete;

    [[nodiscard]] const http_response& active_response() const& noexcept {
        return std::visit(
            [](const auto& state_value) -> const http_response& { return state_value.response(); }, value_);
    }
    const http_response& active_response() const&& = delete;

    // Applies a context metadata mutation (status, headers, cookies). Before a
    // final response exists it edits the pending or provisional response. Once
    // final it records the change in the final metadata overlay, so a
    // replacement built afterwards keeps it, and post-processes the final
    // response. The overlay goes first: inputs may borrow the final response's
    // own header storage, which only the final response's update may release.
    template <typename t_update>
    void update_metadata(t_update&& update) {
        if (auto* final_state = std::get_if<context_final_response>(&value_)) {
            update(final_state->metadata());
            update(final_state->response());
            return;
        }
        update(active_response());
    }

    // The context metadata a newly built response starts from. A final
    // response describes its own representation -- Content-Encoding,
    // Content-Range, validators, cache policy -- so a response replacing it,
    // an error included, inherits none of it: once final, only the metadata
    // assigned after finalization applies.
    [[nodiscard]] const http_response& metadata() const& noexcept {
        if (const auto* final_state = final()) {
            return final_state->metadata();
        }
        return active_response();
    }
    const http_response& metadata() const&& = delete;

    [[nodiscard]] http_response& materialize_provisional() {
        if (auto* pending_state = std::get_if<context_pending_response>(&value_)) {
            auto response = std::move(pending_state->response());
            value_.template emplace<context_provisional_response>(std::move(response));
        }
        return active_response();
    }

    void finalize(http_response&& response) {
        value_.template emplace<context_final_response>(std::move(response), resource_);
    }

    void finalize_active() {
        auto response = std::move(active_response());
        value_.template emplace<context_final_response>(std::move(response), resource_);
    }

    // Hands the final response to the dispatcher's return without an
    // intermediate move. The context ends with that dispatch: it keeps only the
    // moved-from response and must not answer again.
    [[nodiscard]] http_response&& release_final() & noexcept {
        auto* final_state = std::get_if<context_final_response>(&value_);
        assert(final_state != nullptr);
        return std::move(final_state->response());
    }
    http_response&& release_final() && = delete;

private:
    std::pmr::memory_resource* resource_;
    std::variant<context_pending_response, context_provisional_response, context_final_response> value_;
};

}  // namespace ruvia::detail
