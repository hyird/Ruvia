#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"

namespace ruvia {

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
    void register_entry(entry_type& entry, asio::ip::tcp::socket* socket) noexcept;
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

}  // namespace ruvia
