#include "worker_notification.h"

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>

#include "event_loop.h"
#include "native_wake_pair.h"
#include "worker_ref.h"
#include "worker_runtime.h"

namespace ruvia::detail {

// One operation at a time uses this startup-owned storage as its Asio
// associated allocator. Asio destroys the operation (and deallocates this slot)
// before invoking its completion handler, so an immediately re-armed wait can
// reuse it without allocating or overlapping the previous operation.
class worker_notification_wait_resource final {
public:
    [[nodiscard]] bool allocated() const noexcept {
        return allocation_ != nullptr;
    }
    void* allocate(std::size_t bytes, std::size_t alignment);
    void deallocate(void* pointer, std::size_t bytes, std::size_t alignment) noexcept;

private:
    static constexpr std::size_t buffer_size = 1024;
    static constexpr std::size_t max_alignment = 64;

    // Leave room to align the operation without over-aligning the resource or its owner.
    std::array<std::byte, buffer_size + max_alignment - 1> buffer_storage_{};
    void* allocation_{nullptr};
};

template <typename t_type>
class worker_notification_wait_allocator final {
public:
    using value_type = t_type;

    explicit worker_notification_wait_allocator(worker_notification_wait_resource& resource) noexcept :
        resource_(&resource) {}

    template <typename other_type>
    worker_notification_wait_allocator(const worker_notification_wait_allocator<other_type>& other) noexcept :
        resource_(other.resource_) {}

    [[nodiscard]] t_type* allocate(std::size_t count) const {
        return static_cast<t_type*>(resource_->allocate(count * sizeof(t_type), alignof(t_type)));
    }

    void deallocate(t_type* pointer, std::size_t count) const noexcept {
        resource_->deallocate(pointer, count * sizeof(t_type), alignof(t_type));
    }

    template <typename other_type>
    [[nodiscard]] bool operator==(const worker_notification_wait_allocator<other_type>& other) const noexcept {
        return resource_ == other.resource_;
    }

private:
    template <typename>
    friend class worker_notification_wait_allocator;

    worker_notification_wait_resource* resource_;
};

class worker_notification_state final {
public:
    worker_notification_state(asio::io_context& io_context, const worker_ref& worker, worker_notification_options options);
    ~worker_notification_state();

    worker_notification_state(const worker_notification_state&) = delete;
    worker_notification_state& operator=(const worker_notification_state&) = delete;

    [[nodiscard]] worker_notification_status notify() noexcept;
    [[nodiscard]] bool wait_ready();
    [[nodiscard]] bool begin_wait(std::coroutine_handle<> continuation);
    [[nodiscard]] worker_notification_wait_status take_wait_result();
    void close();

private:
    static constexpr std::uint64_t closed = std::uint64_t{1} << 63;
    static constexpr std::uint64_t count_mask = ~closed;

    [[nodiscard]] bool enter_producer() noexcept;
    void leave_producer() noexcept;
    [[nodiscard]] bool is_closed() const noexcept;
    void require_worker() const;
    void ensure_receiver();
#ifndef _WIN32
    [[nodiscard]] bool drain_receiver(std::error_code& error) noexcept;
#endif
    void retire_receiver() noexcept;
    void complete_wait(const std::error_code& error, std::size_t bytes_transferred) noexcept;

    asio::io_context& io_context_;
    const worker_ref& worker_;
    native_wake_pair wake_;
#ifdef _WIN32
    std::array<char, 4096> read_buffer_{};
#endif
    std::atomic<std::uint64_t> activity_{0};
    std::atomic<bool> pending_{false};
    worker_notification_wait_resource wait_resource_;
    std::optional<native_wake_stream> receiver_stream_;
    std::coroutine_handle<> wait_continuation_{};
    std::error_code wait_error_;
    worker_notification_wait_status wait_result_{worker_notification_wait_status::notified};
    bool wait_pending_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

void* worker_notification_wait_resource::allocate(std::size_t bytes_value, std::size_t alignment) {
    if (allocated() || bytes_value > buffer_size || alignment > max_alignment) {
        throw std::bad_alloc();
    }
    void* pointer = buffer_storage_.data();
    auto space = buffer_storage_.size();
    allocation_ = std::align(alignment, bytes_value, pointer, space);
    if (allocation_ == nullptr) {
        throw std::bad_alloc();
    }
    return allocation_;
}

void worker_notification_wait_resource::deallocate(void* pointer, std::size_t, std::size_t) noexcept {
    if (!allocated() || pointer != allocation_) {
        std::terminate();
    }
    allocation_ = nullptr;
}

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

worker_notification_state::worker_notification_state(asio::io_context& io_context, const worker_ref& worker, worker_notification_options options) :
    io_context_(io_context),
    worker_(worker) {
    if (!worker_.valid() || !worker_.accepting()) {
        throw std::invalid_argument("worker notification requires an accepting worker runtime");
    }
    if (options.startup_timeout_.value() <= std::chrono::milliseconds::zero() || options.startup_timeout_.value() > std::chrono::minutes(1)) {
        throw std::invalid_argument("worker notification startup timeout must be in (0, 1 minute]");
    }
    wake_ = make_native_wake_pair(options.startup_timeout_.value());
}

worker_notification_state::~worker_notification_state() {
    const auto activity = activity_.load(std::memory_order_acquire);
    if ((activity & count_mask) != 0 || wait_pending_ || wait_continuation_ || receiver_stream_.has_value() || wait_resource_.allocated()) {
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
    require_worker();
    if (wait_pending_) {
        throw std::logic_error("worker notification permits one outstanding wait");
    }
    if (is_closed()) {
        wait_result_ = worker_notification_wait_status::closed;
        wait_error_.clear();
        return false;
    }
    // A signal arriving after await_ready remains readable by the native
    // async wait; polling the receiver again only adds an empty system call.

    wait_pending_ = true;
    wait_continuation_ = continuation;
    try {
        auto handler = asio::bind_allocator(worker_notification_wait_allocator<std::byte>(wait_resource_),
#ifdef _WIN32
            [this](const asio::error_code& error, std::size_t bytes_transferred) noexcept {
            complete_wait(error, bytes_transferred);
        }
#else
            [this](const asio::error_code& error) noexcept {
            complete_wait(error, 0);
        }
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
        if (activity_.compare_exchange_weak(state_value, state_value + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
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
        wake_.receiver_ = invalid_native_wake_descriptor;
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

void worker_notification_state::complete_wait(const std::error_code& error, std::size_t bytes_transferred) noexcept {
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

// The only place a notification's state is built; every public constructor
// hands its runtime's io_context and worker_ref to this entry.
[[nodiscard]] std::unique_ptr<worker_notification_state> make_worker_notification_state(asio::io_context& io_context, const worker_ref& worker, worker_notification_options options) {
    return std::make_unique<worker_notification_state>(io_context, worker, options);
}

}  // namespace

}  // namespace ruvia::detail

namespace ruvia {

worker_notification::wait_operation::wait_operation(worker_notification& owner_value) noexcept :
    owner_(&owner_value) {}

bool worker_notification::wait_operation::await_ready() {
    return owner_->state_->wait_ready();
}

bool worker_notification::wait_operation::await_suspend(std::coroutine_handle<> continuation) {
    return owner_->state_->begin_wait(continuation);
}

worker_notification_wait_status worker_notification::wait_operation::await_resume() {
    return owner_->state_->take_wait_result();
}

worker_notification::worker_notification(state_pointer state) noexcept :
    state_(std::move(state)),
    wait_awaiter_(*this) {}

worker_notification::worker_notification(worker_runtime& runtime, worker_notification_options options) :
    worker_notification(detail::make_worker_notification_state(runtime.io_context(), runtime.worker(), options)) {}

worker_notification::worker_notification(const event_loop& loop, worker_notification_options options) :
    worker_notification(detail::make_worker_notification_state(loop.io_context(), loop.worker(), options)) {}
worker_notification::~worker_notification() = default;

worker_notification_status worker_notification::notify() noexcept {
    return state_->notify();
}

worker_notification::wait_operation& worker_notification::wait() & noexcept {
    return wait_awaiter_;
}

void worker_notification::close() {
    state_->close();
}

}  // namespace ruvia
