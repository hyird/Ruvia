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

// Product limit: a connection that a timeout asked to end through its drain
// callback (an HTTP/2 GOAWAY drain, bounded by its own round-trip limit) has
// this long to do so before the scanner closes its socket, whatever its
// activity in the meantime.
inline constexpr std::chrono::milliseconds connection_close_grace{std::chrono::seconds(2)};

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

    enum class phase : std::uint8_t {
        idle,
        reading_initial,
        reading_payload,
        long_lived,
        writing,
    };

    class entry;

    // Per-connection node; callback registration is allocation-free.
    // The entry detaches any remaining nodes when it is destroyed.
    class periodic_check_registration final {
    public:
        periodic_check_registration() noexcept = default;
        ~periodic_check_registration() noexcept;
        periodic_check_registration(const periodic_check_registration&) = delete;
        periodic_check_registration& operator=(const periodic_check_registration&) = delete;
        periodic_check_registration(periodic_check_registration&&) = delete;
        periodic_check_registration& operator=(periodic_check_registration&&) = delete;
        void reset() noexcept;

    private:
        friend class connection_scanner;
        friend class entry;
        entry* entry_{nullptr};
        periodic_check_registration* prev_{nullptr};
        periodic_check_registration* next_{nullptr};
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
    class entry final {
    public:
        entry() noexcept = default;
        ~entry() noexcept;
        entry(const entry&) = delete;
        entry& operator=(const entry&) = delete;
        entry(entry&&) = delete;
        entry& operator=(entry&&) = delete;
        void touch() noexcept;
        // Not for long_lived: an upgrade enters that phase via enter_long_lived().
        // Ends a request-head read started by read_request_head().
        void set_phase(phase next_phase) noexcept;
        // Reads the head of the connection's next request, timed by
        // `timeout_phase`. No request is admitted until a later set_phase(), so
        // draining ends the connection meanwhile; touch() keeps the read open.
        // A timeout or drain cancels the read and sets close_requested().
        void read_request_head(phase timeout_phase) noexcept;
        // The scanner asked the connection to end gracefully: it cancelled a
        // request-head read, or invoked a drain callback for a timeout. The
        // owner closes without waiting for further input. Activity no longer
        // extends the connection: the scanner closes its socket once the
        // request's grace passes -- one scan interval for a cancelled read
        // (covering a cancel the transport lost), connection_close_grace for
        // a drain callback.
        [[nodiscard]] bool close_requested() const noexcept {
            return close_requested_;
        }

        // Hands the entry back, when it ends, to the phase it had when it
        // began: same phase, same phase start and request-head state, with
        // the time of the end as activity. A write nested in another phase
        // (held output flushed in the middle of a handler) thus leaves that
        // phase's deadlines, and drain's view of the request, as they were.
        class phase_scope final {
        public:
            explicit phase_scope(entry& entry_value) noexcept
                : entry_(entry_value),
                  phase_started_ms_(entry_value.phase_started_ms_),
                  phase_(entry_value.phase_),
                  reading_request_head_(entry_value.reading_request_head_) {}
            phase_scope(entry& entry_value, phase next_phase) noexcept
                : phase_scope(entry_value) {
                entry_.set_phase(next_phase);
            }
            ~phase_scope();
            phase_scope(const phase_scope&) = delete;
            phase_scope& operator=(const phase_scope&) = delete;

        private:
            entry& entry_;
            std::int64_t phase_started_ms_;
            phase phase_;
            bool reading_request_head_;
        };

        // Moves an established upgrade into the untimed long_lived phase. Once
        // the scanner drains, the upgrade is not admitted and false tells the
        // caller not to start the session's handler: an upgrade with a drain
        // callback is notified to end gracefully, any other has its socket closed.
        [[nodiscard]] bool enter_long_lived() noexcept;
        [[nodiscard]] std::int64_t last_active_ms() const noexcept;
        // `drain` is optional: a long-lived connection uses it to end itself
        // gracefully once draining starts. A connection whose read or idle
        // phase times out is asked the same way once, instead of being closed.
        void register_periodic_check(periodic_check_registration& registration, void* target,
            periodic_check_type tick, drain_check_type drain = nullptr) noexcept;

    private:
        friend class connection_scanner;
        friend class periodic_check_registration;
        friend struct impl_type;
        [[nodiscard]] bool linked() const noexcept;
        [[nodiscard]] bool has_drain_check() const noexcept;
        void notify_drain() noexcept;
        void remove_periodic_check(periodic_check_registration& registration) noexcept;
        void detach_periodic_checks() noexcept;
        asio::ip::tcp::socket* socket_{nullptr};
        connection_scanner* scanner_{nullptr};
        entry* prev_{nullptr};
        entry* next_{nullptr};
        const std::int64_t* now_ms_{nullptr};
        std::int64_t last_active_ms_{0};
        std::int64_t phase_started_ms_{0};
        // Valid while close_requested_: when the scanner closes the socket.
        std::int64_t close_deadline_ms_{0};
        phase phase_{phase::idle};
        bool reading_request_head_{false};
        bool close_requested_{false};
        periodic_check_registration* periodic_checks_{nullptr};
        periodic_check_registration* periodic_scan_next_{nullptr};
    };

    // Binds a TCP socket to an entry for the guard lifetime; its destructor
    // safely detaches even if the scanner was destroyed first. An entry has one
    // registration owner: binding an already-bound entry throws
    // std::logic_error without changing its existing binding or checks.
    class guard final {
    public:
        guard(connection_scanner& scanner, entry& entry_value, asio::ip::tcp::socket& socket);
        ~guard();
        guard(const guard&) = delete;
        guard& operator=(const guard&) = delete;

    private:
        entry& entry_;
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
    // Starts draining: cancels the read of connections waiting for or still
    // reading an unadmitted HTTP/1 request head (see close_requested(); a
    // later scan closes the socket of one whose cancel was lost), then
    // notifies registered protocol drain callbacks on all admitted sessions
    // (including multiplexed HTTP/2).
    // From then on enter_long_lived() refuses new upgrades. Admitted requests
    // and connections without a callback may finish until the worker stops.
    void drain() noexcept;

private:
    friend class entry;
    friend class guard;
    friend struct impl_type;
    static void detach_entry(entry& entry) noexcept;
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
    void register_entry(entry& entry, asio::ip::tcp::socket& socket);
    void unregister_entry(entry& entry) noexcept;
    void close_all() noexcept;
    void drain() noexcept;
    void schedule();
    // Visits every entry; returns whether none was in a request exchange or
    // active since `interval_start`.
    [[nodiscard]] bool scan(std::int64_t interval_start) noexcept;
    [[nodiscard]] bool is_timed_out(const entry& entry, std::int64_t now) const noexcept;
    [[nodiscard]] static bool active_since(const entry& entry, std::int64_t interval_start) noexcept;
    // Ends a timed-out connection: the first time gracefully when its owner
    // can (a pending request-head read, or a drain callback outside writing),
    // otherwise -- including once a graceful request's grace has passed -- by
    // closing its socket.
    void expire(entry& entry) noexcept;
    // Cancels the entry's pending read and closes its socket at `deadline_ms`
    // unless the owner ended the connection first.
    static void request_close(entry& entry, std::int64_t deadline_ms) noexcept;
    static void record_close_request(entry& entry, std::int64_t deadline_ms) noexcept;
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
    entry sentinel_{};
    entry* scan_current_{nullptr};
    entry* scan_next_{nullptr};
    std::atomic_bool running_{false};
    // Worker-local: set only by the timer callback while scan() runs callbacks.
    bool scanning_{false};
    bool release_after_scan_{false};
    // Worker-local; set once by drain() and never cleared.
    bool draining_{false};
};

}  // namespace ruvia::detail
