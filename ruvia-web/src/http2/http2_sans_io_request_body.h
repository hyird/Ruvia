#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/pmr_string.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_request_body_failure.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/web/detail/router/route_modes.h"

#include "http2/http2_sans_io_stream_signal.h"
#include "server/inbound_buffer_resource.h"

// Where a dispatched HTTP/2 request body lives on the Web side. The protocol
// core emits ordered DATA events without knowing whether the route buffers or
// streams them; everything here applies product limits after route resolution,
// and keeps an HTTP content failure distinguishable from local backlog
// exhaustion because the two need different runtime policy.

namespace ruvia::detail {

// Web-owned storage for body/tunnel bytes that have already crossed the
// http2_connection event boundary. The HTTP/2 core owns framing and flow-control
// debt; this queue owns only runtime buffering for a suspended route handler.
// Pending DATA is concatenated so each frame is copied once, not into a new
// string per frame. pop() swaps the pending buffer aside so enqueue cannot
// invalidate the borrowed view.
class http2_sans_io_body_queue final {
public:
    explicit http2_sans_io_body_queue(std::pmr::memory_resource* resource = nullptr)
        : queued_chunk_(pmr_resource_or_default(resource)),
          active_chunk_(pmr_resource_or_default(resource)) {}

    void enqueue(std::string_view data) {
        if (data.empty()) {
            return;
        }
        const auto before = queued_chunk_.size();
        queued_chunk_.append(data.data(), data.size());
        queued_bytes_ += queued_chunk_.size() - before;
    }

    void enqueue(std::string_view data, http2_received_data_credit&& credit) {
        if (data.empty()) {
            return;
        }
        const auto before = queued_chunk_.size();
        // Commit byte accounting only after append succeeds. The incoming
        // credit is still untouched if allocation throws, so its owner can
        // safely return it during unwinding.
        queued_chunk_.append(data.data(), data.size());
        queued_bytes_ += queued_chunk_.size() - before;
        if (!credit.valid()) {
            return;
        }
        if (!queued_credit_.has_value()) {
            queued_credit_.emplace(std::move(credit));
            return;
        }
        if (queued_credit_->merge(std::move(credit)) !=
            http2_received_data_credit_merge_status::merged) {
            std::terminate();
        }
    }

    [[nodiscard]] bool enqueue_bounded(std::string_view data,
        http2_received_data_credit&& credit, std::size_t backlog_limit) {
        if (queued_bytes_ > backlog_limit || data.size() > backlog_limit - queued_bytes_) {
            return false;
        }
        try {
            enqueue(data, std::move(credit));
        } catch (const inbound_buffer_limit_error&) {
            return false;
        }
        return true;
    }

    // Release credits only after the consumer has copied/finished the active view.
    void release_active_credits() noexcept {
        active_credit_.reset();
    }

    [[nodiscard]] bool empty() const noexcept {
        return queued_chunk_.empty();
    }

    [[nodiscard]] std::size_t queued_bytes() const noexcept {
        return queued_bytes_;
    }

    // The returned view remains valid until the next pop().
    [[nodiscard]] std::string_view pop() & {
        ::ruvia::clear_pmr_string_retaining_small(active_chunk_);
        release_active_credits();
        if (queued_chunk_.empty()) {
            return {};
        }
        active_chunk_.swap(queued_chunk_);
        if (queued_credit_.has_value()) {
            active_credit_.emplace(std::move(*queued_credit_));
            queued_credit_.reset();
        }
        queued_bytes_ = 0;
        ::ruvia::clear_pmr_string_retaining_small(queued_chunk_);
        return std::string_view(active_chunk_);
    }
    std::string_view pop() && = delete;

private:
    // The byte storage is destroyed before its credits, so return-window side
    // effects cannot outlive the copied data they account for.
    std::optional<http2_received_data_credit> queued_credit_;
    std::optional<http2_received_data_credit> active_credit_;
    std::pmr::string queued_chunk_;
    std::pmr::string active_chunk_;
    std::size_t queued_bytes_{0};
};

class http2_buffered_request_body;
class http2_streaming_request_body;

class http2_request_body_stored final {
private:
    friend class http2_request_body_store_result;
    constexpr http2_request_body_stored() noexcept = default;
};

class http2_request_body_backlog_overflow final {
private:
    friend class http2_request_body_store_result;
    constexpr http2_request_body_backlog_overflow() noexcept = default;
};

// A product-owned body store distinguishes an HTTP content failure from local
// streaming backlog exhaustion. They require different runtime policy and must
// never collapse into a generic non-accepted enum value.
class http2_request_body_store_result final {
public:
    [[nodiscard]] constexpr const http2_request_body_stored* stored() const& noexcept {
        return state_ == state_type::stored ? &value_.stored_ : nullptr;
    }
    const http2_request_body_stored* stored() const&& = delete;

    [[nodiscard]] constexpr const http_request_body_failure* protocol_failure() const& noexcept {
        return state_ == state_type::protocol_failure ? &value_.protocol_failure_ : nullptr;
    }
    const http_request_body_failure* protocol_failure() const&& = delete;

    [[nodiscard]] constexpr const http2_request_body_backlog_overflow* backlog_overflow()
        const& noexcept {
        return state_ == state_type::backlog_overflow ? &value_.backlog_overflow_ : nullptr;
    }
    const http2_request_body_backlog_overflow* backlog_overflow() const&& = delete;

private:
    friend class http2_buffered_request_body;
    friend class http2_streaming_request_body;

    enum class state_type : std::uint8_t { stored,
        protocol_failure,
        backlog_overflow };

    union value {
        constexpr explicit value(http2_request_body_stored value) noexcept
            : stored_(value) {}
        constexpr explicit value(http_request_body_failure value) noexcept
            : protocol_failure_(value) {}
        constexpr explicit value(http2_request_body_backlog_overflow value) noexcept
            : backlog_overflow_(value) {}

        http2_request_body_stored stored_;
        http_request_body_failure protocol_failure_;
        http2_request_body_backlog_overflow backlog_overflow_;
    };

    explicit constexpr http2_request_body_store_result(http2_request_body_stored value) noexcept
        : value_(value),
          state_(state_type::stored) {}
    explicit constexpr http2_request_body_store_result(http_request_body_failure value) noexcept
        : value_(value),
          state_(state_type::protocol_failure) {}
    explicit constexpr http2_request_body_store_result(http2_request_body_backlog_overflow value) noexcept
        : value_(value),
          state_(state_type::backlog_overflow) {}

    [[nodiscard]] static constexpr http2_request_body_store_result make_stored() noexcept {
        return http2_request_body_store_result(http2_request_body_stored());
    }

    [[nodiscard]] static constexpr http2_request_body_store_result make_protocol_failure(
        http_request_body_failure failure) noexcept {
        return http2_request_body_store_result(failure);
    }

    [[nodiscard]] static constexpr http2_request_body_store_result make_backlog_overflow() noexcept {
        return http2_request_body_store_result(http2_request_body_backlog_overflow());
    }

    value value_;
    state_type state_;
};

static_assert(std::is_trivially_copyable_v<http2_request_body_store_result>);
static_assert(sizeof(http2_request_body_store_result) <= 2);

class http2_buffered_request_body final {
public:
    explicit http2_buffered_request_body(std::pmr::memory_resource* resource) noexcept
        : bytes_(pmr_resource_or_default(resource)) {}

    [[nodiscard]] http2_request_body_store_result store(
        std::string_view data, protocol_byte_limit total_limit) {
        if (const auto failure =
                http_request_body_addition_failure(received_bytes_, data.size(), total_limit)) {
            return http2_request_body_store_result::make_protocol_failure(*failure);
        }
        try {
            if (!data.empty()) {
                bytes_.append(data.data(), data.size());
            }
        } catch (const inbound_buffer_limit_error&) {
            return http2_request_body_store_result::make_backlog_overflow();
        }
        received_bytes_ += data.size();
        return http2_request_body_store_result::make_stored();
    }

    [[nodiscard]] std::size_t received_bytes() const noexcept {
        return received_bytes_;
    }

    [[nodiscard]] std::string_view bytes() const& noexcept {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

private:
    std::size_t received_bytes_{0};
    std::pmr::string bytes_;
};

class http2_streaming_request_body final {
public:
    explicit http2_streaming_request_body(std::pmr::memory_resource* resource) noexcept
        : queue_(pmr_resource_or_default(resource)) {}

    [[nodiscard]] http2_request_body_store_result store(
        std::string_view data, protocol_byte_limit total_limit, std::size_t backlog_limit) {
        return store_impl(data, total_limit, backlog_limit, nullptr);
    }

    [[nodiscard]] http2_request_body_store_result store(std::string_view data,
        protocol_byte_limit total_limit, std::size_t backlog_limit,
        http2_received_data_credit&& credit) {
        return store_impl(data, total_limit, backlog_limit, &credit);
    }

    [[nodiscard]] std::size_t received_bytes() const noexcept {
        return received_bytes_;
    }

    [[nodiscard]] http2_sans_io_body_queue& queue() & noexcept {
        return queue_;
    }
    http2_sans_io_body_queue& queue() && = delete;

    [[nodiscard]] const http2_sans_io_body_queue& queue() const& noexcept {
        return queue_;
    }
    const http2_sans_io_body_queue& queue() const&& = delete;

private:
    [[nodiscard]] http2_request_body_store_result store_impl(std::string_view data,
        protocol_byte_limit total_limit, std::size_t backlog_limit,
        http2_received_data_credit* credit) {
        if (const auto failure =
                http_request_body_addition_failure(received_bytes_, data.size(), total_limit)) {
            return http2_request_body_store_result::make_protocol_failure(*failure);
        }
        if (queue_.queued_bytes() > backlog_limit ||
            data.size() > backlog_limit - queue_.queued_bytes()) {
            return http2_request_body_store_result::make_backlog_overflow();
        }
        try {
            if (credit != nullptr) {
                queue_.enqueue(data, std::move(*credit));
            } else {
                queue_.enqueue(data);
            }
        } catch (const inbound_buffer_limit_error&) {
            return http2_request_body_store_result::make_backlog_overflow();
        }
        received_bytes_ += data.size();
        return http2_request_body_store_result::make_stored();
    }

    std::size_t received_bytes_{0};
    http2_sans_io_body_queue queue_;
};

// Route-selected request-body storage belongs to ruvia-web. The protocol core
// emits ordered DATA events without knowing whether an application buffers or
// streams them; this runtime applies product limits only after route resolution.
class http2_request_body_runtime final {
public:
    [[nodiscard]] request_body_mode mode() const noexcept {
        return std::holds_alternative<http2_buffered_request_body>(storage_)
                   ? request_body_mode::buffered
                   : request_body_mode::stream;
    }

    [[nodiscard]] http2_buffered_request_body* buffered() & noexcept {
        return std::get_if<http2_buffered_request_body>(&storage_);
    }
    http2_buffered_request_body* buffered() && = delete;

    [[nodiscard]] const http2_buffered_request_body* buffered() const& noexcept {
        return std::get_if<http2_buffered_request_body>(&storage_);
    }
    const http2_buffered_request_body* buffered() const&& = delete;

    [[nodiscard]] http2_streaming_request_body* streaming() & noexcept {
        return std::get_if<http2_streaming_request_body>(&storage_);
    }
    http2_streaming_request_body* streaming() && = delete;

    [[nodiscard]] const http2_streaming_request_body* streaming() const& noexcept {
        return std::get_if<http2_streaming_request_body>(&storage_);
    }
    const http2_streaming_request_body* streaming() const&& = delete;

    [[nodiscard]] http2_request_body_store_result store(
        std::string_view data, protocol_byte_limit total_limit, std::size_t streaming_backlog_limit) {
        if (auto* value = buffered()) {
            return value->store(data, total_limit);
        }
        return std::get<http2_streaming_request_body>(storage_).store(
            data, total_limit, streaming_backlog_limit);
    }

    [[nodiscard]] http2_request_body_store_result store(std::string_view data,
        protocol_byte_limit total_limit, std::size_t streaming_backlog_limit,
        http2_received_data_credit&& credit) {
        if (auto* value = buffered()) {
            return value->store(data, total_limit);
        }
        return std::get<http2_streaming_request_body>(storage_).store(
            data, total_limit, streaming_backlog_limit, std::move(credit));
    }

    [[nodiscard]] std::size_t received_bytes() const noexcept {
        if (const auto* value = buffered()) {
            return value->received_bytes();
        }
        return std::get<http2_streaming_request_body>(storage_).received_bytes();
    }

private:
    friend class http2_sans_io_selected_route;

    using storage_type = std::variant<http2_buffered_request_body, http2_streaming_request_body>;

    http2_request_body_runtime(request_body_mode mode, std::pmr::memory_resource* resource) noexcept
        : storage_(make_storage(mode, resource)) {}

    [[nodiscard]] static storage_type make_storage(
        request_body_mode mode, std::pmr::memory_resource* resource) noexcept {
        if (mode == request_body_mode::buffered) {
            return storage_type(std::in_place_type<http2_buffered_request_body>, resource);
        }
        if (mode == request_body_mode::stream) {
            return storage_type(std::in_place_type<http2_streaming_request_body>, resource);
        }
        std::terminate();
    }

    storage_type storage_;
};

}  // namespace ruvia::detail
