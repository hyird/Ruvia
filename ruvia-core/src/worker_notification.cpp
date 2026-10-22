#include "ruvia/core/worker_notification.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/buffer.hpp>
#include <asio/error.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_runtime_context.h"

#include "worker_notification.h"

namespace ruvia::detail {

void* worker_notification_wait_resource::do_allocate(std::size_t bytes_value, std::size_t alignment) {
    if (allocated_ || bytes_value > buffer_size || alignment > max_alignment) {
        throw std::bad_alloc();
    }
    void* pointer = buffer_storage_.data();
    std::size_t space = buffer_storage_.size();
    if (std::align(alignment, bytes_value, pointer, space) == nullptr) {
        throw std::bad_alloc();
    }
    allocated_ = true;
    allocated_pointer_ = pointer;
    allocated_bytes_ = bytes_value;
    allocated_alignment_ = alignment;
    return pointer;
}

void worker_notification_wait_resource::do_deallocate(
    void* pointer, std::size_t bytes_value, std::size_t alignment) noexcept {
    if (!allocated_ || pointer != allocated_pointer_ || bytes_value != allocated_bytes_ ||
        alignment != allocated_alignment_) {
        std::terminate();
    }
    allocated_ = false;
    allocated_pointer_ = nullptr;
    allocated_bytes_ = 0;
    allocated_alignment_ = 0;
}

bool worker_notification_wait_resource::do_is_equal(
    const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

void throw_native_wake_error(const char* operation, int error) {
    throw std::system_error(error, std::system_category(), operation);
}

worker_notification_state::worker_notification_state(asio::io_context& io_context,
    const worker_handle& worker, worker_notification_options options)
    : io_context_(io_context),
      worker_(worker) {
    if (!worker_.valid() || !worker_.accepting()) {
        throw std::invalid_argument("worker notification requires an accepting worker runtime");
    }
    if (options.startup_timeout_ <= std::chrono::milliseconds::zero() ||
        options.startup_timeout_ > std::chrono::minutes(1)) {
        throw std::invalid_argument("worker notification startup timeout must be in (0, 1 minute]");
    }
    wake_ = make_native_wake_pair(options.startup_timeout_);
}

worker_notification_state::~worker_notification_state() {
    const auto activity = activity_.load(std::memory_order_acquire);
    if ((activity & count_mask) != 0 || wait_pending_ || wait_continuation_ ||
        receiver_stream_.has_value() || wait_resource_.allocated()) {
        std::terminate();
    }
    retire_receiver();
}

worker_notification_status worker_notification_state::notify() noexcept {
    if (!enter_producer()) {
        return worker_notification_status::closed;
    }
    struct producer_exit final {
        worker_notification_state& state_;
        ~producer_exit() {
            state_.leave_producer();
        }
    } producer_exit_value{*this};

    if (pending_.exchange(true, std::memory_order_acq_rel)) {
        return is_closed() ? worker_notification_status::closed : worker_notification_status::coalesced;
    }
    if (is_closed()) {
        return worker_notification_status::closed;
    }

    const auto signal = wake_.signal();
    if (is_closed()) {
        return worker_notification_status::closed;
    }
    switch (signal) {
        case native_wake_signal::signaled:
            return worker_notification_status::notified;
        case native_wake_signal::full:
            return worker_notification_status::coalesced;
        case native_wake_signal::failed:
            break;
    }
    std::terminate();
}

bool worker_notification_state::wait_ready() {
    require_worker();
    if (wait_pending_) {
        throw std::logic_error("worker notification permits one outstanding wait");
    }
    if (is_closed()) {
        wait_result_ = worker_notification_wait_status::closed;
        wait_error_.clear();
        return true;
    }
    ensure_receiver();
#ifdef _WIN32
    // Overlapped named-pipe reads are the readiness operation on IOCP. Even an
    // already-latched notification is consumed by the ensuing async_read_some.
    return false;
#else
    const bool received_value = drain_receiver(wait_error_);
    const bool latched = pending_.exchange(false, std::memory_order_acq_rel);
    if (wait_error_ || received_value || latched) {
        wait_result_ = worker_notification_wait_status::notified;
        return true;
    }
    return false;
#endif
}

bool worker_notification_state::begin_wait(std::coroutine_handle<> continuation) {
    if (wait_ready()) {
        return false;
    }

    wait_pending_ = true;
    wait_continuation_ = continuation;
    try {
        auto handler = asio::bind_allocator(
            std::pmr::polymorphic_allocator<std::byte>(&wait_resource_),
#ifdef _WIN32
            [this](const asio::error_code& error, std::size_t bytes_transferred) noexcept {
                complete_wait(error, bytes_transferred);
            }
#else
            [this](const asio::error_code& error) noexcept { complete_wait(error, 0); }
#endif
        );
#ifdef _WIN32
        receiver_stream_->async_read_some(asio::buffer(read_buffer_), std::move(handler));
#else
        receiver_stream_->async_wait(native_wake_stream::wait_read, std::move(handler));
#endif
    } catch (...) {
        wait_pending_ = false;
        wait_continuation_ = {};
        throw;
    }
    return true;
}

worker_notification_wait_status worker_notification_state::take_wait_result() {
    require_worker();
    if (wait_pending_) {
        std::terminate();
    }
    if (wait_error_) {
        const auto error = std::exchange(wait_error_, {});
        throw std::system_error(error, "worker notification wait");
    }
    return wait_result_;
}

void worker_notification_state::close() {
    require_worker();
    const auto old = activity_.fetch_or(closed, std::memory_order_acq_rel);
    if ((old & closed) != 0) {
        return;
    }

    if (receiver_stream_ && wait_pending_) {
        asio::error_code error;
        receiver_stream_->cancel(error);
        if (error) {
            std::terminate();
        }
#ifdef _WIN32
        // Keep the connected server HANDLE until IOCP reports the canceled read.
        return;
#else
        receiver_stream_->close(error);
        if (error) {
            std::terminate();
        }
        return;
#endif
    }
    retire_receiver();
}

bool worker_notification_state::enter_producer() noexcept {
    auto state_value = activity_.load(std::memory_order_acquire);
    for (;;) {
        if ((state_value & closed) != 0) {
            return false;
        }
        if ((state_value & count_mask) == count_mask) {
            std::terminate();
        }
        if (activity_.compare_exchange_weak(state_value, state_value + 1, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
}

void worker_notification_state::leave_producer() noexcept {
    const auto previous = activity_.fetch_sub(1, std::memory_order_release);
    if ((previous & count_mask) == 0) {
        std::terminate();
    }
}

bool worker_notification_state::is_closed() const noexcept {
    return (activity_.load(std::memory_order_acquire) & closed) != 0;
}

void worker_notification_state::require_worker() const {
    if (!worker_.is_current()) {
        throw std::logic_error("worker notification wait and close are worker-affine");
    }
}

void worker_notification_state::ensure_receiver() {
    if (receiver_stream_) {
        return;
    }
    try {
        receiver_stream_.emplace(io_context_);
        asio::error_code error;
        receiver_stream_->assign(wake_.receiver_, error);
        if (error) {
            throw std::system_error(error, "assign worker notification receiver");
        }
        wake_.receiver_ = invalid_native_wake_handle;
    } catch (...) {
        static_cast<void>(activity_.fetch_or(closed, std::memory_order_acq_rel));
        if (receiver_stream_) {
            asio::error_code ignored;
            if (receiver_stream_->is_open()) {
                receiver_stream_->close(ignored);
            }
            receiver_stream_.reset();
        }
        retire_receiver();
        throw;
    }
}

#ifndef _WIN32
bool worker_notification_state::drain_receiver(std::error_code& error) noexcept {
    if (!receiver_stream_ || !receiver_stream_->is_open()) {
        return false;
    }
    return drain_native_receiver(receiver_stream_->native_handle(), error);
}
#endif

void worker_notification_state::retire_receiver() noexcept {
    if (receiver_stream_) {
        asio::error_code error;
        if (receiver_stream_->is_open()) {
#ifdef _WIN32
            disconnect_native_receiver(receiver_stream_->native_handle());
#endif
            receiver_stream_->close(error);
            if (error) {
                std::terminate();
            }
        }
        receiver_stream_.reset();
    }
    wake_.close_receiver();
}

void worker_notification_state::complete_wait(
    const std::error_code& error, std::size_t bytes_transferred) noexcept {
    if (!wait_pending_ || !wait_continuation_) {
        std::terminate();
    }
    wait_pending_ = false;
    const auto continuation = std::exchange(wait_continuation_, {});

    if (is_closed()) {
        wait_error_.clear();
        wait_result_ = worker_notification_wait_status::closed;
        retire_receiver();
        continuation.resume();
        return;
    }

    if (error) {
        wait_error_ = error;
#ifdef _WIN32
    } else if (bytes_transferred == 0) {
        wait_error_ = std::make_error_code(std::errc::connection_reset);
#endif
    } else {
        static_cast<void>(bytes_transferred);
#ifndef _WIN32
        static_cast<void>(drain_receiver(wait_error_));
#endif
        static_cast<void>(pending_.exchange(false, std::memory_order_acq_rel));
    }
    wait_result_ = worker_notification_wait_status::notified;
    continuation.resume();
}

namespace {

// The only place a notification's state is built; both public constructors
// hand their runtime's io_context and handle to this entry.
[[nodiscard]] std::unique_ptr<worker_notification_state, pmr_object_deleter<worker_notification_state>>
make_worker_notification_state(
    asio::io_context& io_context, const worker_handle& worker, worker_notification_options options) {
    return make_pmr_object<worker_notification_state>(process_resource(), io_context, worker, options);
}

}  // namespace

}  // namespace ruvia::detail

namespace ruvia {

worker_notification::wait_awaiter_type::wait_awaiter_type(worker_notification& owner_value) noexcept
    : owner_(&owner_value) {}

bool worker_notification::wait_awaiter_type::await_ready() {
    return owner_->state_->wait_ready();
}

bool worker_notification::wait_awaiter_type::await_suspend(std::coroutine_handle<> continuation) {
    return owner_->state_->begin_wait(continuation);
}

worker_notification_wait_status worker_notification::wait_awaiter_type::await_resume() {
    return owner_->state_->take_wait_result();
}

worker_notification::worker_notification(state_pointer state) noexcept
    : state_(std::move(state)),
      wait_awaiter_(*this) {}

worker_notification::worker_notification(
    worker_runtime& runtime, worker_notification_options options)
    : worker_notification(detail::make_worker_notification_state(runtime.io_context(), runtime.handle(), options)) {}

worker_notification::worker_notification(
    worker_runtime_context& runtime, worker_notification_options options)
    : worker_notification(detail::make_worker_notification_state(runtime.io_context(), runtime.handle(), options)) {}

worker_notification::~worker_notification() = default;

worker_notification_status worker_notification::notify() noexcept {
    return state_->notify();
}

worker_notification::wait_awaiter_type& worker_notification::wait() noexcept {
    return wait_awaiter_;
}

void worker_notification::close() {
    state_->close();
}

}  // namespace ruvia
