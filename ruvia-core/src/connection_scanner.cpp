#include "connection_scanner.h"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "ruvia/core/socket.h"

namespace ruvia {
namespace {

[[nodiscard]] std::int64_t steady_now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

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

connection_scanner::worker_maintenance_registration_type::~worker_maintenance_registration_type() noexcept {
    reset();
}
void connection_scanner::worker_maintenance_registration_type::reset() noexcept {
    if (scanner_ != nullptr) {
        scanner_->remove_worker_maintenance(*this);
    }
}
connection_scanner::periodic_check_registration_type::~periodic_check_registration_type() noexcept {
    reset();
}
void connection_scanner::periodic_check_registration_type::reset() noexcept {
    if (entry_ != nullptr) {
        entry_->remove_periodic_check(*this);
    }
}
connection_scanner::entry_type::~entry_type() noexcept {
    connection_scanner::detach_entry(*this);
    detach_periodic_checks();
}
void connection_scanner::entry_type::touch() noexcept {
    if (now_ms_ != nullptr) {
        last_active_ms_ = *now_ms_;
    }
}
void connection_scanner::entry_type::set_phase(phase_type next_phase) noexcept {
    if (now_ms_ == nullptr) {
        return;
    }
    last_active_ms_ = *now_ms_;
    if (phase_ != next_phase) {
        phase_started_ms_ = *now_ms_;
    }
    phase_ = next_phase;
}
std::int64_t connection_scanner::entry_type::last_active_ms() const noexcept {
    return last_active_ms_;
}
void connection_scanner::entry_type::register_periodic_check(
    periodic_check_registration_type& registration, void* target, periodic_check_type tick) noexcept {
    registration.reset();
    if (target == nullptr || tick == nullptr) {
        return;
    }
    registration.entry_ = this;
    registration.target_ = target;
    registration.tick_ = tick;
    registration.next_ = periodic_checks_;
    if (periodic_checks_ != nullptr) {
        periodic_checks_->prev_ = &registration;
    }
    periodic_checks_ = &registration;
    if (scanner_ != nullptr) {
        scanner_->periodic_check_added();
    }
}
void connection_scanner::entry_type::remove_periodic_check(
    periodic_check_registration_type& registration) noexcept {
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
    if (scanner_ != nullptr) {
        scanner_->periodic_check_removed();
    }
    registration.entry_ = nullptr;
    registration.prev_ = registration.next_ = nullptr;
    registration.target_ = nullptr;
    registration.tick_ = nullptr;
}
void connection_scanner::entry_type::detach_periodic_checks() noexcept {
    periodic_scan_next_ = nullptr;
    while (periodic_checks_ != nullptr) {
        periodic_checks_->reset();
    }
}
bool connection_scanner::entry_type::linked() const noexcept {
    return prev_ != nullptr && next_ != nullptr;
}
connection_scanner::guard_type::guard_type(
    connection_scanner* scanner, entry_type& entry_value, asio::ip::tcp::socket& socket)
    : entry_(scanner != nullptr ? &entry_value : nullptr) {
    if (scanner != nullptr) {
        scanner->register_entry(*entry_, socket);
    }
}
connection_scanner::guard_type::~guard_type() {
    if (entry_ != nullptr) {
        connection_scanner::detach_entry(*entry_);
    }
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
connection_scanner::impl_type::~impl_type() noexcept {
    stop();
    {
        std::lock_guard lock(timer_state_->mutex_);
        timer_state_->owner_ = nullptr;
    }
    while (sentinel_.next_ != &sentinel_) {
        unregister_entry(*sentinel_.next_);
    }
    detach_worker_maintenance();
}

connection_scanner::connection_scanner(worker_handle worker_value, connection_scanner_options options)
    : impl_(std::make_unique<impl_type>(this, std::move(worker_value), std::move(options))) {}
connection_scanner::~connection_scanner() noexcept = default;
const worker_handle& connection_scanner::worker() const& noexcept {
    return impl_->worker_;
}
void connection_scanner::start() {
    impl_->start();
}
void connection_scanner::stop() noexcept {
    impl_->stop();
}
void connection_scanner::register_worker_maintenance(worker_maintenance_registration_type& registration,
    void* target, worker_maintenance_check_type check) noexcept {
    impl_->register_worker_maintenance(registration, target, check);
}
void connection_scanner::register_entry(entry_type& entry_value, asio::ip::tcp::socket& socket) noexcept {
    impl_->register_entry(entry_value, &socket);
}
void connection_scanner::register_entry(entry_type& entry_value) noexcept {
    impl_->register_entry(entry_value, nullptr);
}
void connection_scanner::unregister_entry(entry_type& entry_value) noexcept {
    impl_->unregister_entry(entry_value);
}
void connection_scanner::close_all() noexcept {
    impl_->close_all();
}
void connection_scanner::detach_entry(entry_type& entry_value) noexcept {
    if (entry_value.scanner_ != nullptr) {
        entry_value.scanner_->impl_->unregister_entry(entry_value);
    }
}
void connection_scanner::periodic_check_added() noexcept {
    impl_->periodic_check_added();
}
void connection_scanner::periodic_check_removed() noexcept {
    impl_->periodic_check_removed();
}
void connection_scanner::remove_worker_maintenance(worker_maintenance_registration_type& registration) noexcept {
    impl_->remove_worker_maintenance(registration);
}

void connection_scanner::impl_type::start() {
    if (running_) {
        return;
    }
    running_ = true;
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
void connection_scanner::impl_type::register_worker_maintenance(worker_maintenance_registration_type& registration,
    void* target, worker_maintenance_check_type check) noexcept {
    registration.reset();
    if (target == nullptr || check == nullptr) {
        return;
    }
    registration.scanner_ = owner_;
    registration.target_ = target;
    registration.check_ = check;
    registration.next_ = worker_maintenance_;
    if (worker_maintenance_ != nullptr) {
        worker_maintenance_->prev_ = &registration;
    }
    worker_maintenance_ = &registration;
}
void connection_scanner::impl_type::register_entry(entry_type& entry_value, asio::ip::tcp::socket* socket) noexcept {
    entry_value.socket_ = socket;
    entry_value.scanner_ = owner_;
    entry_value.now_ms_ = &cached_now_ms_;
    entry_value.touch();
    entry_value.phase_started_ms_ = cached_now_ms_;
    entry_value.phase_ = phase_type::idle;
    entry_value.next_ = sentinel_.next_;
    entry_value.prev_ = &sentinel_;
    sentinel_.next_->prev_ = &entry_value;
    sentinel_.next_ = &entry_value;
    for (auto* registration = entry_value.periodic_checks_; registration != nullptr;
        registration = registration->next_) {
        periodic_check_added();
    }
}
void connection_scanner::impl_type::unregister_entry(entry_type& entry_value) noexcept {
    if (entry_value.scanner_ != owner_ || !entry_value.linked()) {
        return;
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
        if (current->socket_ != nullptr) {
            close_socket(*current->socket_);
        }
    }
}
void connection_scanner::impl_type::periodic_check_added() noexcept {
    ++periodic_check_count_;
}
void connection_scanner::impl_type::periodic_check_removed() noexcept {
    if (periodic_check_count_ > 0) {
        --periodic_check_count_;
    }
}
void connection_scanner::impl_type::remove_worker_maintenance(
    worker_maintenance_registration_type& registration) noexcept {
    if (registration.scanner_ != owner_) {
        return;
    }
    if (registration.prev_ != nullptr) {
        registration.prev_->next_ = registration.next_;
    } else {
        worker_maintenance_ = registration.next_;
    }
    if (registration.next_ != nullptr) {
        registration.next_->prev_ = registration.prev_;
    }
    if (worker_maintenance_scan_next_ == &registration) {
        worker_maintenance_scan_next_ = registration.next_;
    }
    registration.scanner_ = nullptr;
    registration.prev_ = registration.next_ = nullptr;
    registration.target_ = nullptr;
    registration.check_ = nullptr;
}
void connection_scanner::impl_type::detach_worker_maintenance() noexcept {
    worker_maintenance_scan_next_ = nullptr;
    while (worker_maintenance_ != nullptr) {
        auto* registration = worker_maintenance_;
        worker_maintenance_ = registration->next_;
        registration->scanner_ = nullptr;
        registration->prev_ = registration->next_ = nullptr;
        registration->target_ = nullptr;
        registration->check_ = nullptr;
    }
}
bool connection_scanner::impl_type::has_scanning_work() const noexcept {
    return options_.idle_timeout_.has_value() || options_.initial_read_timeout_.has_value() ||
           options_.payload_read_timeout_.has_value() || options_.write_timeout_.has_value() ||
           options_.initial_read_completion_timeout_.has_value() ||
           options_.payload_read_completion_timeout_.has_value() ||
           worker_maintenance_ != nullptr || periodic_check_count_ != 0;
}
void connection_scanner::impl_type::schedule() {
    if (!running_) {
        return;
    }
    const auto timer_state = timer_state_;
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
        // which may stop and restart the scanner with a new registration.
        scanner->timer_.cancel_quietly();
        if (scanner->has_scanning_work()) {
            scanner->scan();
        }
        if (!scanner->timer_.registered()) {
            scanner->schedule();
        }
    });
}
void connection_scanner::impl_type::scan() noexcept {
    const auto now = steady_now_ms();
    cached_now_ms_ = now;
    worker_maintenance_scan_next_ = worker_maintenance_;
    while (worker_maintenance_scan_next_ != nullptr) {
        auto* registration = worker_maintenance_scan_next_;
        worker_maintenance_scan_next_ = registration->next_;
        registration->check_(registration->target_);
    }
    scan_next_ = sentinel_.next_;
    while (scan_next_ != &sentinel_) {
        scan_current_ = scan_next_;
        scan_next_ = scan_current_->next_;
        scan_current_->periodic_scan_next_ = scan_current_->periodic_checks_;
        // A callback can retire the current entry or a later one. Unregistration
        // updates both scanner-owned cursors before the entry can be destroyed.
        while (scan_current_ != nullptr && scan_current_->periodic_scan_next_ != nullptr) {
            auto* registration = scan_current_->periodic_scan_next_;
            scan_current_->periodic_scan_next_ = registration->next_;
            registration->tick_(registration->target_, now);
        }
        if (scan_current_ != nullptr && scan_current_->socket_ != nullptr &&
            is_timed_out(*scan_current_, now)) {
            close_socket(*scan_current_->socket_);
        }
        scan_current_ = nullptr;
    }
    scan_next_ = nullptr;
}
bool connection_scanner::impl_type::is_timed_out(const entry_type& entry_value, std::int64_t now) const noexcept {
    const auto inactive_ms = now - entry_value.last_active_ms_;
    switch (entry_value.phase_) {
        case phase_type::reading_initial:
            return timeout_expired(options_.initial_read_timeout_, inactive_ms) ||
                   timeout_expired(options_.initial_read_completion_timeout_, now - entry_value.phase_started_ms_);
        case phase_type::reading_payload:
            return timeout_expired(options_.payload_read_timeout_, inactive_ms) ||
                   timeout_expired(options_.payload_read_completion_timeout_, now - entry_value.phase_started_ms_);
        case phase_type::writing:
            return timeout_expired(options_.write_timeout_, inactive_ms);
        case phase_type::long_lived:
            return false;
        case phase_type::idle:
        default:
            return timeout_expired(options_.idle_timeout_, inactive_ms);
    }
}

}  // namespace ruvia
