#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"

namespace ruvia {
class worker_memory;
}

namespace ruvia::detail {

// Worker-owned HTTP/1 connection activity tracking options.
struct connection_scanner_options final {
    std::chrono::milliseconds scan_interval_{std::chrono::seconds(1)};
    // Inactivity timeouts measured from the connection's last successful I/O.
    // Absence disables the corresponding phase timeout.
    std::optional<std::chrono::milliseconds> idle_timeout_{};
    std::optional<std::chrono::milliseconds> initial_read_timeout_{};
    std::optional<std::chrono::milliseconds> payload_read_timeout_{};
    std::optional<std::chrono::milliseconds> write_timeout_{};
    // Absolute phase deadlines, independent of successful I/O progress.
    std::optional<std::chrono::milliseconds> initial_read_completion_timeout_{};
    std::optional<std::chrono::milliseconds> payload_read_completion_timeout_{};
    // A tick that follows a whole scan interval in which no entry carried a
    // request exchange and none had activity (I/O, registration or retirement)
    // trims this memory on the worker. Must belong to the worker.
    worker_memory* trim_when_idle_;
};

// Worker-affine connection timeout owner. Its implementation is allocated once
// per scanner; entries and checks remain caller-owned intrusive nodes, with no
// per-connection allocation by the scanner.
class connection_scanner final {
private:
    struct impl_type;

public:
    // Product-owned callbacks. A tick runs on the worker during each scan; a
    // drain callback runs on the worker when draining reaches the connection.
    using periodic_check_type = void (*)(void*, std::int64_t) noexcept;
    using drain_check_type = void (*)(void*) noexcept;

    enum class phase_type : std::uint8_t {
        idle,
        reading_initial,
        reading_payload,
        long_lived,
        writing,
    };

    class entry_type;

    // Per-connection node; callback registration is allocation-free.
    // The entry detaches any remaining nodes when it is destroyed.
    class periodic_check_registration_type final {
    public:
        periodic_check_registration_type() noexcept = default;
        ~periodic_check_registration_type() noexcept;
        periodic_check_registration_type(const periodic_check_registration_type&) = delete;
        periodic_check_registration_type& operator=(const periodic_check_registration_type&) = delete;
        periodic_check_registration_type(periodic_check_registration_type&&) = delete;
        periodic_check_registration_type& operator=(periodic_check_registration_type&&) = delete;
        void reset() noexcept;

    private:
        friend class connection_scanner;
        friend class entry_type;
        entry_type* entry_{nullptr};
        periodic_check_registration_type* prev_{nullptr};
        periodic_check_registration_type* next_{nullptr};
        void* target_{nullptr};
        periodic_check_type tick_{nullptr};
        drain_check_type drain_{nullptr};
    };

    // Connection-owned node. touch()/set_phase() use the scanner's
    // cached coarse time, not a per-request clock read; the scanner advances it
    // on every tick and discounts its one-interval lag before timing out.
    // Destruction detaches the entry and its checks, including during a periodic
    // callback. Destroying the scanner detaches its entries before releasing
    // its timestamp storage.
    class entry_type final {
    public:
        entry_type() noexcept = default;
        ~entry_type() noexcept;
        entry_type(const entry_type&) = delete;
        entry_type& operator=(const entry_type&) = delete;
        entry_type(entry_type&&) = delete;
        entry_type& operator=(entry_type&&) = delete;
        void touch() noexcept;
        // Not for long_lived: an upgrade enters that phase via enter_long_lived().
        // Ends a request-head read started by read_request_head().
        void set_phase(phase_type next_phase) noexcept;
        // Reads the head of the connection's next request, timed by
        // `timeout_phase`. No request is admitted until a later set_phase(), so
        // draining closes the connection meanwhile; touch() keeps the read open.
        void read_request_head(phase_type timeout_phase) noexcept;

        // Hands the entry back, when it ends, to the phase it had when it
        // began: same phase, same phase start and request-head state, with
        // the time of the end as activity. A write nested in another phase
        // (held output flushed in the middle of a handler) thus leaves that
        // phase's deadlines, and drain's view of the request, as they were.
        class phase_scope_type final {
        public:
            explicit phase_scope_type(entry_type& entry) noexcept
                : entry_(entry),
                  phase_started_ms_(entry.phase_started_ms_),
                  phase_(entry.phase_),
                  reading_request_head_(entry.reading_request_head_) {}
            ~phase_scope_type();
            phase_scope_type(const phase_scope_type&) = delete;
            phase_scope_type& operator=(const phase_scope_type&) = delete;

        private:
            entry_type& entry_;
            std::int64_t phase_started_ms_;
            phase_type phase_;
            bool reading_request_head_;
        };

        // Moves an established upgrade into the untimed long_lived phase. Once
        // the scanner drains, the upgrade is not admitted and false tells the
        // caller not to start the session's handler: an upgrade with a drain
        // callback is notified to end gracefully, any other has its socket closed.
        [[nodiscard]] bool enter_long_lived() noexcept;
        [[nodiscard]] std::int64_t last_active_ms() const noexcept;
        // `drain` is optional: a long-lived connection uses it to end itself
        // gracefully once draining starts.
        void register_periodic_check(periodic_check_registration_type& registration, void* target,
            periodic_check_type tick, drain_check_type drain = nullptr) noexcept;

    private:
        friend class connection_scanner;
        friend class periodic_check_registration_type;
        friend struct impl_type;
        [[nodiscard]] bool linked() const noexcept;
        [[nodiscard]] bool has_drain_check() const noexcept;
        void notify_drain() noexcept;
        void remove_periodic_check(periodic_check_registration_type& registration) noexcept;
        void detach_periodic_checks() noexcept;
        void* close_target_{};
        void (*close_)(void*) noexcept {};
        connection_scanner* scanner_{nullptr};
        entry_type* prev_{nullptr};
        entry_type* next_{nullptr};
        const std::int64_t* now_ms_{nullptr};
        std::int64_t last_active_ms_{0};
        std::int64_t phase_started_ms_{0};
        phase_type phase_{phase_type::idle};
        bool reading_request_head_{false};
        periodic_check_registration_type* periodic_checks_{nullptr};
        periodic_check_registration_type* periodic_scan_next_{nullptr};
    };

    // Binds a TCP socket to an entry for the guard lifetime; its destructor
    // safely detaches even if the scanner was destroyed first. An entry has one
    // registration owner: binding an already-bound entry throws
    // std::logic_error without changing its existing binding or checks.
    class guard_type final {
    public:
        guard_type(connection_scanner& scanner, entry_type& entry_value, asio::ip::tcp::socket& socket);
        guard_type(connection_scanner& scanner, entry_type& entry_value, void* target, void (*close)(void*) noexcept);
        ~guard_type();
        guard_type(const guard_type&) = delete;
        guard_type& operator=(const guard_type&) = delete;

    private:
        entry_type& entry_;
    };

    connection_scanner(worker_handle worker_value, connection_scanner_options options);
    // Detaches every entry and check. A periodic check may destroy its scanner
    // (or stop and restart it): the running scan visits no further node and
    // releases the remaining scanner state when it returns.
    // Destruction on another thread waits for an in-progress scan to finish.
    ~connection_scanner() noexcept;
    connection_scanner(const connection_scanner&) = delete;
    connection_scanner& operator=(const connection_scanner&) = delete;

    void start();
    void stop() noexcept;
    void close_all() noexcept;
    // Starts draining: closes connections that have no admitted request (those
    // waiting for or still reading a request head), notifies each long-lived
    // upgrade's drain callbacks, and from then on refuses every upgrade as it
    // reaches enter_long_lived(). Request exchanges already admitted, and
    // long-lived connections without a drain callback, keep running until the
    // worker stops.
    void drain() noexcept;

private:
    friend class entry_type;
    friend class guard_type;
    friend struct impl_type;
    static void detach_entry(entry_type& entry) noexcept;
    std::unique_ptr<impl_type> impl_;
};

struct connection_scanner::impl_type final {
    explicit impl_type(connection_scanner* owner_value, worker_handle worker_value, connection_scanner_options options);

    // Detaches every entry and registration and disarms the timer. Returns true
    // when called from inside this scanner's own scan callback: that scan still
    // runs on this stack and takes ownership of the impl, freeing it on return.
    [[nodiscard]] bool retire() noexcept;

    void start();
    void stop() noexcept;
    void register_entry(entry_type& entry, void* target, void (*close)(void*) noexcept);
    void unregister_entry(entry_type& entry) noexcept;
    void close_all() noexcept;
    void drain() noexcept;
    void schedule();
    // Visits every entry; returns whether none was in a request exchange or
    // active since `interval_start`.
    [[nodiscard]] bool scan(std::int64_t interval_start) noexcept;
    [[nodiscard]] bool is_timed_out(const entry_type& entry, std::int64_t now) const noexcept;
    [[nodiscard]] static bool active_since(const entry_type& entry, std::int64_t interval_start) noexcept;

    struct timer_state_type final {
        explicit timer_state_type(impl_type* owner_value) noexcept
            : owner_(owner_value) {}
        std::mutex mutex_;
        impl_type* owner_;
    };

    connection_scanner* owner_;
    worker_handle worker_;
    worker_timer_registration timer_;
    std::shared_ptr<timer_state_type> timer_state_;
    connection_scanner_options options_;
    std::int64_t cached_now_ms_{0};
    // Latest activity of an entry that has since been unregistered.
    std::int64_t retired_activity_ms_{(std::numeric_limits<std::int64_t>::min)()};
    entry_type sentinel_{};
    entry_type* scan_current_{nullptr};
    entry_type* scan_next_{nullptr};
    std::atomic_bool running_{false};
    // Worker-local: set only by the timer callback while scan() runs callbacks.
    bool scanning_{false};
    bool release_after_scan_{false};
    // Worker-local; set once by drain() and never cleared.
    bool draining_{false};
};

}  // namespace ruvia::detail
