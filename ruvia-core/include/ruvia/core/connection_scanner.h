#pragma once

#include <cstdint>
#include <memory>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/connection_scanner_options.h"
#include "ruvia/core/worker_handle.h"

namespace ruvia {

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
    void register_entry(entry_type& entry, asio::ip::tcp::socket& socket) noexcept;
    // Multiplexed stream checks have no socket: scanner timeouts must not
    // close an unrelated connection on their behalf.
    void register_entry(entry_type& entry) noexcept;
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

}  // namespace ruvia
