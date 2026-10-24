#include "server/connection_scanner.h"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/socket.h"

#include "util/steady_clock.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] worker_handle require_scanner_worker(worker_handle worker_value) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("connection scanner requires a valid worker");
    }
    return worker_value;
}

void validate_scanner_timeout(const std::optional<std::chrono::milliseconds>& timeout) {
    if (timeout.has_value() && timeout->count() <= 0) {
        throw std::invalid_argument(
            "configured connection scanner timeouts must be greater than zero");
    }
}

[[nodiscard]] bool timeout_expired(
    const std::optional<std::chrono::milliseconds>& timeout, std::int64_t inactive_ms) noexcept {
    return timeout.has_value() && inactive_ms >= timeout->count();
}

}  // namespace

connection_scanner::periodic_check_registration::~periodic_check_registration() noexcept {
    reset();
}
void connection_scanner::periodic_check_registration::reset() noexcept {
    if (entry_ != nullptr) {
        entry_->remove_periodic_check(*this);
    }
}
connection_scanner::entry::~entry() noexcept {
    connection_scanner::detach_entry(*this);
    detach_periodic_checks();
}
void connection_scanner::entry::touch() noexcept {
    if (now_ms_ != nullptr) {
        last_active_ms_ = *now_ms_;
    }
}
void connection_scanner::entry::set_phase(phase next_phase) noexcept {
    if (now_ms_ == nullptr) {
        return;
    }
    last_active_ms_ = *now_ms_;
    if (phase_ != next_phase) {
        phase_started_ms_ = *now_ms_;
    }
    phase_ = next_phase;
    // A head read that completed before its cancel took effect returns the
    // connection to its phase timeouts; the stale request has no deadline.
    if (reading_request_head_) {
        close_requested_ = false;
    }
    reading_request_head_ = false;
}
void connection_scanner::entry::read_request_head(phase timeout_phase) noexcept {
    set_phase(timeout_phase);
    reading_request_head_ = now_ms_ != nullptr;
    close_requested_ = false;
}
connection_scanner::entry::phase_scope::~phase_scope() {
    if (entry_.now_ms_ == nullptr) {
        return;
    }
    entry_.last_active_ms_ = *entry_.now_ms_;
    entry_.phase_started_ms_ = phase_started_ms_;
    entry_.phase_ = phase_;
    entry_.reading_request_head_ = reading_request_head_;
}
bool connection_scanner::entry::enter_long_lived() noexcept {
    const bool draining = scanner_ != nullptr && scanner_->impl_->draining_;
    if (draining && !has_drain_check()) {
        close_socket(*socket_);
        return false;
    }
    set_phase(phase::long_lived);
    if (draining) {
        notify_drain();
        return false;
    }
    return true;
}
std::int64_t connection_scanner::entry::last_active_ms() const noexcept {
    return last_active_ms_;
}
void connection_scanner::entry::register_periodic_check(periodic_check_registration& registration,
    void* target, periodic_check_type tick, drain_check_type drain) noexcept {
    registration.reset();
    registration.entry_ = this;
    registration.target_ = target;
    registration.tick_ = tick;
    registration.drain_ = drain;
    registration.next_ = periodic_checks_;
    if (periodic_checks_ != nullptr) {
        periodic_checks_->prev_ = &registration;
    }
    periodic_checks_ = &registration;
}
void connection_scanner::entry::remove_periodic_check(
    periodic_check_registration& registration) noexcept {
    if (registration.entry_ != this) {
        return;
    }
    if (registration.prev_ != nullptr) {
        registration.prev_->next_ = registration.next_;
    } else {
        periodic_checks_ = registration.next_;
    }
    if (registration.next_ != nullptr) {
        registration.next_->prev_ = registration.prev_;
    }
    if (periodic_scan_next_ == &registration) {
        periodic_scan_next_ = registration.next_;
    }
    registration.entry_ = nullptr;
    registration.prev_ = registration.next_ = nullptr;
    registration.target_ = nullptr;
    registration.tick_ = nullptr;
    registration.drain_ = nullptr;
}
void connection_scanner::entry::detach_periodic_checks() noexcept {
    periodic_scan_next_ = nullptr;
    while (periodic_checks_ != nullptr) {
        periodic_checks_->reset();
    }
}
bool connection_scanner::entry::linked() const noexcept {
    return prev_ != nullptr && next_ != nullptr;
}
bool connection_scanner::entry::has_drain_check() const noexcept {
    for (const auto* registration = periodic_checks_; registration != nullptr; registration = registration->next_) {
        if (registration->drain_ != nullptr) {
            return true;
        }
    }
    return false;
}
void connection_scanner::entry::notify_drain() noexcept {
    // A callback may detach its own registration; read the successor first.
    for (auto* registration = periodic_checks_; registration != nullptr;) {
        auto* const next = registration->next_;
        if (registration->drain_ != nullptr) {
            registration->drain_(registration->target_);
        }
        registration = next;
    }
}
connection_scanner::guard::guard(
    connection_scanner& scanner, entry& entry_value, asio::ip::tcp::socket& socket)
    : entry_(entry_value) {
    scanner.impl_->register_entry(entry_, socket);
}
connection_scanner::guard::~guard() {
    connection_scanner::detach_entry(entry_);
}
connection_scanner::impl_type::impl_type(
    connection_scanner* owner_value, worker_handle worker_value, connection_scanner_options options)
    : owner_(owner_value),
      worker_(require_scanner_worker(std::move(worker_value))),
      timer_state_(std::make_shared<timer_state_type>(this)),
      options_(std::move(options)),
      cached_now_ms_(steady_now_ms()) {
    if (options_.scan_interval_.count() <= 0) {
        throw std::invalid_argument("connection scanner interval must be greater than zero");
    }
    validate_scanner_timeout(options_.idle_timeout_);
    validate_scanner_timeout(options_.initial_read_timeout_);
    validate_scanner_timeout(options_.payload_read_timeout_);
    validate_scanner_timeout(options_.write_timeout_);
    validate_scanner_timeout(options_.initial_read_completion_timeout_);
    validate_scanner_timeout(options_.payload_read_completion_timeout_);
    sentinel_.prev_ = sentinel_.next_ = &sentinel_;
}
bool connection_scanner::impl_type::retire() noexcept {
    stop();
    // scanning_ is worker-local; only the worker thread may observe it. On that
    // thread a set flag means this call is nested inside the scan callback,
    // which already holds the timer mutex.
    const bool nested_in_scan = worker_.is_current() && scanning_;
    if (nested_in_scan) {
        timer_state_->owner_ = nullptr;
        release_after_scan_ = true;
    } else {
        // Another thread may be scanning; wait for that callback to finish.
        std::lock_guard lock(timer_state_->mutex_);
        timer_state_->owner_ = nullptr;
    }
    // Unregistration keeps the scan cursors consistent, so a nested scan ends
    // without visiting any detached node.
    while (sentinel_.next_ != &sentinel_) {
        unregister_entry(*sentinel_.next_);
    }
    return nested_in_scan;
}

connection_scanner::connection_scanner(worker_handle worker_value, connection_scanner_options options)
    : impl_(std::make_unique<impl_type>(this, std::move(worker_value), std::move(options))) {}
connection_scanner::~connection_scanner() noexcept {
    if (impl_->retire()) {
        // The scan running further up this stack frees the impl.
        static_cast<void>(impl_.release());
    }
}
void connection_scanner::start() {
    impl_->start();
}
void connection_scanner::stop() noexcept {
    impl_->stop();
}
void connection_scanner::close_all() noexcept {
    impl_->close_all();
}
void connection_scanner::drain() noexcept {
    impl_->drain();
}
void connection_scanner::detach_entry(entry& entry_value) noexcept {
    if (entry_value.scanner_ != nullptr) {
        entry_value.scanner_->impl_->unregister_entry(entry_value);
    }
}

void connection_scanner::impl_type::start() {
    if (running_) {
        return;
    }
    running_ = true;
    cached_now_ms_ = steady_now_ms();
    try {
        schedule();
    } catch (...) {
        running_ = false;
        throw;
    }
}
void connection_scanner::impl_type::stop() noexcept {
    running_ = false;
    timer_.cancel();
}
void connection_scanner::impl_type::register_entry(entry& entry_value, asio::ip::tcp::socket& socket) {
    if (entry_value.scanner_ != nullptr) {
        throw std::logic_error("connection scanner entry is already registered");
    }
    entry_value.socket_ = &socket;
    entry_value.scanner_ = owner_;
    entry_value.now_ms_ = &cached_now_ms_;
    entry_value.touch();
    entry_value.phase_started_ms_ = cached_now_ms_;
    entry_value.phase_ = phase::idle;
    entry_value.close_requested_ = false;
    entry_value.next_ = sentinel_.next_;
    entry_value.prev_ = &sentinel_;
    sentinel_.next_->prev_ = &entry_value;
    sentinel_.next_ = &entry_value;
}
void connection_scanner::impl_type::unregister_entry(entry& entry_value) noexcept {
    if (entry_value.scanner_ != owner_ || !entry_value.linked()) {
        return;
    }
    if (entry_value.last_active_ms_ > retired_activity_ms_) {
        retired_activity_ms_ = entry_value.last_active_ms_;
    }
    if (scan_current_ == &entry_value) {
        scan_current_ = nullptr;
    }
    if (scan_next_ == &entry_value) {
        scan_next_ = entry_value.next_;
    }
    entry_value.prev_->next_ = entry_value.next_;
    entry_value.next_->prev_ = entry_value.prev_;
    entry_value.prev_ = entry_value.next_ = nullptr;
    entry_value.socket_ = nullptr;
    entry_value.now_ms_ = nullptr;
    entry_value.detach_periodic_checks();
    entry_value.scanner_ = nullptr;
}
void connection_scanner::impl_type::close_all() noexcept {
    for (auto* current = sentinel_.next_; current != &sentinel_; current = current->next_) {
        close_socket(*current->socket_);
    }
}
void connection_scanner::impl_type::drain() noexcept {
    draining_ = true;
    // The coarse clock may lag by an interval; a lost cancel is closed by the
    // first scan at least one whole interval after this drain.
    const auto close_deadline = steady_now_ms() + options_.scan_interval_.count();
    for (auto* current = sentinel_.next_; current != &sentinel_;) {
        auto* const next = current->next_;
        if (current->reading_request_head_) {
            request_close(*current, close_deadline);
        } else {
            current->notify_drain();
        }
        current = next;
    }
}
void connection_scanner::impl_type::schedule() {
    if (!running_) {
        return;
    }
    const auto timer_state = timer_state_;
    // A stopped timer queue arms nothing: scanning ends with the worker, as it
    // does for a scan timer that stop_timers() cancels.
    (worker_).schedule_timer(timer_, ::ruvia::worker_timer_deadline_after(options_.scan_interval_), [timer_state](worker_timer_outcome outcome) {
        if (outcome == worker_timer_outcome::cancelled) {
            return;
        }
        std::lock_guard lock(timer_state->mutex_);
        auto* scanner = timer_state->owner_;
        if (scanner == nullptr || !scanner->running_) {
            return;
        }
        // Expiry has consumed this timer. Release its token before callbacks,
        // which may stop, restart, or destroy the scanner.
        scanner->timer_.cancel();
        // Every tick advances the coarse clock, even without timeouts or
        // checks: an entry registered later must not be stamped with a stale time.
        // Activity during the interval that just ended was stamped with the
        // previous tick's time.
        const auto interval_start = scanner->cached_now_ms_;
        scanner->cached_now_ms_ = steady_now_ms();
        scanner->scanning_ = true;
        const bool idle = scanner->scan(interval_start);
        scanner->scanning_ = false;
        if (scanner->release_after_scan_) {
            // A callback destroyed the scanner; its teardown already ran.
            delete scanner;
            return;
        }
        // Never during an exchange or a burst: only after a whole interval
        // without either. The timer runs on the worker that owns the memory.
        if (idle && scanner->retired_activity_ms_ < interval_start) {
            scanner->options_.trim_when_idle_->trim(scanner->worker_);
        }
        if (!scanner->timer_.registered()) {
            scanner->schedule();
        }
    });
}
bool connection_scanner::impl_type::scan(std::int64_t interval_start) noexcept {
    const auto now = cached_now_ms_;
    bool idle = true;
    scan_next_ = sentinel_.next_;
    while (scan_next_ != &sentinel_) {
        scan_current_ = scan_next_;
        scan_next_ = scan_current_->next_;
        // Checked before this entry's callbacks, which may touch it at the new time.
        idle = idle && !active_since(*scan_current_, interval_start);
        scan_current_->periodic_scan_next_ = scan_current_->periodic_checks_;
        // A callback can retire the current entry or a later one. Unregistration
        // updates both scanner-owned cursors before the entry can be destroyed.
        while (scan_current_ != nullptr && scan_current_->periodic_scan_next_ != nullptr) {
            auto* registration = scan_current_->periodic_scan_next_;
            scan_current_->periodic_scan_next_ = registration->next_;
            registration->tick_(registration->target_, now);
        }
        if (scan_current_ != nullptr && is_timed_out(*scan_current_, now)) {
            expire(*scan_current_);
        }
        scan_current_ = nullptr;
    }
    scan_next_ = nullptr;
    return idle;
}
bool connection_scanner::impl_type::active_since(const entry& entry_value, std::int64_t interval_start) noexcept {
    // A connection waiting for, or still reading, a request head has no
    // exchange; neither has an upgraded one between messages. Anything else
    // (a handshake, an admitted request, a response) is an exchange.
    return entry_value.last_active_ms_ >= interval_start ||
           (!entry_value.reading_request_head_ && entry_value.phase_ != phase::long_lived);
}
void connection_scanner::impl_type::expire(entry& entry_value) noexcept {
    if (!entry_value.close_requested_) {
        const auto now = cached_now_ms_;
        if (entry_value.reading_request_head_) {
            request_close(entry_value, now + options_.scan_interval_.count());
            return;
        }
        // A stalled write cannot carry a graceful end (GOAWAY, close alert).
        if (entry_value.phase_ != phase::writing && entry_value.has_drain_check()) {
            // The callback may retire the entry; record the request first.
            record_close_request(entry_value, now + connection_close_grace.count());
            entry_value.notify_drain();
            return;
        }
    }
    close_socket(*entry_value.socket_);
}
void connection_scanner::impl_type::request_close(entry& entry_value, std::int64_t deadline_ms) noexcept {
    // The pending read completes with operation_aborted; its owner then ends
    // the connection with its close alert instead of an abrupt close. A TLS
    // read can lose the cancel (the ssl operation resumes reading a partial
    // record), so the deadline closes the socket of one that never returns.
    record_close_request(entry_value, deadline_ms);
    std::error_code ignored;
    entry_value.socket_->cancel(ignored);
}
void connection_scanner::impl_type::record_close_request(entry& entry_value, std::int64_t deadline_ms) noexcept {
    // Repeated requests (a timeout, then a drain) keep the earliest deadline.
    if (!entry_value.close_requested_ || deadline_ms < entry_value.close_deadline_ms_) {
        entry_value.close_deadline_ms_ = deadline_ms;
    }
    entry_value.close_requested_ = true;
}
bool connection_scanner::impl_type::is_timed_out(const entry& entry_value, std::int64_t now) const noexcept {
    // A graceful close request is bounded by its own deadline, which later
    // activity (the GOAWAY or alert write it asked for) does not extend.
    if (entry_value.close_requested_) {
        return now >= entry_value.close_deadline_ms_;
    }
    // Stamps come from the coarse clock, which lags the activity it records by
    // up to one scan interval. Discounting that lag keeps every timeout from
    // firing before the configured time has really elapsed.
    const auto resolution_ms = options_.scan_interval_.count();
    const auto inactive_ms = now - entry_value.last_active_ms_ - resolution_ms;
    const auto phase_elapsed_ms = now - entry_value.phase_started_ms_ - resolution_ms;
    switch (entry_value.phase_) {
        case phase::reading_initial:
            return timeout_expired(options_.initial_read_timeout_, inactive_ms) ||
                   timeout_expired(options_.initial_read_completion_timeout_, phase_elapsed_ms);
        case phase::reading_payload:
            return timeout_expired(options_.payload_read_timeout_, inactive_ms) ||
                   timeout_expired(options_.payload_read_completion_timeout_, phase_elapsed_ms);
        case phase::writing:
            return timeout_expired(options_.write_timeout_, inactive_ms);
        case phase::long_lived:
            return false;
        case phase::idle:
        default:
            return timeout_expired(options_.idle_timeout_, inactive_ms);
    }
}

}  // namespace ruvia::detail
