#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"

namespace ruvia::detail {

// Worker-owned connection activity tracking options shared by protocol runtimes.
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
};

// Worker-affine connection timeout and maintenance owner. Its implementation
// is allocated once per scanner; entries and registrations remain caller-owned
// intrusive nodes, with no per-connection allocation by the scanner.
class connection_scanner final {
private:
    struct impl_type;

public:
    // Product-owned checks operate on their own transport. A check belonging
    // to one multiplexed stream must not close its connection's socket.
    using periodic_check_type = void (*)(void*, std::int64_t) noexcept;
    using worker_maintenance_check_type = void (*)(void*) noexcept;

    enum class phase_type : std::uint8_t {
        idle,
        reading_initial,
        reading_payload,
        long_lived,
        writing,
    };

    class entry_type;

    // Startup-owned worker maintenance node; the caller keeps it alive while
    // registered. reset() is safe after its scanner has been destroyed.
    class worker_maintenance_registration_type final {
    public:
        worker_maintenance_registration_type() noexcept = default;
        ~worker_maintenance_registration_type() noexcept;
        worker_maintenance_registration_type(const worker_maintenance_registration_type&) = delete;
        worker_maintenance_registration_type& operator=(const worker_maintenance_registration_type&) = delete;
        worker_maintenance_registration_type(worker_maintenance_registration_type&&) = delete;
        worker_maintenance_registration_type& operator=(worker_maintenance_registration_type&&) = delete;
        void reset() noexcept;

    private:
        friend class connection_scanner;
        friend struct impl_type;
        connection_scanner* scanner_{nullptr};
        worker_maintenance_registration_type* prev_{nullptr};
        worker_maintenance_registration_type* next_{nullptr};
        void* target_{nullptr};
        worker_maintenance_check_type check_{nullptr};
    };

    // Per-connection or per-stream node; callback registration is allocation-free.
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
    };

    // Connection- or stream-owned node. touch()/set_phase() use the scanner's
    // cached coarse time, not a per-request clock read. Destruction detaches the
    // entry and its checks, including during a periodic callback. Destroying the
    // scanner detaches its entries before releasing its timestamp storage.
    class entry_type final {
    public:
        entry_type() noexcept = default;
        ~entry_type() noexcept;
        entry_type(const entry_type&) = delete;
        entry_type& operator=(const entry_type&) = delete;
        entry_type(entry_type&&) = delete;
        entry_type& operator=(entry_type&&) = delete;
        void touch() noexcept;
        void set_phase(phase_type next_phase) noexcept;
        [[nodiscard]] std::int64_t last_active_ms() const noexcept;
        void register_periodic_check(
            periodic_check_registration_type& registration, void* target, periodic_check_type tick) noexcept;

    private:
        friend class connection_scanner;
        friend class periodic_check_registration_type;
        friend struct impl_type;
        [[nodiscard]] bool linked() const noexcept;
        void remove_periodic_check(periodic_check_registration_type& registration) noexcept;
        void detach_periodic_checks() noexcept;
        asio::ip::tcp::socket* socket_{nullptr};
        connection_scanner* scanner_{nullptr};
        entry_type* prev_{nullptr};
        entry_type* next_{nullptr};
        const std::int64_t* now_ms_{nullptr};
        std::int64_t last_active_ms_{0};
        std::int64_t phase_started_ms_{0};
        phase_type phase_{phase_type::idle};
        periodic_check_registration_type* periodic_checks_{nullptr};
        periodic_check_registration_type* periodic_scan_next_{nullptr};
    };

    // Binds a TCP socket to an entry for the guard lifetime; its destructor
    // safely detaches even if the scanner was destroyed first.
    class guard_type final {
    public:
        guard_type(connection_scanner* scanner, entry_type& entry_value, asio::ip::tcp::socket& socket);
        ~guard_type();
        guard_type(const guard_type&) = delete;
        guard_type& operator=(const guard_type&) = delete;

    private:
        entry_type* entry_;
    };

    connection_scanner(worker_handle worker_value, connection_scanner_options options);
    // Detaches every entry and registration. A check or maintenance callback may
    // destroy its scanner (or stop and restart it): the running scan visits no
    // further node and releases the remaining scanner state when it returns.
    // Destruction on another thread waits for an in-progress scan to finish.
    ~connection_scanner() noexcept;
    connection_scanner(const connection_scanner&) = delete;
    connection_scanner& operator=(const connection_scanner&) = delete;

    [[nodiscard]] const worker_handle& worker() const& noexcept;
    const worker_handle& worker() const&& = delete;
    void start();
    void stop() noexcept;
    void register_worker_maintenance(worker_maintenance_registration_type& registration, void* target,
        worker_maintenance_check_type check) noexcept;
    // An entry has one registration owner. Registering an already-bound entry
    // throws std::logic_error without changing its existing binding or checks.
    // Unregister it (or release its guard) before registering it again.
    void register_entry(entry_type& entry, asio::ip::tcp::socket& socket);
    // Multiplexed stream checks have no socket: scanner timeouts must not
    // close an unrelated connection on their behalf.
    void register_entry(entry_type& entry);
    void unregister_entry(entry_type& entry) noexcept;
    void close_all() noexcept;

private:
    friend class entry_type;
    friend class worker_maintenance_registration_type;
    friend class guard_type;
    friend struct impl_type;
    static void detach_entry(entry_type& entry) noexcept;
    void remove_worker_maintenance(worker_maintenance_registration_type& registration) noexcept;
    void periodic_check_added() noexcept;
    void periodic_check_removed() noexcept;
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
    void register_worker_maintenance(worker_maintenance_registration_type& registration, void* target,
        worker_maintenance_check_type check) noexcept;
    void register_entry(entry_type& entry, asio::ip::tcp::socket* socket);
    void unregister_entry(entry_type& entry) noexcept;
    void close_all() noexcept;
    void periodic_check_added() noexcept;
    void periodic_check_removed() noexcept;
    void remove_worker_maintenance(worker_maintenance_registration_type& registration) noexcept;
    void detach_worker_maintenance() noexcept;
    [[nodiscard]] bool has_scanning_work() const noexcept;
    void schedule();
    void scan() noexcept;
    [[nodiscard]] bool is_timed_out(const entry_type& entry, std::int64_t now) const noexcept;

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
    entry_type sentinel_{};
    entry_type* scan_current_{nullptr};
    entry_type* scan_next_{nullptr};
    worker_maintenance_registration_type* worker_maintenance_{nullptr};
    worker_maintenance_registration_type* worker_maintenance_scan_next_{nullptr};
    std::size_t periodic_check_count_{0};
    std::atomic_bool running_{false};
    // Worker-local: set only by the timer callback while scan() runs callbacks.
    bool scanning_{false};
    bool release_after_scan_{false};
};

}  // namespace ruvia::detail
