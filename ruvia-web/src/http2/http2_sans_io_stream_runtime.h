#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_request_trailers.h"

#include "http2/http2_sans_io_request_body.h"
#include "http2/http2_sans_io_stream_signal.h"
#include "http2/http2_sans_io_termination.h"
#include "router/route_resolution.h"
#include "server/request_deadline.h"

namespace ruvia::detail {

class http2_sans_io_selected_route final {
public:
    [[nodiscard]] const route_resolution& resolution() const& noexcept {
        return resolution_;
    }
    const route_resolution& resolution() const&& = delete;

    // Per stream, not per connection: one HTTP/2 connection multiplexes many
    // requests and each carries its own deadline. This state is address-stable
    // and non-movable, which is what the timer registration and the token
    // handed to context_services both require.
    void arm_deadline(const worker_handle& worker_value, const stop_token& worker_stop,
        std::chrono::milliseconds deadline_value) {
        deadline_.emplace(worker_stop);
        deadline_->arm(worker_value, deadline_value);
    }

    [[nodiscard]] const request_deadline* deadline() const noexcept {
        return deadline_ ? &*deadline_ : nullptr;
    }

    [[nodiscard]] http2_request_body_runtime& body() & noexcept {
        return body_;
    }
    http2_request_body_runtime& body() && = delete;

    [[nodiscard]] const http2_request_body_runtime& body() const& noexcept {
        return body_;
    }
    const http2_request_body_runtime& body() const&& = delete;

    [[nodiscard]] bool dispatched() const noexcept {
        return signal() != nullptr;
    }

    [[nodiscard]] http2_sans_io_stream_signal* signal() & noexcept {
        return std::get_if<http2_sans_io_stream_signal>(&dispatch_);
    }
    [[nodiscard]] http2_sans_io_stream_signal* signal() && = delete;

    [[nodiscard]] const http2_sans_io_stream_signal* signal() const& noexcept {
        return std::get_if<http2_sans_io_stream_signal>(&dispatch_);
    }
    [[nodiscard]] const http2_sans_io_stream_signal* signal() const&& = delete;

private:
    friend class http2_sans_io_stream_runtime;

    // The private token lets the owning runtime construct this non-movable
    // address-stable state directly inside its optional storage.
    struct token_type final {};
    struct awaiting_dispatch_type final {};

    using dispatch_state_type = std::variant<awaiting_dispatch_type, http2_sans_io_stream_signal>;

public:
    http2_sans_io_selected_route(token_type, route_resolution resolution, request_body_mode body_mode,
        std::pmr::memory_resource* resource) noexcept
        : resolution_(std::move(resolution)),
          body_(body_mode, resource) {}

private:
    [[nodiscard]] http2_sans_io_stream_signal* begin_dispatch(
        const worker_handle& worker_value, http2_sans_io_termination& termination) {
        if (dispatched()) {
            return nullptr;
        }
        dispatch_.emplace<http2_sans_io_stream_signal>(worker_value, termination);
        return signal();
    }

    route_resolution resolution_;
    std::optional<request_deadline> deadline_;
    http2_request_body_runtime body_;
    dispatch_state_type dispatch_;
};

class http2_sans_io_stream_runtime final {
public:
    http2_sans_io_stream_runtime(std::uint32_t stream_id, std::pmr::memory_resource* resource,
        std::pmr::memory_resource* body_resource = nullptr)
        : stream_id_(stream_id),
          resource_(pmr_resource_or_default(resource)),
          body_resource_(body_resource != nullptr ? body_resource : resource_),
          trailers_(resource_) {}

    void bind_push_parent(std::uint32_t id) noexcept {
        push_parent_ = id;
    }
    [[nodiscard]] std::uint32_t push_parent() const noexcept {
        return push_parent_;
    }
    [[nodiscard]] http_request_trailers& trailers() & noexcept {
        return trailers_;
    }
    [[nodiscard]] const std::optional<http_priority>& priority_update() const& noexcept {
        return priority_update_;
    }
    void reprioritize(http_priority priority) noexcept {
        priority_update_ = priority;
    }

    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

    [[nodiscard]] bool hold_request_head(http2_request_head_event&& request_head) noexcept {
        if (request_head_ || request_head.stream_id() != stream_id_) {
            return false;
        }
        request_head_.emplace(std::move(request_head));
        return true;
    }

    [[nodiscard]] http2_request_head_event* request_head() & noexcept {
        return request_head_ ? &*request_head_ : nullptr;
    }
    http2_request_head_event* request_head() && = delete;

    [[nodiscard]] bool select_route(route_resolution resolution, request_body_mode body_mode) noexcept {
        if (selected_route_) {
            return false;
        }
        selected_route_.emplace(
            http2_sans_io_selected_route::token_type{}, std::move(resolution), body_mode, body_resource_);
        return true;
    }

    [[nodiscard]] http2_sans_io_selected_route* selected_route() & noexcept {
        return selected_route_ ? &*selected_route_ : nullptr;
    }
    http2_sans_io_selected_route* selected_route() && = delete;

    [[nodiscard]] const http2_sans_io_selected_route* selected_route() const& noexcept {
        return selected_route_ ? &*selected_route_ : nullptr;
    }
    const http2_sans_io_selected_route* selected_route() const&& = delete;

    [[nodiscard]] bool dispatched() const noexcept {
        const auto* selected = selected_route();
        return selected != nullptr && selected->dispatched();
    }

    [[nodiscard]] http2_sans_io_stream_signal* signal() noexcept {
        auto* selected = selected_route();
        return selected != nullptr ? selected->signal() : nullptr;
    }

    [[nodiscard]] const http2_sans_io_stream_signal* signal() const noexcept {
        const auto* selected = selected_route();
        return selected != nullptr ? selected->signal() : nullptr;
    }

private:
    friend class http2_sans_io_stream_runtime_table;

    [[nodiscard]] http2_sans_io_stream_signal* begin_dispatch(
        const worker_handle& worker_value, http2_sans_io_termination& termination) {
        auto* selected = selected_route();
        return selected != nullptr ? selected->begin_dispatch(worker_value, termination) : nullptr;
    }

    void mark_tunnel() noexcept {
        tunnel_ = true;
    }

    [[nodiscard]] bool tunnel() const noexcept {
        return tunnel_;
    }

    std::uint32_t stream_id_;
    std::uint32_t push_parent_{};
    std::pmr::memory_resource* resource_;
    std::pmr::memory_resource* body_resource_;
    http_request_trailers trailers_;
    std::optional<http_priority> priority_update_{};
    std::optional<http2_sans_io_selected_route> selected_route_;
    std::optional<http2_request_head_event> request_head_;
    bool tunnel_{false};
};

// Stable per-stream Web runtime storage. The common multiplexing case uses inline
// slots; overflow objects are PMR-owned and remain stable when the pointer vector
// compacts, so request body views and dispatch signal references cannot be invalidated
// by another stream being admitted or erased. dispatched_count_ is acquired before a
// handler is scheduled and released only when that same runtime is removed.
class http2_sans_io_stream_runtime_table final {
public:
    explicit http2_sans_io_stream_runtime_table(
        std::pmr::memory_resource* resource, http2_sans_io_termination& termination,
        std::pmr::memory_resource* body_resource = nullptr)
        : resource_(pmr_resource_or_default(resource)),
          body_resource_(body_resource != nullptr ? body_resource : resource_),
          termination_(termination),
          overflow_(resource_) {}

    [[nodiscard]] http2_sans_io_stream_runtime* find(std::uint32_t stream_id) noexcept {
        for (auto& slot : inline_) {
            if (slot && slot->stream_id() == stream_id) {
                return &*slot;
            }
        }
        for (auto& runtime : overflow_) {
            if (runtime != nullptr && runtime->stream_id() == stream_id) {
                return runtime.get();
            }
        }
        return nullptr;
    }

    // The dispatch signal of a stream that has one. A stream with no runtime,
    // or one admitted but not yet dispatched, has nothing to wake.
    [[nodiscard]] http2_sans_io_stream_signal* signal_for(std::uint32_t stream_id) noexcept {
        auto* runtime = find(stream_id);
        return runtime != nullptr ? runtime->signal() : nullptr;
    }

    [[nodiscard]] const http2_sans_io_stream_runtime* find(std::uint32_t stream_id) const noexcept {
        for (const auto& slot : inline_) {
            if (slot && slot->stream_id() == stream_id) {
                return &*slot;
            }
        }
        for (const auto& runtime : overflow_) {
            if (runtime != nullptr && runtime->stream_id() == stream_id) {
                return runtime.get();
            }
        }
        return nullptr;
    }

    // Protocol admission is already committed before Web attaches application
    // runtime state. The HTTP stream identifier is sufficient here; this table
    // does not retain or inspect protocol stream storage.
    [[nodiscard]] http2_sans_io_stream_runtime& ensure_accepted(std::uint32_t stream_id) {
        if (auto* existing = find(stream_id)) {
            return *existing;
        }
        for (auto& slot : inline_) {
            if (!slot) {
                slot.emplace(stream_id, resource_, body_resource_);
                ++size_;
                return *slot;
            }
        }
        auto runtime = make_pmr_object<http2_sans_io_stream_runtime>(resource_, stream_id, resource_, body_resource_);
        auto* result_value = runtime.get();
        overflow_.push_back(std::move(runtime));
        ++size_;
        return *result_value;
    }

    [[nodiscard]] http2_sans_io_stream_signal* begin_dispatch(
        std::uint32_t stream_id, const worker_handle& worker_value) {
        auto* runtime = find(stream_id);
        if (runtime == nullptr) {
            return nullptr;
        }
        auto* signal = runtime->begin_dispatch(worker_value, termination_);
        if (signal != nullptr) {
            ++dispatched_count_;
        }
        return signal;
    }
    http2_sans_io_stream_signal* begin_dispatch(std::uint32_t, worker_handle&&) = delete;

    [[nodiscard]] bool mark_tunnel(std::uint32_t stream_id) noexcept {
        auto* runtime = find(stream_id);
        if (runtime == nullptr || runtime->tunnel()) {
            return false;
        }
        runtime->mark_tunnel();
        ++tunnel_count_;
        return true;
    }

    [[nodiscard]] bool remove(std::uint32_t stream_id) noexcept {
        for (auto& slot : inline_) {
            if (slot && slot->stream_id() == stream_id) {
                account_removal(*slot);
                slot.reset();
                --size_;
                return true;
            }
        }
        for (std::size_t i = 0; i < overflow_.size(); ++i) {
            if (overflow_[i] == nullptr || overflow_[i]->stream_id() != stream_id) {
                continue;
            }
            account_removal(*overflow_[i]);
            if (i + 1 != overflow_.size()) {
                overflow_[i] = std::move(overflow_.back());
            }
            overflow_.pop_back();
            --size_;
            return true;
        }
        return false;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::size_t dispatched_count() const noexcept {
        return dispatched_count_;
    }

    [[nodiscard]] std::size_t tunnel_count() const noexcept {
        return tunnel_count_;
    }

    template <typename callback>
    void for_each(callback&& callback_value) {
        for (auto& slot : inline_) {
            if (slot) {
                callback_value(*slot);
            }
        }
        for (auto& runtime : overflow_) {
            if (runtime != nullptr) {
                callback_value(*runtime);
            }
        }
    }

private:
    // Mirrors http2_stream_table::inline_capacity: two inline slots for the
    // typical cadence, pmr overflow for deeper multiplexing, so the table's
    // resident footprint stays small in every connection.
    static constexpr std::size_t inline_capacity = 2;
    using overflow_runtime_type =
        std::unique_ptr<http2_sans_io_stream_runtime, pmr_object_deleter<http2_sans_io_stream_runtime>>;

    void account_removal(const http2_sans_io_stream_runtime& runtime) noexcept {
        if (runtime.dispatched()) {
            --dispatched_count_;
        }
        if (runtime.tunnel()) {
            --tunnel_count_;
        }
    }

    std::pmr::memory_resource* resource_;
    std::pmr::memory_resource* body_resource_;
    http2_sans_io_termination& termination_;
    std::array<std::optional<http2_sans_io_stream_runtime>, inline_capacity> inline_{};
    std::pmr::vector<overflow_runtime_type> overflow_;
    std::size_t size_{0};
    std::size_t dispatched_count_{0};
    std::size_t tunnel_count_{0};
};

}  // namespace ruvia::detail
