#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <asio/associated_allocator.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/associated_executor.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/completion_condition.hpp>
#include <asio/detail/socket_ops.hpp>
#include <asio/error.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/ssl/error.hpp>
#include <asio/ssl/stream.hpp>

#include "blocking_pool.h"
#include "borrowed_text.h"
#include "connection_scanner.h"
#include "event_loop.h"
#include "http1_server_response.h"
#include "http_client_types.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response.h"
#include "http_server.h"
#include "middleware.h"
#include "stop_token.h"
#include "task.h"
#include "task_scope.h"
#include "worker_ref.h"
#include "worker_wait.h"

#if defined(_WIN32)
#include <winerror.h>
#endif
#if defined(__linux__)
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif
#if defined(_WIN32)
#include <mswsock.h>

#include <asio/windows/overlapped_ptr.hpp>
#endif

namespace ruvia::detail {

// A failed claim leaves the lane untouched; the caller supplies its domain error.
class operation_lane_lease final {
public:
    explicit operation_lane_lease(bool& active) noexcept :
        active_(active ? nullptr : &active) {
        if (active_ != nullptr) {
            *active_ = true;
        }
    }

    operation_lane_lease(const operation_lane_lease&) = delete;
    operation_lane_lease& operator=(const operation_lane_lease&) = delete;
    operation_lane_lease(operation_lane_lease&& other) noexcept :
        active_(std::exchange(other.active_, nullptr)) {}
    operation_lane_lease& operator=(operation_lane_lease&&) = delete;

    ~operation_lane_lease() noexcept {
        if (active_ != nullptr) {
            *active_ = false;
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return active_ != nullptr;
    }

private:
    bool* active_;
};

[[nodiscard]] inline operation_lane_lease claim_operation_lane(bool& active, const char* in_progress_message) {
    operation_lane_lease lease(active);
    if (!lease) {
        throw std::logic_error(in_progress_message);
    }
    return lease;
}

struct request_io_metrics final {
    std::uint64_t db_count_{0};
    std::uint64_t db_duration_micros_{0};
    std::uint64_t redis_count_{0};
    std::uint64_t redis_duration_micros_{0};
};

enum class request_io_kind : std::uint8_t {
    db = 0,
    redis = 1,
};

class scoped_request_io_timer final {
public:
    // `metrics` is the request's counter block, or null outside a request
    // (worker callbacks, standalone clients); null disables timing.
    scoped_request_io_timer(request_io_metrics* metrics, request_io_kind kind) noexcept :
        metrics_(metrics),
        kind_(kind),
        started_at_(metrics_ != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}

    ~scoped_request_io_timer() {
        if (metrics_ == nullptr) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started_at_).count();
        const auto micros = elapsed < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(elapsed);
        if (kind_ == request_io_kind::db) {
            ++metrics_->db_count_;
            metrics_->db_duration_micros_ += micros;
        } else {
            ++metrics_->redis_count_;
            metrics_->redis_duration_micros_ += micros;
        }
    }

    scoped_request_io_timer(const scoped_request_io_timer&) = delete;
    scoped_request_io_timer& operator=(const scoped_request_io_timer&) = delete;

private:
    request_io_metrics* metrics_{nullptr};
    request_io_kind kind_{request_io_kind::db};
    std::chrono::steady_clock::time_point started_at_{};
};

inline constexpr std::string_view default_capability_alias = "default";

inline void validate_capability_alias(std::string_view alias, const char* empty_message) {
    if (alias.empty()) {
        throw std::invalid_argument(empty_message);
    }
}

// Every named worker capability has the same startup representation: an alias
// plus validated, normalized configuration storage.
template <typename storage_type>
struct named_capability_definition final {
    using config_storage_type = storage_type;

    std::string alias_;
    storage_type config_;
};

// Replaces the definition registered under replacement.alias_ in place, so
// existing aliases preserve registration order, or appends a new alias.
template <typename definition_type>
void upsert_named_capability_definition(std::vector<definition_type>& definitions, definition_type&& replacement) {
    const std::string_view alias = replacement.alias_;
    for (auto& definition : definitions) {
        if (std::string_view(definition.alias_) == alias) {
            std::destroy_at(&definition);
            std::construct_at(&definition, std::move(replacement));
            return;
        }
    }
    definitions.push_back(std::move(replacement));
}

// Validates and normalizes the replacement before mutating retained application
// state; the new definition takes one owned copy of the name and configuration.
template <typename definition_type, typename source_config_type>
void upsert_named_capability_definition(std::vector<definition_type>& definitions, std::string_view alias, const source_config_type& source_config, const char* empty_message) {
    validate_capability_alias(alias, empty_message);
    upsert_named_capability_definition(definitions, definition_type{std::string(alias), typename definition_type::config_storage_type(source_config)});
}

// Validates the complete startup alias set without allocating. Capability
// owners call this before constructing pools, so an invalid later definition
// cannot leave a partially built worker capability graph behind.
template <typename entries_type>
void validate_capability_aliases(const entries_type& entries, const char* empty_message, const char* duplicate_message) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const std::string_view alias = entries[index].alias_;
        validate_capability_alias(alias, empty_message);
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (std::string_view(entries[previous].alias_) == alias) {
                throw std::invalid_argument(duplicate_message);
            }
        }
    }
}

// Immutable startup-built index shared by worker-local named capabilities.
// It owns one copy of each alias and binds it to the source registration
// position; source definitions need not outlive build(). Request-time lookup
// performs one allocation-free binary search.
class named_capability_index final {
public:
    named_capability_index() = default;
    void build(std::initializer_list<std::string_view> aliases) {
        build_aliases(aliases, [](std::string_view alias) noexcept {
            return alias;
        });
    }

    template <typename entries_type>
    void build(const entries_type& entries) {
        build_aliases(entries, [](const auto& entry_value) noexcept -> std::string_view {
            return entry_value.alias_;
        });
    }

    [[nodiscard]] std::optional<std::size_t> find(std::string_view alias) const noexcept {
        const auto match = std::ranges::lower_bound(aliases_, alias, {}, &named_capability_index::alias_view);
        if (match == aliases_.end() || match->alias_ != alias) {
            return std::nullopt;
        }
        return match->index_;
    }

    [[nodiscard]] std::optional<std::size_t> default_index() const noexcept {
        return default_index_;
    }

private:
    struct indexed_alias_type final {
        std::string alias_;
        std::size_t index_;
    };

    [[nodiscard]] static std::string_view alias_view(const indexed_alias_type& entry_value) noexcept {
        return entry_value.alias_;
    }

    template <typename entries_type, typename alias_projection_type>
    void build_aliases(const entries_type& entries, alias_projection_type alias_projection) {
        std::vector<indexed_alias_type> aliases;
        aliases.reserve(entries.size());
        std::optional<std::size_t> default_index;
        std::size_t index = 0;
        for (const auto& entry : entries) {
            const std::string_view alias = alias_projection(entry);
            aliases.push_back(indexed_alias_type{std::string(alias), index});
            if (alias == default_capability_alias) {
                default_index = index;
            }
            ++index;
        }
        std::ranges::sort(aliases, {}, &named_capability_index::alias_view);
        aliases_.swap(aliases);
        default_index_ = default_index;
    }

    std::vector<indexed_alias_type> aliases_;
    std::optional<std::size_t> default_index_;
};

// Receipts cover execution AND destruction of posted controls. Waiting never
// cancels early: borrowed command state cannot retire before every receipt.
class runtime_control_completion final {
    struct state final {
        explicit state(worker_ref caller) :
            caller_(std::move(caller)) {}
        ~state() {
            if (failure_ && !observed_) {
                report_unhandled_failure("runtime control", failure_);
            }
        }
        worker_ref caller_;
        std::mutex mutex_;
        std::exception_ptr failure_;
        std::coroutine_handle<> continuation_{};
        std::size_t completed_{};
        bool released_{};
        bool observed_{};
    };

public:
    template <typename function_type>
    class command;
    class receipt final {
    public:
        void complete(std::exception_ptr failure = {}) const noexcept {
            std::lock_guard lock(state_->mutex_);
            ++state_->completed_;
            if (!state_->failure_) {
                state_->failure_ = std::move(failure);
            }
        }
        template <typename function_type>
        [[nodiscard]] command<std::decay_t<function_type>> bind(function_type function) && {
            return command<std::decay_t<function_type>>(std::move(*this), std::move(function));
        }

    private:
        receipt(std::shared_ptr<state> state_value, std::shared_ptr<void> lifetime) noexcept :
            state_(std::move(state_value)),
            lifetime_(std::move(lifetime)) {}
        std::shared_ptr<state> state_;
        std::shared_ptr<void> lifetime_;
        friend class runtime_control_completion;
    };

    // Capture lists do not specify closure member declaration order. Keep the
    // receipt in a named first member so command payload destruction completes
    // before its final receipt can resume another worker.
    template <typename function_type>
    class command final {
    public:
        command(receipt completion, function_type function) :
            completion_(std::move(completion)),
            function_(std::move(function)) {}
        command(const command&) = delete;
        command& operator=(const command&) = delete;
        command(command&&) = default;
        command& operator=(command&&) = delete;
        template <typename... args_type>
            requires std::invocable<function_type&, const receipt&, args_type...>
        decltype(auto) operator()(args_type&&... args) {
            return std::invoke(function_, std::as_const(completion_), std::forward<args_type>(args)...);
        }

    private:
        receipt completion_;
        function_type function_;
    };

    class wait_operation final {
    public:
        wait_operation(const wait_operation&) = delete;
        wait_operation& operator=(const wait_operation&) = delete;
        void operator&() const = delete;
        ~wait_operation() {
            if (started_ && !resumed_) {
                std::terminate();
            }
        }
        bool await_ready() {
            if (!state_->caller_.is_current()) {
                throw std::logic_error("runtime control completion must be awaited on its caller worker");
            }
            std::lock_guard lock(state_->mutex_);
            started_ = true;
            return state_->released_;
        }
        bool await_suspend(std::coroutine_handle<> continuation) {
            std::lock_guard lock(state_->mutex_);
            if (state_->released_) {
                return false;
            }
            if (state_->continuation_) {
                std::terminate();
            }
            state_->continuation_ = continuation;
            return true;
        }
        void await_resume() noexcept {
            resumed_ = true;
        }

    private:
        explicit wait_operation(std::shared_ptr<state> value) noexcept :
            state_(std::move(value)) {}
        std::shared_ptr<state> state_;
        bool started_{};
        bool resumed_{};
        friend class runtime_control_completion;
    };
    explicit runtime_control_completion(worker_ref caller) :
        state_(std::make_shared<state>(std::move(caller))),
        lifetime_(state_.get(), [value = state_](void*) noexcept {
            std::coroutine_handle<> continuation;
            {
                std::lock_guard lock(value->mutex_);
                value->released_ = true;
                continuation = std::exchange(value->continuation_, {});
            }
            if (continuation) {
                value->caller_.defer_resume(continuation);
            }
        }) {}
    runtime_control_completion(const runtime_control_completion&) = delete;
    runtime_control_completion& operator=(const runtime_control_completion&) = delete;
    [[nodiscard]] receipt make_receipt() const {
        if (!lifetime_) {
            throw std::logic_error("runtime control completion is already waiting");
        }
        return receipt(state_, lifetime_);
    }
    template <typename function_type>
    [[nodiscard]] command<std::decay_t<function_type>> make_command(function_type function) const {
        return make_receipt().bind(std::move(function));
    }
    [[nodiscard]] wait_operation wait() & {
        if (!state_->caller_.is_current()) {
            throw std::logic_error("runtime control must wait on its caller worker");
        }
        if (waiting_) {
            throw std::logic_error("runtime control completion can only wait once");
        }
        waiting_ = true;
        lifetime_.reset();
        return wait_operation(state_);
    }
    wait_operation wait() && = delete;
    [[nodiscard]] std::size_t completed() const noexcept {
        std::lock_guard lock(state_->mutex_);
        return state_->completed_;
    }
    [[nodiscard]] std::exception_ptr failure() const noexcept {
        std::lock_guard lock(state_->mutex_);
        state_->observed_ = true;
        return state_->failure_;
    }

private:
    std::shared_ptr<state> state_;
    std::shared_ptr<void> lifetime_;
    bool waiting_{};
};

// Serializes admitted controls without holding a mutex across suspension.
// The owning state must outlive every acquire and granted lease.
class runtime_control_gate final {
public:
    class acquire_operation;
    class lease final {
    public:
        lease(const lease&) = delete;
        lease& operator=(const lease&) = delete;
        lease(lease&& other) noexcept :
            gate_(std::exchange(other.gate_, nullptr)) {}
        lease& operator=(lease&&) = delete;
        ~lease() {
            if (gate_) {
                gate_->release();
            }
        }

    private:
        explicit lease(runtime_control_gate& gate) noexcept :
            gate_(&gate) {}
        runtime_control_gate* gate_;
        friend class acquire_operation;
    };

    class acquire_operation final {
    public:
        acquire_operation(const acquire_operation&) = delete;
        acquire_operation& operator=(const acquire_operation&) = delete;
        void operator&() const = delete;
        ~acquire_operation() {
            if (started_ && !resumed_) {
                std::terminate();
            }
        }
        bool await_ready() {
            if (!caller_.is_current()) {
                throw std::logic_error("runtime control must be awaited on its caller worker");
            }
            std::lock_guard lock(gate_.mutex_);
            started_ = true;
            if (gate_.active_) {
                return false;
            }
            gate_.active_ = true;
            return true;
        }
        bool await_suspend(std::coroutine_handle<> continuation) {
            std::lock_guard lock(gate_.mutex_);
            if (!gate_.active_) {
                gate_.active_ = true;
                return false;
            }
            continuation_ = continuation;
            if (gate_.tail_) {
                gate_.tail_->next_ = this;
            } else {
                gate_.head_ = this;
            }
            gate_.tail_ = this;
            return true;
        }
        lease await_resume() noexcept {
            resumed_ = true;
            return lease(gate_);
        }

    private:
        acquire_operation(runtime_control_gate& gate, worker_ref caller) noexcept :
            gate_(gate),
            caller_(std::move(caller)) {}
        runtime_control_gate& gate_;
        worker_ref caller_;
        acquire_operation* next_{};
        std::coroutine_handle<> continuation_{};
        bool started_{};
        bool resumed_{};
        friend class runtime_control_gate;
    };

    runtime_control_gate() = default;
    runtime_control_gate(const runtime_control_gate&) = delete;
    runtime_control_gate& operator=(const runtime_control_gate&) = delete;
    ~runtime_control_gate() {
        if (active_ || head_) {
            std::terminate();
        }
    }
    [[nodiscard]] acquire_operation acquire(worker_ref caller) & noexcept {
        return acquire_operation(*this, std::move(caller));
    }
    acquire_operation acquire(worker_ref) && = delete;

private:
    void release() noexcept {
        worker_ref caller;
        std::coroutine_handle<> continuation;
        {
            std::lock_guard lock(mutex_);
            if (!head_) {
                active_ = false;
                return;
            }
            auto* next = head_;
            head_ = next->next_;
            if (!head_) {
                tail_ = nullptr;
            }
            next->next_ = nullptr;
            caller = next->caller_;
            continuation = std::exchange(next->continuation_, {});
        }
        caller.defer_resume(continuation);
    }
    std::mutex mutex_;
    acquire_operation* head_{};
    acquire_operation* tail_{};
    bool active_{};
};

class operation_interrupt;

// Routes stop requests to the running operations of one I/O owner on its
// worker. The owner keeps it at a stable address until those operations have
// completed. The first stoppable operation creates the shared target; it is
// detached on destruction, so a stop delivered later reaches no owner.
class operation_interrupt_source final {
public:
    operation_interrupt_source() noexcept = default;
    operation_interrupt_source(const operation_interrupt_source&) = delete;
    operation_interrupt_source& operator=(const operation_interrupt_source&) = delete;
    ~operation_interrupt_source() {
        if (target_) {
            target_->detach(*this);
        }
    }

    void cancel_operation_by_id(std::uint64_t id) noexcept;

private:
    friend class operation_interrupt;

    std::shared_ptr<worker_cancellation_target<operation_interrupt_source>> target_;
    operation_interrupt* running_{};
};

enum class operation_interrupt_cause : std::uint8_t { cancelled,
    timed_out };

// The deadline and stop token of one running operation. Both are delivered on
// the operation's worker, where the first records its cause and calls
// `interrupt(target)` once: a read wakes its wait and keeps buffered input, an
// output operation aborts its stream. A stop requested before construction
// throws operation_canceled before anything is armed, so the operation has no
// effect. Construct it before the operation's first wait and keep it until
// that operation completes.
class operation_interrupt final {
public:
    using interrupt_type = void (*)(void*) noexcept;

    operation_interrupt(operation_interrupt_source& source, const worker_ref& worker_value, const operation_options& options, void* target, interrupt_type interrupt, const char* operation_name) :
        source_(source),
        target_(target),
        interrupt_(interrupt),
        operation_name_(operation_name) {
        if (options.stop_token_.stop_requested()) {
            throw_cause(operation_interrupt_cause::cancelled, operation_name_);
        }
        if (options.stop_token_.stoppable() && !source_.target_) {
            source_.target_ = make_worker_cancellation_target(source_, worker_value);
        }
        if (options.timeout_.has_value()) {
            (void)worker_value.schedule_timer(timer_, options.timeout_->deadline_from(std::chrono::steady_clock::now()), [this](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    fire(operation_interrupt_cause::timed_out);
                }
            });
        }
        // Linked before arming: a stop requested meanwhile is delivered inline.
        next_ = source_.running_;
        source_.running_ = this;
        if (options.stop_token_.stoppable()) {
            try {
                cancellation_.emplace(source_.target_, id_);
                cancellation_->arm(options.stop_token_);
            } catch (...) {
                cancellation_.reset();
                unlink();
                throw;
            }
        }
    }

    operation_interrupt(const operation_interrupt&) = delete;
    operation_interrupt& operator=(const operation_interrupt&) = delete;

    ~operation_interrupt() {
        // Joins a stop callback before this operation leaves the source.
        cancellation_.reset();
        unlink();
    }

    [[nodiscard]] bool interrupted() const noexcept {
        return cause_.has_value();
    }

    [[nodiscard]] std::optional<operation_interrupt_cause> cause() const noexcept {
        return cause_;
    }

    // Throws std::system_error (timed_out or operation_canceled) once interrupted.
    void throw_if_interrupted() const {
        if (cause_.has_value()) {
            throw_cause(*cause_, operation_name_);
        }
    }

    [[noreturn]] static void throw_cause(operation_interrupt_cause cause, const char* operation_name) {
        throw std::system_error(std::make_error_code(cause == operation_interrupt_cause::timed_out ? std::errc::timed_out : std::errc::operation_canceled), operation_name);
    }

private:
    friend class operation_interrupt_source;

    void unlink() noexcept {
        for (auto** link = &source_.running_; *link != nullptr; link = &(*link)->next_) {
            if (*link == this) {
                *link = next_;
                return;
            }
        }
    }

    void fire(operation_interrupt_cause cause) noexcept {
        if (cause_.has_value()) {
            return;
        }
        cause_ = cause;
        interrupt_(target_);
    }

    operation_interrupt_source& source_;
    void* target_;
    interrupt_type interrupt_;
    const char* operation_name_;
    std::optional<operation_interrupt_cause> cause_;
    operation_interrupt* next_{};
    std::uint64_t id_{};
    worker_timer_registration timer_;
    // Declared last: its reset joins a pending stop callback first.
    std::optional<worker_cancellation_registration<worker_cancellation_target<operation_interrupt_source>>> cancellation_;
};

inline void operation_interrupt_source::cancel_operation_by_id(std::uint64_t id) noexcept {
    for (auto* running = running_; running != nullptr; running = running->next_) {
        if (id != 0 && running->id_ == id) {
            running->fire(operation_interrupt_cause::cancelled);
            return;
        }
    }
}

// One operation made of several interruptible steps: each step runs with the
// time left and the same stop token, and a step's interruption is recognized
// as this operation's own.
class operation_steps final {
public:
    explicit operation_steps(operation_options operation) noexcept :
        deadline_(operation.timeout_),
        stop_(std::move(operation.stop_token_)) {}

    // The time left and the same stop token for the next step; std::nullopt
    // once the deadline has passed.
    [[nodiscard]] std::optional<operation_options> try_next() const {
        if (deadline_.expired()) {
            return std::nullopt;
        }
        return operation_options{.timeout_ = deadline_.timeout(), .stop_token_ = stop_};
    }

    // Throws std::system_error (timed_out) once the deadline has passed.
    [[nodiscard]] operation_options next(const char* operation_name) const {
        auto step = try_next();
        if (!step.has_value()) {
            operation_interrupt::throw_cause(operation_interrupt_cause::timed_out, operation_name);
        }
        return std::move(*step);
    }

    // Whether this operation's own deadline or stop token accounts for `cause`.
    [[nodiscard]] bool interrupted_by(operation_interrupt_cause cause) const noexcept {
        return cause == operation_interrupt_cause::timed_out ? deadline_.expired() : stop_.stop_requested();
    }

    // Whether `error` is a step interrupted by this operation's deadline or stop token.
    [[nodiscard]] bool interrupted(const std::system_error& error) const noexcept {
        return (error.code() == std::errc::timed_out && interrupted_by(operation_interrupt_cause::timed_out)) || (error.code() == std::errc::operation_canceled && interrupted_by(operation_interrupt_cause::cancelled));
    }

private:
    operation_deadline deadline_;
    stop_token stop_;
};

// A configured host whose port is a separate field: brackets and a lone colon
// (host:port) are rejected; an unbracketed IPv6 literal has several colons.
inline void ensure_config_host(std::string_view host, const char* empty_message, const char* invalid_message) {
    if (host.empty()) {
        throw std::invalid_argument(empty_message);
    }
    std::size_t colon_count = 0;
    for (const auto ch : host) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte == 0x7F || byte == '/' || byte == '\\' || byte == '[' || byte == ']') {
            throw std::invalid_argument(invalid_message);
        }
        colon_count += byte == ':' ? 1 : 0;
    }
    if (colon_count == 1) {
        throw std::invalid_argument(invalid_message);
    }
}

inline void ensure_positive_size(std::size_t value, const char* message) {
    if (value == 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_positive_optional_size(const std::optional<std::size_t>& value, const char* message) {
    if (value.has_value() && *value == 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_non_zero_port(std::uint16_t port, const char* message) {
    if (port == 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_non_zero_optional_port(const std::optional<port_number>& port, const char* message) {
    if (port.has_value()) {
        ensure_non_zero_port(port->value(), message);
    }
}

template <typename rep_type, typename period_type>
void ensure_positive_duration(std::chrono::duration<rep_type, period_type> value, const char* message) {
    if (value.count() <= 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_positive_duration(timeout_duration value, const char* message) {
    ensure_positive_duration(value.value(), message);
}

template <typename rep_type, typename period_type>
void ensure_positive_optional_duration(const std::optional<std::chrono::duration<rep_type, period_type>>& value, const char* message) {
    if (value.has_value() && value->count() <= 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_positive_optional_duration(const std::optional<timeout_duration>& value, const char* message) {
    if (value.has_value()) {
        ensure_positive_duration(*value, message);
    }
}

template <typename first_duration_type, typename... rest_durations_type>
void ensure_positive_optional_durations(const char* message, const first_duration_type& first, const rest_durations_type&... rest) {
    ensure_positive_optional_duration(first, message);
    (ensure_positive_optional_duration(rest, message), ...);
}

inline constexpr std::size_t http_body_buffer_bytes = std::size_t{8} * 1024;
// The smallest capacity a buffered known-length body grows to while it is read
// straight into its own storage. It matches the WebSocket read window: a peer
// that sends only headers can make the declared length allocate at most this
// much before any body bytes arrive; beyond it capacity doubles from arrivals.
inline constexpr std::size_t known_length_min_read_bytes = std::size_t{64} * 1024;

// http_error_info borrows its fields. The caller's http_protocol_error lvalue
// must outlive any suspension of the error handler that reads this view.
[[nodiscard]] inline http_error_info copy_http_protocol_error_info(const http_protocol_error& error RUVIA_LIFETIMEBOUND) noexcept {
    return http_error_info({.status_ = error.status(), .code_ = error.code(), .message_ = std::string_view(error.what())});
}
http_error_info copy_http_protocol_error_info(const http_protocol_error&&) = delete;

enum class http_stream_end : std::uint8_t { keep_open,
    end };

// Whether a read error marks the end of input rather than a read failure: a
// TCP FIN or TLS close_notify (eof), or a TLS peer that closed TCP without
// close_notify (stream_truncated). Message-delimited protocols above the
// stream still detect a truncated message themselves; the transport only
// reports that no more bytes follow.
[[nodiscard]] inline bool is_stream_read_end(const std::error_code& error) noexcept {
    return error == asio::error::eof || error == asio::ssl::error::stream_truncated;
}

// A failed I/O operation on the inbound connection's own transport: the socket
// or TLS stream this server accepted. The type carries the failure's
// provenance, so connection_failure_sink can tell a connection that ended (the
// peer reset it, or this server cancelled its I/O) from a handler or upstream
// failure that happens to carry the same error code. Handlers still observe it
// as a std::system_error.
class inbound_transport_error final : public std::system_error {
public:
    inbound_transport_error(std::error_code code, const char* what_arg) :
        std::system_error(code, what_arg) {}
};

// Errors with which an inbound connection ends without any fault on this side:
// the peer closed it (including a TLS peer that closed TCP without
// close_notify), reset or aborted it, the network gave up on a vanished peer,
// or this server ended the I/O itself. Cancellation may carry Asio's native
// operation_aborted code or the generic operation_canceled condition used by
// the TLS output pump; bad_descriptor is I/O started after our own close
// (stop, drain, or a timeout). Only this server closes an inbound socket, so
// these codes cannot come from anything else on the inbound transport.
[[nodiscard]] inline bool is_inbound_connection_end(const std::error_code& error) noexcept {
    return is_stream_read_end(error) || error == asio::error::connection_reset || error == asio::error::connection_aborted || error == asio::error::broken_pipe || error == asio::error::operation_aborted || error == std::errc::operation_canceled || error == asio::error::bad_descriptor || error == asio::error::timed_out || error == asio::error::not_connected || error == asio::error::shut_down;
}

// Owner-thread phase of one server worker:
// - running: admits connections and requests;
// - stopped: the core runtime stopped; every connection is closed now.
enum class http_server_worker_state : std::uint8_t {
    fresh,
    running,
    stopped,
};

// True only while the worker admits new connections and requests.
[[nodiscard]] inline bool http_server_worker_running(http_server_worker_state state_value) noexcept {
    return state_value == http_server_worker_state::running;
}

// One worker's admitted-connection gauge. Only the owning worker counts; any
// thread reads it relaxed (stats(), and the Windows acceptor choosing the
// least-loaded worker).
class connection_count final {
public:
    explicit connection_count(connection_count* parent = nullptr) noexcept :
        parent_(parent) {}
    connection_count(const connection_count&) = delete;
    connection_count& operator=(const connection_count&) = delete;

    [[nodiscard]] std::size_t active() const noexcept {
        return active_.load(std::memory_order_relaxed);
    }

    // Worker thread: one admitted connection.
    void acquire() noexcept {
        active_.fetch_add(1, std::memory_order_relaxed);
        if (parent_) {
            parent_->acquire();
        }
    }

    // Worker thread: one closed connection.
    void release() noexcept {
        const auto previous = active_.fetch_sub(1, std::memory_order_relaxed);
        if (previous == 0) {
            std::terminate();
        }
        if (parent_) {
            parent_->release();
        }
        if (previous == 1) {
            std::vector<std::weak_ptr<wait_state>> ready;
            {
                std::lock_guard lock(mutex_);
                ready.swap(waiters_);
            }
            for (auto& weak : ready) {
                if (auto state = weak.lock()) {
                    state->closed_.store(true, std::memory_order_release);
                    detail::worker_ref_access::defer_or_terminate(state->worker_, [state] {
                        state->changed_.notify();
                    });
                }
            }
        }
    }

    // The caller retains the gauge; registrations never borrow its coroutine.
    static task<void> wait(std::shared_ptr<connection_count> count, worker_ref caller, operation_options options, stop_token host_stop) {
        if (!caller.is_current()) {
            throw std::logic_error("connection wait must run on its caller worker");
        }
        stop_group stops(options.stop_token_, host_stop);
        options.stop_token_ = stops.token();
        auto state = std::make_shared<wait_state>(caller);
        operation_interrupt_source source;
        operation_interrupt interrupt(source, caller, options, state.get(), [](void* target) noexcept {
            static_cast<wait_state*>(target)->changed_.notify();
        }, "wait_connections_closed");
        {
            std::lock_guard lock(count->mutex_);
            if (count->active() != 0) {
                count->waiters_.push_back(state);
            } else {
                state->closed_.store(true, std::memory_order_relaxed);
            }
        }
        struct subscription final {
            connection_count& count_;
            wait_state* state_;
            ~subscription() {
                std::lock_guard lock(count_.mutex_);
                std::erase_if(count_.waiters_, [this](const auto& weak) {
                    const auto state = weak.lock();
                    return !state || state.get() == state_;
                });
            }
        } registration{*count, state.get()};
        interrupt.throw_if_interrupted();
        while (!state->closed_.load(std::memory_order_acquire)) {
            co_await state->changed_.wait();
            interrupt.throw_if_interrupted();
        }
        interrupt.throw_if_interrupted();
    }

private:
    std::atomic<std::size_t> active_{};
    struct wait_state final {
        explicit wait_state(worker_ref worker) :
            worker_(std::move(worker)),
            changed_(worker_) {}
        worker_ref worker_;
        worker_signal changed_;
        std::atomic<bool> closed_{false};
    };
    connection_count* parent_;
    std::mutex mutex_;
    std::vector<std::weak_ptr<wait_state>> waiters_;
};

// One admitted connection's place in its worker's connection_count, as a
// linear value. Every transport's connection owns exactly one: a TCP session
// through accepted_connection_lease, a QUIC connection directly.
class connection_slot final {
public:
    explicit connection_slot(connection_count& count) noexcept :
        count_(&count) {
        count_->acquire();
    }

    connection_slot(const connection_slot&) = delete;
    connection_slot& operator=(const connection_slot&) = delete;

    connection_slot(connection_slot&& other) noexcept :
        count_(std::exchange(other.count_, nullptr)) {}

    connection_slot& operator=(connection_slot&&) = delete;

    ~connection_slot() {
        if (count_ != nullptr) {
            count_->release();
        }
    }

private:
    connection_count* count_;
};

// Merge endpoint gauges across workers and successive listener instances.
inline void append_listener_stats(http_server_stats& total, const http_server_stats& worker) {
    for (const auto& listener : worker.listeners_) {
        auto found = std::find_if(total.listeners_.begin(), total.listeners_.end(), [&](const auto& existing) {
            return existing.address_ == listener.address_ && existing.port_ == listener.port_;
        });
        if (found == total.listeners_.end()) {
            total.listeners_.push_back(listener);
        } else {
            found->active_connections_ += listener.active_connections_;
        }
    }
}

// Process-wide monotonically increasing IDs cannot alias across workers,
// hosts, or restarts. Exhaustion terminates rather than recycling a stale ID.
class connection_id_sequence final {
public:
    connection_id_sequence() noexcept = default;

    connection_id_sequence(const connection_id_sequence&) = delete;
    connection_id_sequence& operator=(const connection_id_sequence&) = delete;

    [[nodiscard]] std::uint64_t next() noexcept {
        const auto id = next_.fetch_add(1, std::memory_order_relaxed);
        if (id == 0) {
            std::terminate();
        }
        return id;
    }

private:
    inline static std::atomic<std::uint64_t> next_{1};
};

[[nodiscard]] inline ip_address to_ip_address(const asio::ip::address& address) noexcept {
    if (address.is_v4()) {
        return ip_address::ipv4(address.to_v4().to_bytes());
    }
    const auto v6 = address.to_v6();
    return ip_address::ipv6(v6.to_bytes(), static_cast<std::uint32_t>(v6.scope_id()));
}

[[nodiscard]] inline asio::ip::address to_asio_address(const ip_address& address) noexcept {
    if (address.family() == ip_address_family::ipv4) {
        asio::ip::address_v4::bytes_type bytes{};
        std::copy(address.bytes().begin(), address.bytes().end(), bytes.begin());
        return asio::ip::address_v4(bytes);
    }
    asio::ip::address_v6::bytes_type bytes{};
    std::copy(address.bytes().begin(), address.bytes().end(), bytes.begin());
    return asio::ip::address_v6(bytes, address.scope_id());
}

// The peer address a connection's services name, shared by TCP and QUIC
// sessions: dotted IPv4, also for an IPv4-mapped IPv6 peer, otherwise the
// RFC 5952 IPv6 text.
inline void assign_peer_address(std::string& output, const asio::ip::address& address) {
    std::array<char, 64> buffer;
    asio::error_code ec;
    const char* formatted = nullptr;
    if (address.is_v4() || address.to_v6().is_v4_mapped()) {
        const auto v4 = address.is_v4() ? address.to_v4() : asio::ip::make_address_v4(asio::ip::v4_mapped, address.to_v6());
        const auto bytes = v4.to_bytes();
        formatted = asio::detail::socket_ops::inet_ntop(ASIO_OS_DEF(AF_INET), bytes.data(), buffer.data(), buffer.size(), 0, ec);
    } else {
        const auto v6 = address.to_v6();
        const auto bytes = v6.to_bytes();
        formatted = asio::detail::socket_ops::inet_ntop(ASIO_OS_DEF(AF_INET6), bytes.data(), buffer.data(), buffer.size(), v6.scope_id(), ec);
    }
    if (formatted == nullptr) {
        asio::detail::throw_error(ec);
    }
    output.assign(formatted, std::strlen(formatted));
}

enum class accept_error_action : std::uint8_t { retry,
    report_and_retry,
    fail };

// Peer disappearance and readiness races are normal accept outcomes. Resource
// and network failures leave the listener usable but must remain observable.
[[nodiscard]] inline accept_error_action classify_accept_error(const std::error_code& error) noexcept {
    if (error == asio::error::connection_aborted || error == asio::error::connection_reset || error == asio::error::eof || error == asio::error::try_again || error == asio::error::would_block || error == asio::error::interrupted
#if defined(_WIN32)
        || (error.category() == asio::system_category() && error.value() == ERROR_NETNAME_DELETED)
#endif
    ) {
        return accept_error_action::retry;
    }
    if (error == asio::error::no_descriptors || error == asio::error::no_buffer_space || error == asio::error::no_memory || error == std::errc::too_many_files_open_in_system || error == std::errc::protocol_error || error == asio::error::network_down || error == asio::error::network_unreachable || error == asio::error::host_unreachable || error == asio::error::no_protocol_option || error == asio::error::operation_not_supported || error == asio::error::no_permission || error == asio::error::timed_out) {
        return accept_error_action::report_and_retry;
    }
#if defined(EHOSTDOWN) && defined(ENONET)
    if (error.category() == asio::system_category() && (error.value() == EHOSTDOWN || error.value() == ENONET)) {
        return accept_error_action::report_and_retry;
    }
#endif
    return accept_error_action::fail;
}

inline void validate_client_tcp_config(const client_tcp_config& config) {
    switch (config.no_delay_) {
        case tcp_no_delay_policy::system_default:
        case tcp_no_delay_policy::enable:
            break;
        default:
            throw std::invalid_argument("TCP no-delay policy is invalid");
    }
    switch (config.keep_alive_) {
        case tcp_keep_alive_policy::system_default:
        case tcp_keep_alive_policy::enable:
            break;
        default:
            throw std::invalid_argument("TCP keepalive policy is invalid");
    }
}

// Returns the error of the first enabled policy that cannot be applied.
[[nodiscard]] inline std::error_code apply_tcp_socket_policies(asio::ip::tcp::socket& socket, const client_tcp_config& config) noexcept {
    std::error_code error;
    if (config.no_delay_ == tcp_no_delay_policy::enable) {
        socket.set_option(asio::ip::tcp::no_delay(true), error);
        if (error) {
            return error;
        }
    }
    if (config.keep_alive_ == tcp_keep_alive_policy::enable) {
        socket.set_option(asio::socket_base::keep_alive(true), error);
    }
    return error;
}

// Intrusive, worker-affine registry. No lock is held while invoking user code
// or releasing state. IDs are retired before close callbacks can reenter.
class connection_registry final {
public:
    [[nodiscard]] bool close_connection(std::uint64_t id) noexcept;
    void close_listener(const void* listener) noexcept;

private:
    friend class connection_lifecycle;
    connection_lifecycle* head_{};
};

class connection_lifecycle final {
public:
    connection_lifecycle(connection_registry& registry, const void* listener, connection_event event, connection_open_callback_ref opened, connection_close_callback_ref closed, std::uint32_t interface_index = 0, connection_transport transport = {}) :
        registry_(&registry),
        listener_(listener),
        event_(std::move(event)),
        closed_(closed),
        transport_(transport),
        bound_(transport.valid()),
        interface_index_(interface_index) {
        next_ = registry.head_;
        if (next_ != nullptr) {
            next_->prev_ = this;
        }
        registry.head_ = this;
        format_local();
        try {
            if (opened) {
                state_ = opened(event_);
            }
        } catch (...) {
            if (prev_ != nullptr) {
                prev_->next_ = next_;
            } else {
                registry_->head_ = next_;
            }
            if (next_ != nullptr) {
                next_->prev_ = prev_;
            }
            registry_ = nullptr;
            if (bound_) {
                transport_.close();
            }
            info_.reason_ = connection_close_reason::handler_error;
            if (closed_) {
                closed_(event_, state_, info_);
            }
            throw;
        }
    }
    connection_lifecycle(const connection_lifecycle&) = delete;
    connection_lifecycle& operator=(const connection_lifecycle&) = delete;
    connection_lifecycle& operator=(connection_lifecycle&&) = delete;
    connection_lifecycle(connection_lifecycle&& other) noexcept :
        registry_(std::exchange(other.registry_, nullptr)),
        listener_(other.listener_),
        prev_(other.prev_),
        next_(other.next_),
        event_(other.event_),
        closed_(other.closed_),
        state_(std::move(other.state_)),
        info_(other.info_),
        transport_(other.transport_),
        aborted_(other.aborted_),
        bound_(other.bound_),
        interface_index_(other.interface_index_),
        local_text_(other.local_text_) {
        if (registry_ != nullptr) {
            if (prev_ != nullptr) {
                prev_->next_ = this;
            } else {
                registry_->head_ = this;
            }
            if (next_ != nullptr) {
                next_->prev_ = this;
            }
        }
    }
    ~connection_lifecycle() {
        if (registry_ == nullptr) {
            return;
        }
        if (prev_ != nullptr) {
            prev_->next_ = next_;
        } else {
            registry_->head_ = next_;
        }
        if (next_ != nullptr) {
            next_->prev_ = prev_;
        }
        registry_ = nullptr;
        const auto closed = closed_;
        const auto event = event_;
        const auto info = info_;
        const auto state = std::move(state_);
        if (closed) {
            closed(event, state, info);
        }
    }
    [[nodiscard]] std::uint64_t connection_id() const noexcept {
        return event_.connection_id();
    }
    [[nodiscard]] const std::shared_ptr<void>& state() const noexcept {
        return state_;
    }
    [[nodiscard]] std::string_view local_address() const noexcept {
        return local_text_.data();
    }
    [[nodiscard]] const ip_endpoint& local_endpoint() const noexcept {
        return event_.local();
    }
    [[nodiscard]] const ip_endpoint& remote_endpoint() const noexcept {
        return event_.remote();
    }
    void set_reason(connection_close_reason reason) noexcept {
        // Keep the initiating cause when teardown subsequently cancels I/O.
        if (info_.reason_ == connection_close_reason::normal || (info_.reason_ == connection_close_reason::peer_eof && reason != connection_close_reason::normal)) {
            info_.reason_ = reason;
        }
    }
    void note_read(std::size_t bytes) noexcept {
        add_bytes(info_.bytes_in_, bytes);
    }
    void note_write(std::size_t bytes) noexcept {
        add_bytes(info_.bytes_out_, bytes);
    }
    void note_request() noexcept {
        add_bytes(info_.requests_, 1);
    }
    void note_error(const std::error_code& error) noexcept {
        if (!error || error == asio::error::would_block || error == asio::error::try_again || error == asio::error::interrupted) {
            return;
        }
        if (is_stream_read_end(error)) {
            set_reason(connection_close_reason::peer_eof);
        } else if (error == asio::error::timed_out || error == std::errc::timed_out) {
            set_reason(connection_close_reason::timeout);
        } else if (error == asio::error::operation_aborted || error == std::errc::operation_canceled || error == asio::error::bad_descriptor) {
            set_reason(connection_close_reason::cancelled);
        } else if (error == std::errc::protocol_error || error.category() == asio::error::get_ssl_category()) {
            set_reason(connection_close_reason::protocol_error);
        } else {
            set_reason(connection_close_reason::transport_error);
        }
    }
    void note_exception(const std::exception_ptr& failure) noexcept {
        if (failure == nullptr) {
            return;
        }
        try {
            std::rethrow_exception(failure);
        } catch (const inbound_transport_error& error) {
            note_error(error.code());
        } catch (...) {
            set_reason(connection_close_reason::handler_error);
        }
    }
    [[nodiscard]] std::uint32_t interface_index() const noexcept {
        return interface_index_;
    }
    [[nodiscard]] std::uint16_t local_port() const noexcept {
        return event_.local().port();
    }
    void bind_transport(connection_transport transport) noexcept {
        transport_ = transport;
        bound_ = true;
        if (aborted_) {
            transport_.close();
        }
    }
    void abort() noexcept {
        set_reason(connection_close_reason::aborted);
        aborted_ = true;
        if (bound_) {
            transport_.close();
        }
    }
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_;
    }

private:
    friend class connection_registry;
    static void add_bytes(std::uint64_t& value, std::size_t bytes) noexcept {
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        value += (std::min)(maximum - value, static_cast<std::uint64_t>(bytes));
    }
    void format_local() noexcept {
        const auto address = to_asio_address(event_.local().address());
        asio::error_code error;
        if (address.is_v4()) {
            const auto bytes = address.to_v4().to_bytes();
            (void)asio::detail::socket_ops::inet_ntop(ASIO_OS_DEF(AF_INET), bytes.data(), local_text_.data(), local_text_.size(), 0, error);
        } else {
            const auto v6 = address.to_v6();
            const auto bytes = v6.to_bytes();
            (void)asio::detail::socket_ops::inet_ntop(ASIO_OS_DEF(AF_INET6), bytes.data(), local_text_.data(), local_text_.size(), v6.scope_id(), error);
        }
    }
    connection_registry* registry_;
    const void* listener_;
    connection_lifecycle* prev_{};
    connection_lifecycle* next_{};
    connection_event event_;
    connection_close_callback_ref closed_;
    std::shared_ptr<void> state_;
    connection_close_info info_;
    connection_transport transport_;
    bool aborted_{};
    bool bound_{};
    std::uint32_t interface_index_{};
    std::array<char, 64> local_text_{};
};

inline bool connection_registry::close_connection(std::uint64_t id) noexcept {
    if (id == 0) {
        return false;
    }
    for (auto* node = head_; node != nullptr; node = node->next_) {
        if (node->connection_id() == id) {
            node->abort();
            return true;
        }
    }
    return false;
}

inline void connection_registry::close_listener(const void* listener) noexcept {
    for (auto* node = head_; node != nullptr; node = node->next_) {
        if (node->listener_ == listener) {
            node->abort();
        }
    }
}

// The caller owns the socket and the already-read prefix until all I/O joins.
// Prefix delivery is asynchronous, like socket reads, and never reads a byte twice.
class inbound_socket_stream final {
public:
    using executor_type = asio::ip::tcp::socket::executor_type;
    using lowest_layer_type = asio::ip::tcp::socket;

    explicit inbound_socket_stream(asio::ip::tcp::socket& socket, std::span<const std::byte> prefix = {}, connection_lifecycle* lifecycle = nullptr) noexcept :
        socket_(socket),
        prefix_(prefix),
        lifecycle_(lifecycle) {}

    executor_type get_executor() noexcept {
        return socket_.get_executor();
    }
    lowest_layer_type& lowest_layer() noexcept {
        return socket_;
    }
    const lowest_layer_type& lowest_layer() const noexcept {
        return socket_;
    }
    auto native_handle() {
        return socket_.native_handle();
    }
    bool non_blocking() const {
        return socket_.non_blocking();
    }
    void shutdown(asio::ip::tcp::socket::shutdown_type what, std::error_code& error) {
        socket_.shutdown(what, error);
    }
    template <typename buffers_type>
    std::size_t send(const buffers_type& buffers, asio::socket_base::message_flags flags, std::error_code& error) {
        const auto bytes = socket_.send(buffers, flags, error);
        if (lifecycle_ != nullptr) {
            lifecycle_->note_write(bytes);
            lifecycle_->note_error(error);
        }
        return bytes;
    }

    template <typename buffers_type>
    std::size_t read_some(const buffers_type& buffers, std::error_code& error) {
        if (!prefix_.empty()) {
            error.clear();
            return consume_prefix(buffers);
        }
        const auto bytes = socket_.read_some(buffers, error);
        if (lifecycle_ != nullptr) {
            lifecycle_->note_read(bytes);
            lifecycle_->note_error(error);
        }
        return bytes;
    }

    template <typename buffers_type, typename handler_type>
    void async_read_some(const buffers_type& buffers, handler_type&& handler) {
        if (prefix_.empty()) {
            socket_.async_read_some(buffers, completion<std::decay_t<handler_type>, false>{std::forward<handler_type>(handler), socket_.get_executor(), lifecycle_});
            return;
        }
        const auto bytes = consume_prefix(buffers);
        asio::post(socket_.get_executor(), [handler = std::forward<handler_type>(handler), bytes]() mutable {
            std::move(handler)(std::error_code{}, bytes);
        });
    }

    template <typename buffers_type, typename handler_type>
    void async_write_some(const buffers_type& buffers, handler_type&& handler) {
        socket_.async_write_some(buffers, completion<std::decay_t<handler_type>, true>{std::forward<handler_type>(handler), socket_.get_executor(), lifecycle_});
    }

private:
    template <typename handler_type, bool writing>
    struct completion final {
        handler_type handler_;
        inbound_socket_stream::executor_type fallback_executor_;
        connection_lifecycle* lifecycle_;
        using executor_type = asio::associated_executor_t<handler_type, inbound_socket_stream::executor_type>;
        using allocator_type = asio::associated_allocator_t<handler_type>;
        using cancellation_slot_type = asio::associated_cancellation_slot_t<handler_type>;
        [[nodiscard]] executor_type get_executor() const noexcept {
            return asio::get_associated_executor(handler_, fallback_executor_);
        }
        [[nodiscard]] allocator_type get_allocator() const noexcept {
            return asio::get_associated_allocator(handler_);
        }
        [[nodiscard]] cancellation_slot_type get_cancellation_slot() const noexcept {
            return asio::get_associated_cancellation_slot(handler_);
        }
        void operator()(const std::error_code& error, std::size_t bytes) {
            if (lifecycle_ != nullptr) {
                if constexpr (writing) {
                    lifecycle_->note_write(bytes);
                } else {
                    lifecycle_->note_read(bytes);
                }
                // Protocol drivers also cancel reads for a successful drain.
                // Timeout, shutdown and explicit abort record their cause first.
                if (error != asio::error::operation_aborted && error != asio::error::bad_descriptor) {
                    lifecycle_->note_error(error);
                }
            }
            std::move(handler_)(error, bytes);
        }
    };
    template <typename buffers_type>
    std::size_t consume_prefix(const buffers_type& buffers) noexcept {
        const auto bytes = asio::buffer_copy(buffers, asio::buffer(prefix_.data(), prefix_.size()));
        prefix_ = prefix_.subspan(bytes);
        return bytes;
    }
    asio::ip::tcp::socket& socket_;
    std::span<const std::byte> prefix_;
    connection_lifecycle* lifecycle_;
};

// SSL 的读操作也可能发送 KeyUpdate 等控制记录。取消读允许保留已接收的
// 密文字节，但不能截断已从 SSL BIO 取出的输出，否则后续复用会丢失记录。
// 屏蔽单操作的写取消；socket 关闭和既有连接超时仍能终止底层写入。
class tls_socket_transport final {
public:
    using executor_type = asio::ip::tcp::socket::executor_type;
    using lowest_layer_type = asio::ip::tcp::socket;

    explicit tls_socket_transport(inbound_socket_stream& socket) noexcept :
        socket_(socket) {}

    [[nodiscard]] executor_type get_executor() noexcept {
        return socket_.get_executor();
    }

    [[nodiscard]] lowest_layer_type& lowest_layer() noexcept {
        return socket_.lowest_layer();
    }

    [[nodiscard]] const lowest_layer_type& lowest_layer() const noexcept {
        return socket_.lowest_layer();
    }

    template <typename buffers_type, typename handler_type>
    auto async_read_some(const buffers_type& buffers, handler_type&& handler) {
        return socket_.async_read_some(buffers, std::forward<handler_type>(handler));
    }

    template <typename buffers_type, typename handler_type>
    auto async_write_some(const buffers_type& buffers, handler_type&& handler) {
        return socket_.async_write_some(buffers, asio::bind_cancellation_slot(asio::cancellation_slot{}, std::forward<handler_type>(handler)));
    }

private:
    inbound_socket_stream& socket_;
};

using server_tls_stream = asio::ssl::stream<tls_socket_transport>;

// How a TCP connection session ended, which decides how its socket closes.
// A stopping worker closes every connection immediately, whatever its mode.
enum class http_connection_close : std::uint8_t {
    // Transport failure, a stalled transfer, or a vanished peer: close at once.
    abort,
    // No further peer input is expected: the peer ended its input (FIN or TLS
    // close_notify), or this side ended a wait for a request head (timeout or
    // drain). TLS sends close_notify first (RFC 9112 §9.8, RFC 9846 §6.1).
    notify,
    // This side ended the connection while peer input may still arrive: after
    // the final HTTP/1 response, or after HTTP/2 GOAWAY. Lingering close
    // (RFC 9112 §9.6): TLS close_notify, TCP send shutdown, then peer input is
    // read and discarded until its end, a deadline or a byte budget, so unread
    // input cannot make TCP reset the connection and destroy the last response.
    linger,
};

// Half-closes plain TCP after the final response. The session's close still
// lingers and closes the socket.
inline void shutdown_http_connection_send(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
}

// The lingering close of a plain connection, short of the final TCP close.
// The caller closes TCP after completion and must not start this operation
// once the worker is stopping; scanner close_all, a fixed deadline and the
// worker scanner bound it. The task borrows its arguments until completion.
[[nodiscard]] task<void> linger_http_connection(asio::ip::tcp::socket& socket, connection_scanner& scanner, const worker_ref& worker_value);

// Sends TLS close_notify for a non-aborted session without waiting for the
// peer's alert, then lingers like linger_http_connection() when `mode` is
// http_connection_close::linger. Same bounds and caller duties as above.
[[nodiscard]] task<void> close_http_connection_gracefully(server_tls_stream& stream, http_connection_close mode, connection_scanner& scanner, const worker_ref& worker_value);

}  // namespace ruvia::detail
