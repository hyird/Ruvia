#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/entity_rows.h"

namespace ruvia {

namespace detail {
struct request_io_metrics;

// A database stream or transaction owns one pool lease. Cold guards only
// register lifetime obligations that forbid destroying their owner; they do
// not reserve the operation right. Operating is the sole state that grants
// that right. Closed and Failed are terminal states so a failed lease can
// never be reused accidentally.
template <typename payload>
class db_operation_guard;

template <typename payload>
class db_operation_state final {
public:
    struct closed_type final {};
    struct failed_type final {};

    struct active_type final {
        payload payload_;
    };

    struct operating_type final {
        payload payload_;
    };

    db_operation_state() noexcept = default;

    explicit db_operation_state(payload payload_value) noexcept
        : state_(std::in_place_type<active_type>, std::move(payload_value)) {
        static_assert(std::is_nothrow_move_constructible_v<payload>);
    }

    db_operation_state(const db_operation_state&) = delete;
    db_operation_state& operator=(const db_operation_state&) = delete;
    ~db_operation_state() {
        reset();
    }

    db_operation_state(db_operation_state&&) = delete;
    db_operation_state& operator=(db_operation_state&&) = delete;

    [[nodiscard]] bool active() const noexcept {
        return std::holds_alternative<active_type>(state_);
    }

    [[nodiscard]] bool finished() const noexcept {
        return std::holds_alternative<closed_type>(state_) || std::holds_alternative<failed_type>(state_);
    }
    // Preparation only borrows immutable connection and resource metadata. It
    // never grants the operation right, even while another task holds it.
    [[nodiscard]] const payload& preparation_payload() const {
        if (const auto* active = std::get_if<active_type>(&state_); active != nullptr) {
            return active->payload_;
        }
        if (const auto* operating = std::get_if<operating_type>(&state_); operating != nullptr) {
            return operating->payload_;
        }
        throw std::logic_error("database resource is not active");
    }

    void reset() noexcept {
        if (cold_borrows_ != 0 || std::holds_alternative<operating_type>(state_)) {
            // Destroying the database owner while any operation borrows it would
            // leave that coroutine with a dangling owner, even before start().
            std::terminate();
        }
        if (auto* active = std::get_if<active_type>(&state_); active != nullptr) {
            active->payload_.abort();
        }
        state_.template emplace<closed_type>();
    }

private:
    friend class db_operation_guard<payload>;

    void register_cold_borrow() noexcept {
        ++cold_borrows_;
    }

    void start() {
        auto* active = std::get_if<active_type>(&state_);
        if (active == nullptr) {
            throw std::logic_error(std::holds_alternative<operating_type>(state_)
                                       ? "database operation is already in progress"
                                       : "database resource is not active");
        }
        if (cold_borrows_ == 0) {
            std::terminate();
        }
        --cold_borrows_;
        payload payload_value(std::move(active->payload_));
        state_.template emplace<operating_type>(std::move(payload_value));
    }

    void release_cold_borrow() noexcept {
        if (cold_borrows_ == 0) {
            std::terminate();
        }
        --cold_borrows_;
    }

    [[nodiscard]] payload& operation_payload() noexcept {
        auto* operating = std::get_if<operating_type>(&state_);
        if (operating == nullptr) {
            std::terminate();
        }
        return operating->payload_;
    }

    void finish_active() noexcept {
        if (!std::holds_alternative<operating_type>(state_)) {
            std::terminate();
        }
        auto& operating = std::get<operating_type>(state_);
        payload payload_value(std::move(operating.payload_));
        state_.template emplace<active_type>(std::move(payload_value));
    }

    void finish_closed() noexcept {
        if (!std::holds_alternative<operating_type>(state_)) {
            std::terminate();
        }
        state_.template emplace<closed_type>();
    }

    void finish_failed() noexcept {
        if (!std::holds_alternative<operating_type>(state_)) {
            std::terminate();
        }
        state_.template emplace<failed_type>();
    }

    std::variant<closed_type, active_type, operating_type, failed_type> state_{};
    std::size_t cold_borrows_{0};
};

// One operation on a db_operation_state. Construction only registers a cold
// lifetime borrow. start() acquires the exclusive lease when the coroutine
// actually begins. Dropping a cold guard unregisters only its own borrow;
// destroying a started guard without naming an ending fails the lease.
template <typename payload>
class db_operation_guard final {
public:
    using state_type = db_operation_state<payload>;

    explicit db_operation_guard(state_type& state_value) noexcept
        : state_(&state_value) {
        state_->register_cold_borrow();
    }

    db_operation_guard(const db_operation_guard&) = delete;
    db_operation_guard& operator=(const db_operation_guard&) = delete;
    db_operation_guard(db_operation_guard&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)),
          started_(std::exchange(other.started_, false)) {}
    db_operation_guard& operator=(db_operation_guard&&) = delete;

    ~db_operation_guard() {
        if (state_ != nullptr) {
            if (started_) {
                state_->finish_failed();
            } else {
                state_->release_cold_borrow();
            }
        }
    }

    void start() {
        state_->start();
        started_ = true;
    }

    [[nodiscard]] bool finished() const noexcept {
        return state_ != nullptr && state_->finished();
    }
    [[nodiscard]] payload& lease() noexcept {
        if (state_ == nullptr || !started_) {
            std::terminate();
        }
        return state_->operation_payload();
    }

    void finish_active() noexcept {
        state_->finish_active();
        state_ = nullptr;
    }

    void finish_closed() noexcept {
        state_->finish_closed();
        state_ = nullptr;
    }

    void finish_failed() noexcept {
        state_->finish_failed();
        state_ = nullptr;
    }

private:
    state_type* state_;
    bool started_{false};
};

}  // namespace detail

// Result of a statement whose contract is side effects rather than a row set.
// PostgreSQL does not expose a portable connection-level insert id, so that
// value is present only when the selected backend supplied one.
class db_exec_result final {
public:
    [[nodiscard]] constexpr std::uint64_t affected_rows() const noexcept {
        return affected_rows_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> last_insert_id() const noexcept {
        return last_insert_id_;
    }

private:
    friend struct detail::db_result_access;

    constexpr db_exec_result(
        std::uint64_t affected_rows, std::optional<std::uint64_t> last_insert_id) noexcept
        : affected_rows_(affected_rows),
          last_insert_id_(last_insert_id) {}

    std::uint64_t affected_rows_{0};
    std::optional<std::uint64_t> last_insert_id_;
};

class db_rows final {
public:
    db_rows(const db_rows&) = delete;
    db_rows& operator=(const db_rows&) = delete;
    db_rows(db_rows&& other) noexcept;
    db_rows& operator=(db_rows&& other);
    ~db_rows();

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const db_row& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_row& operator[](std::size_t index) const&& = delete;
    [[nodiscard]] const db_row* begin() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_row* begin() const&& = delete;
    [[nodiscard]] const db_row* end() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_row* end() const&& = delete;
    [[nodiscard]] const db_row& front() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_row& front() const&& = delete;
    [[nodiscard]] const db_row& back() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_row& back() const&& = delete;
    [[nodiscard]] std::span<const std::pmr::string> column_names() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const std::pmr::string> column_names() const&& = delete;

private:
    friend struct detail::db_result_access;

    struct no_raw_result_type final {};

    struct owned_raw_result_type final {
        owned_raw_result_type(void* owned_value, void (*owned_release)(void*) noexcept) noexcept
            : value_(owned_value),
              release_(owned_release) {}

        void* value_;
        void (*release_)(void*) noexcept;
    };

    explicit db_rows(std::pmr::memory_resource* resource = nullptr);
    db_rows(resolved_pmr_resource_tag, std::pmr::memory_resource* resource);

    std::pmr::vector<db_row> rows_;
    std::pmr::vector<db_field> fields_;
    std::pmr::vector<std::pmr::string> column_names_;
    std::variant<no_raw_result_type, owned_raw_result_type> raw_result_;
};

class db_stream_result final {
public:
    db_stream_result(const db_stream_result&) = delete;
    db_stream_result& operator=(const db_stream_result&) = delete;
    // Operations borrow the address-stable state owned by this object, so a
    // move transfers the state without invalidating a cold or running frame.
    db_stream_result(db_stream_result&& other) noexcept;
    db_stream_result& operator=(db_stream_result&&) = delete;
    ~db_stream_result();

    [[nodiscard]] bool active() const noexcept;
    // Available when query_stream() completes, including empty row sets.
    // Names remain valid through reads, EOF and close, until this state dies.
    [[nodiscard]] std::span<const std::pmr::string> column_names() const& RUVIA_LIFETIMEBOUND;
    std::span<const std::pmr::string> column_names() const&& = delete;
    // Each read has one operation deadline, including any EOF/trailing-result
    // drain needed before returning nullopt. A separate close starts its own
    // deadline. Both end no later than the stream's overall deadline
    // (db_config::transaction_timeout_), which runs from query_stream() until
    // the stream ends and also counts the time between operations; once it has
    // passed, the connection is closed and read() and close() fail with
    // db_error::code_type::timeout.
    // `operation` refines the handle's options for this call only: its timeout
    // may only tighten, and a supplied token replaces the inherited one. A
    // non-positive timeout throws std::invalid_argument at the call; an
    // already-stopped token throws db_error(cancelled) and leaves the stream
    // usable. Once started, a timeout or stop request closes the connection --
    // a pooled connection interrupted mid-result is never reused -- and ends
    // the stream with db_error timeout / cancelled.
    [[nodiscard]] task<std::optional<db_row>> read(operation_options operation = {}) &;
    task<std::optional<db_row>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<void> close(operation_options operation = {}) &;
    task<void> close(operation_options = {}) && = delete;

private:
    friend class db_handle;
    template <typename driver_type>
    friend class detail::db_pool;

    // The driver's result handle lives in the pool slot, so the pool alone
    // frees it, including when the stream's deadline expires between reads.
    struct lease_type final {
        lease_type(detail::db_pool_ref_type client, std::size_t slot,
            std::pmr::memory_resource* resource, operation_options options) noexcept;
        void abort() noexcept;

        detail::db_pool_ref_type client_;
        std::size_t slot_;
        std::pmr::memory_resource* resource_;
        std::pmr::vector<db_field> fields_;
        operation_options options_;
    };

    using operation_state_type = detail::db_operation_state<lease_type>;
    using operation_guard_type = detail::db_operation_guard<lease_type>;

    struct state_type final {
        state_type(lease_type lease, std::pmr::vector<std::pmr::string> column_names) noexcept;
        std::pmr::vector<std::pmr::string> column_names_;
        operation_state_type operation_;
    };
    using state_owner_type = std::unique_ptr<state_type, pmr_object_deleter<state_type>>;

    db_stream_result() noexcept = default;
    db_stream_result(detail::db_pool_ref_type client, std::size_t slot, std::pmr::vector<std::pmr::string> column_names,
        std::pmr::memory_resource* resource, operation_options options);
    void reset() noexcept;
    // `io_metrics` is the request's counter block, or null outside a request.
    void bind_operation_scope(::ruvia::operation_scope& scope, detail::request_io_metrics* io_metrics) noexcept;
    static void expire_capability(void* target) noexcept;
    static task<std::optional<db_row>> read_task(detail::request_io_metrics* io_metrics,
        operation_guard_type operation, std::pmr::vector<std::pmr::string>& column_names, operation_options operation_value);
    static task<void> close_task(detail::request_io_metrics* io_metrics, operation_guard_type operation, operation_options operation_value);

    state_owner_type state_;
    detail::request_io_metrics* io_metrics_{nullptr};
    scoped_capability_registration registration_;
};

namespace detail {

// The public result objects stay backend-neutral. Concrete drivers populate
// them through this single internal access point instead of accumulating one
// friend declaration per driver.
struct db_result_access final {
    template <typename entity_type>
    [[nodiscard]] static entity_rows<entity_type> make_entity_rows(std::pmr::memory_resource* resource, std::size_t size) {
        entity_rows<entity_type> result(resource);
        result.reserve(size);
        return result;
    }

    [[nodiscard]] static db_rows make_result(std::pmr::memory_resource* resource) {
        return db_rows(resource);
    }

    [[nodiscard]] static constexpr db_exec_result make_exec_result(std::uint64_t affected_rows,
        std::optional<std::uint64_t> last_insert_id = std::nullopt) noexcept {
        return db_exec_result(affected_rows, last_insert_id);
    }

    [[nodiscard]] static std::pmr::vector<db_row>& rows(db_rows& result_value) noexcept {
        return result_value.rows_;
    }

    [[nodiscard]] static std::pmr::vector<db_field>& fields(db_rows& result_value) noexcept {
        return result_value.fields_;
    }

    [[nodiscard]] static std::pmr::vector<std::pmr::string>& column_names(db_rows& result_value) noexcept {
        return result_value.column_names_;
    }

    // Leaves a result whose rows could not be kept empty, so a failed
    // materialization never hands out part of a result.
    static void clear(db_rows& result_value) noexcept {
        result_value.rows_.clear();
        result_value.fields_.clear();
        result_value.column_names_.clear();
    }

    static void own_raw_result(db_rows& result_value, void* raw, void (*release)(void*) noexcept) noexcept {
        if (raw == nullptr || release == nullptr ||
            std::holds_alternative<db_rows::owned_raw_result_type>(result_value.raw_result_)) {
            std::terminate();
        }
        result_value.raw_result_.template emplace<db_rows::owned_raw_result_type>(raw, release);
    }

    [[nodiscard]] static db_field null_field(std::pmr::memory_resource* resource) {
        return db_field(nullptr, resource);
    }

    [[nodiscard]] static db_field owned_field(
        std::string_view value, std::pmr::memory_resource* resource) {
        return db_field(value, resource);
    }

    [[nodiscard]] static db_field borrowed_field(
        std::string_view value, std::pmr::memory_resource* resource) {
        return db_field::borrowed(value, resource);
    }

    [[nodiscard]] static db_row borrowed_row(const db_field* fields_value, std::size_t size,
        const std::pmr::string* column_names, std::size_t column_count) noexcept {
        return db_row(fields_value, size, column_names, column_count);
    }

    static void reserve_result(db_rows& result_value, std::size_t row_count, std::size_t field_count) {
        result_value.rows_.reserve(row_count);
        result_value.fields_.reserve(row_count * field_count);
        result_value.column_names_.reserve(field_count);
    }

    static void append_row(db_rows& result_value, std::size_t row_start, std::size_t field_count) {
        result_value.rows_.push_back(borrowed_row(result_value.fields_.data() + row_start, field_count,
            result_value.column_names_.data(), result_value.column_names_.size()));
    }

    template <typename field_at_type>
    [[nodiscard]] static db_row materialize_stream_row(std::pmr::vector<db_field>& fields_value,
        const std::pmr::vector<std::pmr::string>& column_names, std::size_t field_count,
        std::pmr::memory_resource* resource, field_at_type&& field_at) {
        fields_value.clear();
        fields_value.reserve(field_count);
        for (std::size_t field = 0; field < field_count; ++field) {
            if (const auto value = field_at(field); value.has_value()) {
                fields_value.push_back(owned_field(*value, resource));
            } else {
                fields_value.push_back(null_field(resource));
            }
        }
        return borrowed_row(fields_value.data(), fields_value.size(), column_names.data(), column_names.size());
    }
};

}  // namespace detail

}  // namespace ruvia
