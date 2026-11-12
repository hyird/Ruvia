#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_request_trailers.h"

#include "http2/http2_sans_io_request_body.h"
#include "router/route_resolution.h"
#include "server/multiplexed_stream_signal.h"

namespace ruvia::detail {

class http2_sans_io_stream_runtime final {
public:
    // The owning table resolves metadata and body resources before construction.
    http2_sans_io_stream_runtime(std::uint32_t stream_id, std::pmr::memory_resource* resource,
        std::pmr::memory_resource* body_resource)
        : stream_id_(stream_id),
          trailers_(resource),
          buffered_body_(body_resource),
          body_queue_(body_resource) {}

    void bind_push_parent(std::uint32_t id) noexcept {
        push_parent_ = id;
    }
    [[nodiscard]] std::uint32_t push_parent() const noexcept {
        return push_parent_;
    }
    [[nodiscard]] http_request_trailers& trailers() & noexcept RUVIA_LIFETIMEBOUND {
        return trailers_;
    }
    http_request_trailers& trailers() && = delete;
    [[nodiscard]] const std::optional<http_priority>& priority_update() const& noexcept RUVIA_LIFETIMEBOUND {
        return priority_update_;
    }
    const std::optional<http_priority>& priority_update() const&& = delete;
    void reprioritize(http_priority priority) noexcept {
        priority_update_ = priority;
    }
    void mark_body_failure(http_request_body_failure failure) noexcept {
        if (!body_failure_.has_value()) {
            body_failure_ = failure;
        }
    }
    [[nodiscard]] const std::optional<http_request_body_failure>& body_failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_failure_;
    }
    const std::optional<http_request_body_failure>& body_failure() const&& = delete;
    void mark_response_committed() noexcept {
        response_committed_ = true;
    }
    [[nodiscard]] bool response_committed() const noexcept {
        return response_committed_;
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

    [[nodiscard]] http2_request_head_event* request_head() & noexcept RUVIA_LIFETIMEBOUND {
        return request_head_ ? &*request_head_ : nullptr;
    }
    http2_request_head_event* request_head() && = delete;

    [[nodiscard]] bool select_route(route_resolution resolution, request_body_mode body_mode) noexcept {
        if (route_selected_) {
            return false;
        }
        resolution_ = std::move(resolution);
        body_mode_ = body_mode;
        route_selected_ = true;
        return true;
    }

    [[nodiscard]] bool route_selected() const noexcept {
        return route_selected_;
    }

    [[nodiscard]] const route_resolution& resolution() const& noexcept RUVIA_LIFETIMEBOUND {
        return resolution_;
    }
    const route_resolution& resolution() const&& = delete;

    [[nodiscard]] request_body_mode body_mode() const noexcept {
        return body_mode_;
    }

    [[nodiscard]] http2_request_body_store_status store_body(
        http2_message_body_chunk_event& chunk, protocol_byte_limit total_limit, std::size_t backlog_limit) {
        const auto data = chunk.bytes();
        if (http_request_body_addition_failure(received_bytes_, data.size(), total_limit).has_value()) {
            return http2_request_body_store_status::too_large;
        }
        if (body_mode_ == request_body_mode::stream) {
            if (!body_queue_.enqueue_bounded(data, chunk.take_credit(), backlog_limit)) {
                return http2_request_body_store_status::backlog_overflow;
            }
        } else {
            try {
                if (!data.empty()) {
                    buffered_body_.append(data.data(), data.size());
                }
            } catch (const inbound_buffer_limit_error&) {
                return http2_request_body_store_status::backlog_overflow;
            }
        }
        received_bytes_ += data.size();
        return http2_request_body_store_status::stored;
    }

    [[nodiscard]] std::string_view buffered_body() const& noexcept RUVIA_LIFETIMEBOUND {
        return buffered_body_;
    }
    std::string_view buffered_body() const&& = delete;

    [[nodiscard]] http2_sans_io_body_queue& body_queue() & noexcept RUVIA_LIFETIMEBOUND {
        return body_queue_;
    }
    http2_sans_io_body_queue& body_queue() && = delete;

    [[nodiscard]] const http2_sans_io_body_queue& body_queue() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_queue_;
    }
    const http2_sans_io_body_queue& body_queue() const&& = delete;

    void discard_body() noexcept {
        std::pmr::string(buffered_body_.get_allocator()).swap(buffered_body_);
        body_queue_.discard();
    }

    [[nodiscard]] bool dispatched() const noexcept {
        return signal_.has_value();
    }

    [[nodiscard]] multiplexed_stream_signal* signal() & noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    multiplexed_stream_signal* signal() && = delete;

    [[nodiscard]] const multiplexed_stream_signal* signal() const& noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    const multiplexed_stream_signal* signal() const&& = delete;

    // Request-body DATA arrived at `now_ms` (the time of the read carrying it).
    void note_request_body_progress(std::int64_t now_ms) noexcept {
        request_body_deadline_.progress(now_ms);
    }

    // The request body is still open and has gone `inactivity` without DATA,
    // or `completion` since it opened (the stream's request_body_timeout_ and
    // body_completion_timeout_).
    [[nodiscard]] bool request_body_overdue(std::int64_t now_ms,
        const std::optional<timeout_duration>& inactivity,
        const std::optional<timeout_duration>& completion) const noexcept {
        return request_body_deadline_.overdue(now_ms, inactivity, completion);
    }

private:
    friend class http2_sans_io_stream_runtime_table;

    [[nodiscard]] multiplexed_stream_signal* begin_dispatch(
        const worker_handle& worker_value, multiplexed_session_termination& termination) {
        if (!route_selected_ || signal_) {
            return nullptr;
        }
        signal_.emplace(worker_value, termination);
        return &*signal_;
    }

    void mark_tunnel() noexcept {
        tunnel_ = true;
    }

    [[nodiscard]] bool tunnel() const noexcept {
        return tunnel_;
    }

    void mark_request_body_open(std::int64_t now_ms) noexcept {
        request_body_deadline_.open(now_ms);
    }

    void mark_request_body_closed() noexcept {
        request_body_deadline_.close();
    }

    std::uint32_t stream_id_;
    std::uint32_t push_parent_{};
    http_request_trailers trailers_;
    std::optional<http_priority> priority_update_{};
    std::optional<http2_request_head_event> request_head_;
    route_resolution resolution_{};
    request_body_mode body_mode_{request_body_mode::buffered};
    std::pmr::string buffered_body_;
    http2_sans_io_body_queue body_queue_;
    std::size_t received_bytes_{0};
    std::optional<multiplexed_stream_signal> signal_;
    std::optional<http_request_body_failure> body_failure_{};
    multiplexed_request_body_deadline request_body_deadline_;
    bool route_selected_{false};
    bool tunnel_{false};
    bool response_committed_{false};
};

// Stable per-stream Web runtime storage. The common multiplexing case uses inline
// slots; overflow objects are PMR-owned and remain stable when the pointer vector
// compacts, so request body views and dispatch signal references cannot be invalidated
// by another stream being admitted or erased.
class http2_sans_io_stream_runtime_table final {
public:
    explicit http2_sans_io_stream_runtime_table(
        std::pmr::memory_resource* resource, multiplexed_session_termination& termination,
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
    [[nodiscard]] multiplexed_stream_signal* signal_for(std::uint32_t stream_id) noexcept {
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

    [[nodiscard]] multiplexed_stream_signal* begin_dispatch(
        std::uint32_t stream_id, const worker_handle& worker_value) {
        auto* runtime = find(stream_id);
        if (runtime == nullptr) {
            return nullptr;
        }
        return runtime->begin_dispatch(worker_value, termination_);
    }
    multiplexed_stream_signal* begin_dispatch(std::uint32_t, worker_handle&&) = delete;

    [[nodiscard]] bool mark_tunnel(std::uint32_t stream_id) noexcept {
        auto* runtime = find(stream_id);
        if (runtime == nullptr || runtime->tunnel()) {
            return false;
        }
        runtime->mark_tunnel();
        ++tunnel_count_;
        return true;
    }

    // `now_ms` is the time of the read that carried the request head.
    void mark_request_body_open(std::uint32_t stream_id, std::int64_t now_ms) noexcept {
        if (auto* runtime = find(stream_id)) {
            runtime->mark_request_body_open(now_ms);
        }
    }

    void mark_request_body_closed(std::uint32_t stream_id) noexcept {
        if (auto* runtime = find(stream_id)) {
            runtime->mark_request_body_closed();
        }
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
        if (runtime.tunnel()) {
            --tunnel_count_;
        }
    }

    std::pmr::memory_resource* resource_;
    std::pmr::memory_resource* body_resource_;
    multiplexed_session_termination& termination_;
    std::array<std::optional<http2_sans_io_stream_runtime>, inline_capacity> inline_{};
    std::pmr::vector<overflow_runtime_type> overflow_;
    std::size_t size_{0};
    std::size_t tunnel_count_{0};
};

}  // namespace ruvia::detail
