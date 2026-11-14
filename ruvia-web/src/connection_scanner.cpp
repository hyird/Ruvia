#include "connection_scanner.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/ip/tcp.hpp>

#include "asio_bridge.h"
#include "connection_lifecycle.h"
#include "task.h"
#include "timeout_duration.h"
#include "worker_ref.h"
#include "worker_timer.h"

namespace ruvia::detail {

connection_transport connection_transport::tcp(asio::ip::tcp::socket& socket) noexcept {
    return connection_transport(&socket, [](void* target) noexcept {
        close_socket(*static_cast<asio::ip::tcp::socket*>(target));
    }, [](void* target) noexcept {
        std::error_code ignored;
        static_cast<asio::ip::tcp::socket*>(target)->cancel(ignored);
    });
}
namespace {

[[nodiscard]] worker_ref require_scanner_worker(worker_ref worker_value) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("connection scanner requires a valid worker");
    }
    return worker_value;
}

[[nodiscard]] bool timeout_expired(const std::optional<timeout_duration>& timeout, std::int64_t inactive_ms) noexcept {
    return timeout.has_value() && inactive_ms >= timeout->value().count();
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
    if (scanner_ != nullptr) {
        last_active_ms_ = steady_stamp_ms();
    }
}
void connection_scanner::entry::note_write_progress() noexcept {
    if (scanner_ != nullptr) {
        last_active_ms_ = steady_stamp_ms();
        write_progress_ms_ = last_active_ms_;
    }
}
void connection_scanner::entry::begin_next_write() noexcept {
    note_write_progress();
    write_started_ms_ = write_progress_ms_;
}
void connection_scanner::entry::set_phase(phase next_phase) noexcept {
    if (scanner_ == nullptr) {
        return;
    }
    const bool write_started = next_phase == phase::writing && !write_pending();
    touch();
    if (phase_ != next_phase) {
        phase_started_ms_ = last_active_ms_;
    }
    if (write_started) {
        write_progress_ms_ = last_active_ms_;
        write_started_ms_ = last_active_ms_;
    }
    phase_ = next_phase;
    // A head read that completed before its cancel took effect returns the
    // connection to its phase timeouts; the stale request has no deadline.
    if (reading_request_head_) {
        close_requested_ = false;
    }
    reading_request_head_ = false;
    sync_reusable();
}
void connection_scanner::entry::read_request_head(phase timeout_phase) noexcept {
    set_phase(timeout_phase);
    reading_request_head_ = scanner_ != nullptr;
    close_requested_ = false;
    sync_reusable();
}
connection_scanner::entry::phase_scope::~phase_scope() {
    if (entry_.scanner_ == nullptr) {
        return;
    }
    entry_.touch();
    entry_.phase_started_ms_ = phase_started_ms_;
    entry_.phase_ = phase_;
    entry_.reading_request_head_ = reading_request_head_;
    entry_.sync_reusable();
}
connection_scanner::entry::write_scope::write_scope(entry& entry_value) noexcept :
    entry_(entry_value) {
    if (!entry_.write_pending()) {
        entry_.begin_next_write();
    }
    ++entry_.write_scopes_;
}
connection_scanner::entry::write_scope::~write_scope() {
    --entry_.write_scopes_;
    entry_.touch();
}
void connection_scanner::entry::enter_long_lived() noexcept {
    set_phase(phase::long_lived);
}
std::int64_t connection_scanner::entry::last_active_ms() const noexcept {
    return last_active_ms_;
}
void connection_scanner::entry::register_periodic_check(periodic_check_registration& registration, void* target, periodic_check_type tick, drain_check_type drain) noexcept {
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
void connection_scanner::entry::remove_periodic_check(periodic_check_registration& registration) noexcept {
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
void connection_scanner::entry::sync_reusable() noexcept {
    const bool listed = reusable_next_ != nullptr;
    if (scanner_ == nullptr || reusable() == listed) {
        return;
    }
    if (listed) {
        unlink_reusable();
        return;
    }
    auto& sentinel = scanner_->impl_->reusable_sentinel_;
    reusable_prev_ = sentinel.reusable_prev_;
    reusable_next_ = &sentinel;
    sentinel.reusable_prev_->reusable_next_ = this;
    sentinel.reusable_prev_ = this;
}
void connection_scanner::entry::unlink_reusable() noexcept {
    if (reusable_next_ == nullptr) {
        return;
    }
    reusable_prev_->reusable_next_ = reusable_next_;
    reusable_next_->reusable_prev_ = reusable_prev_;
    reusable_prev_ = reusable_next_ = nullptr;
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
connection_scanner::guard::guard(connection_scanner& scanner, entry& entry_value, connection_transport transport_value) :
    entry_(entry_value) {
    scanner.impl_->register_entry(entry_, transport_value);
}
connection_scanner::guard::~guard() {
    connection_scanner::detach_entry(entry_);
}
connection_scanner::impl_type::impl_type(connection_scanner* owner_value, worker_ref worker_value, connection_scanner_options options) :
    owner_(owner_value),
    worker_(require_scanner_worker(std::move(worker_value))),
    timer_state_(std::make_shared<timer_state_type>(this)),
    options_(std::move(options)),
    cached_now_ms_(steady_now_ms()) {
    sentinel_.prev_ = sentinel_.next_ = &sentinel_;
    reusable_sentinel_.reusable_prev_ = reusable_sentinel_.reusable_next_ = &reusable_sentinel_;
}
bool connection_scanner::impl_type::retire() noexcept {
    const bool on_worker = worker_.is_current();
    bool nested_in_scan = false;
    {
        std::unique_lock lock(timer_state_->mutex_);
        if (on_worker) {
            // Rounds run only on the worker, so a running one is this call's own
            // caller further up the stack; waiting for it would never end.
            nested_in_scan = timer_state_->scanning_;
        } else {
            timer_state_->scan_ended_.wait(lock, [this] {
                return !timer_state_->scanning_;
            });
        }
        timer_state_->owner_ = nullptr;
    }
    // Outside the lock: cancelling destroys the pending completion. An expiry
    // in between finds no owner and starts no round.
    stop();
    release_after_scan_ = nested_in_scan;
    // Unregistration keeps the scan cursors consistent, so a nested scan ends
    // without visiting any detached node.
    while (sentinel_.next_ != &sentinel_) {
        unregister_entry(*sentinel_.next_);
    }
    return nested_in_scan;
}

connection_scanner::connection_scanner(worker_ref worker_value, connection_scanner_options options) :
    impl_(std::make_unique<impl_type>(this, std::move(worker_value), std::move(options))) {}
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
bool connection_scanner::evict_oldest_reusable() noexcept {
    auto& sentinel = impl_->reusable_sentinel_;
    for (auto* current = sentinel.reusable_next_; current != &sentinel; current = current->reusable_next_) {
        // A write still under way belongs to an exchange that only just
        // ended; its connection is not idle yet.
        if (current->write_pending()) {
            continue;
        }
        // Closed at once rather than asked to close: the new connection takes
        // the slot now. The close request keeps the entry out of the list
        // while its owner unwinds.
        if (current->lifecycle_ != nullptr) {
            current->lifecycle_->set_reason(connection_close_reason::aborted);
        }
        impl_type::record_close_request(*current, impl_->cached_now_ms_);
        current->transport_.close();
        return true;
    }
    return false;
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
void connection_scanner::impl_type::register_entry(entry& entry_value, connection_transport transport_value) {
    if (entry_value.scanner_ != nullptr) {
        throw std::logic_error("connection scanner entry is already registered");
    }
    // A parked scanner resumes with its first entry. Armed before linking, so
    // a failure leaves the entry unregistered.
    if (running_ && !timer_.registered()) {
        schedule();
    }
    entry_value.transport_ = transport_value;
    entry_value.scanner_ = owner_;
    entry_value.touch();
    entry_value.phase_started_ms_ = entry_value.last_active_ms_;
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
    entry_value.unlink_reusable();
    entry_value.transport_ = {};
    entry_value.detach_periodic_checks();
    entry_value.scanner_ = nullptr;
}
void connection_scanner::impl_type::close_all() noexcept {
    for (auto* current = sentinel_.next_; current != &sentinel_; current = current->next_) {
        if (current->lifecycle_ != nullptr) {
            current->lifecycle_->set_reason(connection_close_reason::cancelled);
        }
        current->transport_.close();
    }
}
void connection_scanner::impl_type::schedule() {
    if (!running_) {
        return;
    }
    const auto timer_state = timer_state_;
    // A stopped timer queue arms nothing: scanning ends with the worker, as it
    // does for a scan timer that stop_timers() cancels.
    (worker_).schedule_timer(timer_, ::ruvia::deadline_after(options_.scan_interval_.value()), [timer_state](worker_timer_outcome outcome) {
        if (outcome == worker_timer_outcome::cancelled) {
            return;
        }
        impl_type* scanner = nullptr;
        {
            std::lock_guard lock(timer_state->mutex_);
            scanner = timer_state->owner_;
            if (scanner == nullptr || !scanner->running_) {
                return;
            }
            // The flag, not the mutex, holds off retire() on other threads, so
            // the round's callbacks, closes and heap collection run unlocked.
            timer_state->scanning_ = true;
        }
        // Ends the round even when its re-arm throws, so a waiting retire()
        // always resumes.
        struct round_end_guard final {
            timer_state_type& state_;
            ~round_end_guard() noexcept {
                {
                    std::lock_guard lock(state_.mutex_);
                    state_.scanning_ = false;
                }
                state_.scan_ended_.notify_all();
            }
        };
        bool retired = false;
        {
            const round_end_guard round_end{*timer_state};
            retired = scanner->scan_round();
        }
        // Past the round's end a retire() on another thread may free the
        // scanner; only one its own callback retired is still ours to free.
        if (retired) {
            delete scanner;
        }
    });
}
bool connection_scanner::impl_type::scan_round() {
    // Expiry has consumed this timer. Release its token before callbacks,
    // which may stop, restart, or destroy the scanner.
    timer_.cancel();
    // 每轮采样一次比较时钟，供超时、协议检查和跨轮空闲判断共用。
    const auto interval_start = cached_now_ms_;
    cached_now_ms_ = steady_now_ms();
    const bool idle = scan(interval_start);
    if (release_after_scan_) {
        // A callback destroyed the scanner; its teardown already ran.
        return true;
    }
    // Never during an exchange or a burst: only after a whole interval in
    // which no entry carried a request exchange and none had activity (I/O,
    // registration or retirement), once per such quiet period. The timer runs
    // on the worker, whose thread heap this collects.
    if (idle && retired_activity_ms_ < interval_start) {
        if (!quiet_trimmed_) {
            collect_thread_heap();
            quiet_trimmed_ = true;
        }
    } else {
        quiet_trimmed_ = false;
    }
    // With no entry left and the quiet period trimmed, nothing remains to
    // time: park until register_entry() re-arms the scan.
    const bool parked = quiet_trimmed_ && sentinel_.next_ == &sentinel_;
    if (!parked && !timer_.registered()) {
        schedule();
    }
    return false;
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
    // exchange; neither has an upgraded one between messages, nor a
    // multiplexed session without a stream. Anything else (a handshake, an
    // admitted request, a response) is an exchange.
    return entry_value.last_active_ms_ >= interval_start || entry_value.write_pending() || (!entry_value.reading_request_head_ && entry_value.phase_ != phase::long_lived && entry_value.phase_ != phase::awaiting_stream);
}
void connection_scanner::impl_type::expire(entry& entry_value) noexcept {
    if (entry_value.lifecycle_ != nullptr) {
        entry_value.lifecycle_->set_reason(connection_close_reason::timeout);
    }
    if (!entry_value.close_requested_) {
        const auto now = cached_now_ms_;
        if (entry_value.reading_request_head_) {
            request_close(entry_value, steady_deadline_ms(now, options_.scan_interval_.value()));
            return;
        }
        // An overdue write cannot carry a graceful end (GOAWAY, close alert);
        // a write still within its limits is followed by the drain's own output.
        if (entry_value.phase_ != phase::writing && !write_overdue(entry_value, now) && entry_value.has_drain_check()) {
            // The callback may retire the entry; record the request first.
            record_close_request(entry_value, steady_deadline_ms(now, connection_close_grace));
            entry_value.notify_drain();
            return;
        }
    }
    entry_value.transport_.close();
}
void connection_scanner::impl_type::request_close(entry& entry_value, std::int64_t deadline_ms) noexcept {
    // The pending read completes with operation_aborted; its owner then ends
    // the connection with its close alert instead of an abrupt close. A TLS
    // read can lose the cancel (the ssl operation resumes reading a partial
    // record), so the deadline closes the socket of one that never returns.
    record_close_request(entry_value, deadline_ms);
    entry_value.transport_.cancel();
}
void connection_scanner::impl_type::record_close_request(entry& entry_value, std::int64_t deadline_ms) noexcept {
    // Repeated requests (a timeout, then a drain) keep the earliest deadline.
    if (!entry_value.close_requested_ || deadline_ms < entry_value.close_deadline_ms_) {
        entry_value.close_deadline_ms_ = deadline_ms;
    }
    entry_value.close_requested_ = true;
    entry_value.sync_reusable();
}
bool connection_scanner::impl_type::is_timed_out(const entry& entry_value, std::int64_t now) const noexcept {
    // A graceful close request is bounded by its own deadline, which later
    // activity (the GOAWAY or alert write it asked for) does not extend.
    if (entry_value.close_requested_) {
        return now >= entry_value.close_deadline_ms_;
    }
    if (write_overdue(entry_value, now)) {
        return true;
    }
    // 活动时间独立采样，不能把配置的扫描间隔当成调度延迟的上界。
    const auto inactive_ms = now - entry_value.last_active_ms_;
    const auto phase_elapsed_ms = now - entry_value.phase_started_ms_;
    switch (entry_value.phase_) {
        case phase::reading_initial:
            return timeout_expired(options_.initial_read_timeout_, inactive_ms) || timeout_expired(options_.initial_read_completion_timeout_, phase_elapsed_ms);
        case phase::reading_payload:
            return timeout_expired(options_.payload_read_timeout_, inactive_ms) || timeout_expired(options_.payload_read_completion_timeout_, phase_elapsed_ms);
        case phase::writing:
        case phase::long_lived:
            return false;
        case phase::awaiting_stream:
            return timeout_expired(options_.idle_timeout_, phase_elapsed_ms);
        case phase::idle:
        default:
            return timeout_expired(options_.idle_timeout_, inactive_ms);
    }
}
bool connection_scanner::impl_type::write_overdue(const entry& entry_value, std::int64_t now) const noexcept {
    return entry_value.write_pending() && (timeout_expired(options_.write_timeout_, now - entry_value.write_progress_ms_) || timeout_expired(options_.write_completion_timeout_, now - entry_value.write_started_ms_));
}

}  // namespace ruvia::detail
