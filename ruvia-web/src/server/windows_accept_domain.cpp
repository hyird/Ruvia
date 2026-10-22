#include "server/windows_accept_domain.h"

#if defined(_WIN32)

#include <mswsock.h>

#include <array>
#include <cassert>
#include <chrono>
#include <limits>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/bind_allocator.hpp>
#include <asio/steady_timer.hpp>
#include <asio/windows/overlapped_ptr.hpp>

#include "ruvia/core/worker_notification.h"
#include "ruvia/core/worker_runtime.h"

namespace ruvia::detail {
namespace {

std::size_t checked_product(std::size_t left, std::size_t right) {
    if (right != 0 && left > (std::numeric_limits<std::size_t>::max)() / right) {
        throw std::length_error("Windows accept matrix is too large");
    }
    return left * right;
}

// Asio's public associated allocator stores the native wrapper here. Its free
// happens on the listener worker, before our pointer-only completion is called.
// The storage is slot-private and is not reusable until origin consumption.
class operation_storage final : public std::pmr::memory_resource {
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > storage_.size() || alignment > alignof(std::max_align_t)) {
            throw std::bad_alloc();
        }
        return storage_.data();
    }
    void do_deallocate(void*, std::size_t, std::size_t) override {}
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    alignas(std::max_align_t) std::array<std::byte, 1024> storage_{};
};

enum class error_action { retry,
    report,
    fail };

error_action classify_error(const asio::error_code& error) noexcept {
    if (error == asio::error::connection_aborted || error == asio::error::connection_reset ||
        error == asio::error::eof || error == asio::error::try_again ||
        error == asio::error::would_block || error == asio::error::interrupted ||
        (error.category() == asio::system_category() && error.value() == ERROR_NETNAME_DELETED)) {
        return error_action::retry;
    }
    if (error == asio::error::no_descriptors || error == asio::error::no_buffer_space ||
        error == asio::error::no_memory || error == std::errc::too_many_files_open_in_system ||
        error == std::errc::protocol_error || error == asio::error::network_down ||
        error == asio::error::network_unreachable || error == asio::error::host_unreachable ||
        error == asio::error::no_protocol_option || error == asio::error::operation_not_supported ||
        error == asio::error::no_permission || error == asio::error::timed_out) {
        return error_action::report;
    }
    return error_action::fail;
}

}  // namespace

class windows_accept_domain::impl final {
public:
    struct descriptor final {
        SOCKET handle_{INVALID_SOCKET};
        LPFN_ACCEPTEX accept_{};
        std::optional<asio::io_context::executor_type> executor_;
        asio::ip::tcp protocol_{asio::ip::tcp::v4()};
        std::size_t logical_{};
    };
    struct origin;
    enum class slot_state { idle,
        pending,
        ready,
        consuming };
    struct slot final {
        impl* domain_{};
        origin* origin_{};
        descriptor* listener_{};
        operation_storage storage_;
        std::optional<asio::ip::tcp::socket> socket_;
        std::array<std::byte, 2 * (sizeof(sockaddr_storage) + 16)> addresses_{};
        OVERLAPPED* overlapped_{};
        asio::error_code error_;
        slot_state state_{slot_state::idle};
        bool canceled_{};
        std::chrono::steady_clock::time_point retry_{};
    };
    struct origin final {
        origin(impl& domain, worker_runtime& runtime, callbacks completion)
            : domain_(domain),
              runtime_(runtime),
              completion_(completion),
              wake_(runtime) {}
        impl& domain_;
        worker_runtime& runtime_;
        callbacks completion_;
        worker_notification wake_;
        std::optional<asio::steady_timer> retry_timer_;
        operation_storage timer_storage_;
        slot* slots_{};
        bool serving_{};
        bool closed_{};
        bool running_{};
        bool joined_{};
        bool timer_pending_{};
        bool timer_canceling_{};
        std::chrono::steady_clock::time_point timer_deadline_{};
        asio::error_code timer_error_;
    };

    impl(std::size_t workers, std::size_t listeners, std::pmr::memory_resource* resource)
        : worker_count_(workers),
          listener_count_(listeners),
          descriptors_(checked_product(workers, listeners), resource),
          slot_count_(checked_product(workers, descriptors_.size())),
          slots_(slot_count_, resource),
          origins_(workers, resource) {
        if (workers == 0 || listeners == 0) {
            throw std::invalid_argument("Windows accept matrix requires workers and listeners");
        }
    }

    void notify_locked(origin& receiver) noexcept {
        if (!receiver.closed_) {
            (void)receiver.wake_.notify();
        }
    }
    void notify_all_locked() noexcept {
        for (auto& receiver : origins_) {
            if (receiver) {
                notify_locked(*receiver);
            }
        }
    }
    void stop() noexcept {
        const std::lock_guard lock(mutex_);
        if (!stopping_) {
            stopping_ = true;
            notify_all_locked();
        }
    }
    void report(origin& receiver, const asio::error_code& error, const char* message, bool fatal) noexcept {
        try {
            throw std::system_error(error, message);
        } catch (...) {
            receiver.completion_.failure_(receiver.completion_.target_, std::current_exception(), fatal);
        }
        if (fatal) {
            stop();
        }
    }
    void failed(origin& receiver, std::exception_ptr failure) noexcept {
        receiver.completion_.failure_(receiver.completion_.target_, std::move(failure), true);
        stop();
    }

    // Runs on the listener's IOCP. There is no origin-owned object in the
    // handler: moving/destruction/deallocation of it is foreign-thread safe.
    static void completed(slot* operation, const asio::error_code& error, std::size_t) noexcept {
        auto& domain = *operation->domain_;
        const std::lock_guard lock(domain.mutex_);
        assert(operation->state_ == slot_state::pending);
        operation->error_ = error;
        operation->state_ = slot_state::ready;
        domain.notify_locked(*operation->origin_);
        // The mutex is released only after notify returns. A consumer that
        // retires this credit therefore also joins every producer-side access.
    }

    void issue(origin& receiver, slot& operation) {
        assert(receiver.runtime_.handle().is_current());
        // Socket creation and IOCP association are exclusively origin work.
        operation.socket_.emplace(receiver.runtime_.io_context());
        asio::error_code open_error;
        operation.socket_->open(operation.listener_->protocol_, open_error);
        const std::lock_guard lock(mutex_);
        if (stopping_ || !receiver.serving_) {
            operation.socket_.reset();
            return;
        }
        if (open_error) {
            operation.error_ = open_error;
            operation.state_ = slot_state::ready;
            ++live_;
            notify_locked(receiver);
            return;
        }
        auto handler = asio::bind_allocator(
            std::pmr::polymorphic_allocator<std::byte>(&operation.storage_),
            [pointer = &operation](const asio::error_code& error, std::size_t bytes) noexcept {
                completed(pointer, error, bytes);
            });
        // This executor is deliberately the LISTENER's executor. It balances
        // IOCP native work even when the initiating socket belongs elsewhere.
        asio::windows::overlapped_ptr native(*operation.listener_->executor_, std::move(handler));
        operation.overlapped_ = native.get();
        operation.canceled_ = false;
        operation.state_ = slot_state::pending;
        ++live_;
        DWORD bytes = 0;
        const BOOL success = operation.listener_->accept_(operation.listener_->handle_,
            operation.socket_->native_handle(), operation.addresses_.data(), 0,
            static_cast<DWORD>(sizeof(sockaddr_storage) + 16),
            static_cast<DWORD>(sizeof(sockaddr_storage) + 16), &bytes, native.get());
        const int error = success ? 0 : ::WSAGetLastError();
        if (success || error == WSA_IO_PENDING) {
            // Even synchronous success produces the normal IOCP packet.
            (void)native.release();
        } else {
            native.complete(asio::error_code(error, asio::system_category()), 0);
        }
    }

    void consume(origin& receiver, slot& operation) noexcept {
        asio::error_code error;
        bool stopping;
        {
            const std::lock_guard lock(mutex_);
            if (operation.state_ != slot_state::ready) {
                return;
            }
            operation.state_ = slot_state::consuming;
            error = operation.error_;
            stopping = stopping_;
        }
        if (!stopping && !error) {
            const auto listener = operation.listener_->handle_;
            if (::setsockopt(operation.socket_->native_handle(), SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                    reinterpret_cast<const char*>(&listener), static_cast<int>(sizeof(listener))) == SOCKET_ERROR) {
                error = asio::error_code(::WSAGetLastError(), asio::system_category());
            }
        }
        if (!stopping) {
            if (!error) {
                try {
                    receiver.completion_.accepted_(receiver.completion_.target_,
                        operation.listener_->logical_, std::move(*operation.socket_));
                } catch (...) {
                    failed(receiver, std::current_exception());
                }
            } else {
                const auto action = classify_error(error);
                if (action != error_action::retry) {
                    report(receiver, error, action == error_action::fail ? "fatal TCP AcceptEx error" : "recoverable TCP AcceptEx error",
                        action == error_action::fail);
                }
                operation.retry_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
            }
        }
        operation.socket_.reset();
        {
            const std::lock_guard lock(mutex_);
            operation.overlapped_ = nullptr;
            operation.state_ = slot_state::idle;
            assert(live_ != 0);
            --live_;
            if (stopping_ && live_ == 0) {
                notify_all_locked();
            }
        }
    }

    void cancel(origin& receiver, slot& operation) noexcept {
        asio::error_code error;
        {
            const std::lock_guard lock(mutex_);
            if (operation.state_ != slot_state::pending || operation.canceled_) {
                return;
            }
            operation.canceled_ = true;
            // The dedicated slot address cannot be recycled for another
            // request, including after native deallocation but before consume.
            if (!::CancelIoEx(reinterpret_cast<HANDLE>(operation.listener_->handle_), operation.overlapped_)) {
                const auto value = ::GetLastError();
                if (value != ERROR_NOT_FOUND) {
                    error = asio::error_code(static_cast<int>(value), asio::system_category());
                }
            }
        }
        if (error) {
            report(receiver, error, "cancel TCP AcceptEx", true);
        }
    }

    void update_timer(origin& receiver, std::chrono::steady_clock::time_point deadline, bool stopping) {
        if (receiver.timer_pending_) {
            if ((stopping || deadline != receiver.timer_deadline_) && !receiver.timer_canceling_) {
                receiver.timer_canceling_ = true;
                asio::error_code error;
                receiver.retry_timer_->cancel(error);
                if (error) {
                    report(receiver, error, "cancel TCP accept retry", true);
                }
            }
            return;
        }
        if (stopping || deadline == (std::chrono::steady_clock::time_point::max)()) {
            return;
        }
        receiver.retry_timer_->expires_at(deadline);
        receiver.timer_deadline_ = deadline;
        receiver.timer_pending_ = true;
        receiver.timer_canceling_ = false;
        try {
            receiver.retry_timer_->async_wait(asio::bind_allocator(
                std::pmr::polymorphic_allocator<std::byte>(&receiver.timer_storage_),
                [pointer = &receiver](const asio::error_code& error) noexcept {
                    pointer->timer_pending_ = false;
                    pointer->timer_canceling_ = false;
                    if (error != asio::error::operation_aborted) {
                        pointer->timer_error_ = error;
                    }
                    const std::lock_guard lock(pointer->domain_.mutex_);
                    pointer->domain_.notify_locked(*pointer);
                }));
        } catch (...) {
            receiver.timer_pending_ = false;
            throw;
        }
    }

    task<void> run(origin& receiver) {
        assert(receiver.runtime_.handle().is_current());
        if (receiver.running_ || receiver.joined_) {
            throw std::logic_error("Windows accept receiver can run only once");
        }
        receiver.running_ = true;
        try {
            receiver.retry_timer_.emplace(receiver.runtime_.io_context());
        } catch (...) {
            failed(receiver, std::current_exception());
        }
        for (;;) {
            bool stopping;
            bool serving;
            {
                const std::lock_guard lock(mutex_);
                stopping = stopping_;
                serving = receiver.serving_;
            }
            if (receiver.timer_error_) {
                report(receiver, std::exchange(receiver.timer_error_, {}), "TCP accept retry failed", true);
                stopping = true;
            }
            auto deadline = (std::chrono::steady_clock::time_point::max)();
            for (std::size_t index = 0; index < descriptors_.size(); ++index) {
                auto& operation = receiver.slots_[index];
                consume(receiver, operation);
                if (stopping) {
                    cancel(receiver, operation);
                    continue;
                }
                bool idle;
                {
                    const std::lock_guard lock(mutex_);
                    idle = operation.state_ == slot_state::idle;
                    stopping = stopping_;
                }
                if (stopping) {
                    cancel(receiver, operation);
                } else if (serving && idle) {
                    if (operation.retry_ > std::chrono::steady_clock::now()) {
                        if (operation.retry_ < deadline) {
                            deadline = operation.retry_;
                        }
                    } else {
                        try {
                            issue(receiver, operation);
                        } catch (...) {
                            operation.socket_.reset();
                            failed(receiver, std::current_exception());
                            stopping = true;
                        }
                    }
                }
            }
            try {
                update_timer(receiver, deadline, stopping);
            } catch (...) {
                failed(receiver, std::current_exception());
            }
            {
                const std::lock_guard lock(mutex_);
                if (stopping_ && live_ == 0 && !receiver.timer_pending_) {
                    // All callbacks have left their publish/notify critical
                    // section, all results (including accept-context work)
                    // were consumed, and this origin's timer producer joined.
                    receiver.closed_ = true;
                    receiver.wake_.close();
                    break;
                }
            }
            try {
                (void)co_await receiver.wake_.wait();
            } catch (...) {
                failed(receiver, std::current_exception());
            }
        }
        // close is normally reached between waits; explicitly observe closed
        // so no suspended notification operation can survive this task.
        (void)co_await receiver.wake_.wait();
        receiver.retry_timer_.reset();
        receiver.running_ = false;
        receiver.joined_ = true;
    }

    std::mutex mutex_;
    const std::size_t worker_count_;
    const std::size_t listener_count_;
    std::pmr::vector<descriptor> descriptors_;
    const std::size_t slot_count_;
    std::pmr::vector<slot> slots_;
    std::pmr::vector<std::unique_ptr<origin, pmr_object_deleter<origin>>> origins_;
    std::size_t live_{};
    bool published_{};
    bool stopping_{};
};

windows_accept_domain::windows_accept_domain(std::size_t workers, std::size_t listeners,
    std::pmr::memory_resource* resource)
    : impl_(make_pmr_object<impl>(resource, workers, listeners, resource)) {}
windows_accept_domain::~windows_accept_domain() = default;

void windows_accept_domain::attach(std::size_t index, worker_runtime& runtime, callbacks completion) {
    if (index >= impl_->worker_count_ || impl_->origins_[index] || !completion.accepted_ || !completion.failure_) {
        throw std::logic_error("invalid Windows accept origin attachment");
    }
    auto receiver = make_pmr_object<impl::origin>(impl_->origins_.get_allocator().resource(), *impl_, runtime, completion);
    receiver->slots_ = impl_->slots_.data() + index * impl_->descriptors_.size();
    for (std::size_t listener = 0; listener < impl_->descriptors_.size(); ++listener) {
        auto& operation = receiver->slots_[listener];
        operation.domain_ = impl_.get();
        operation.origin_ = receiver.get();
        operation.listener_ = &impl_->descriptors_[listener];
    }
    impl_->origins_[index] = std::move(receiver);
}

void windows_accept_domain::detach(std::size_t index) noexcept {
    const std::lock_guard lock(impl_->mutex_);
    auto& receiver = impl_->origins_[index];
    if (receiver && (receiver->running_ || receiver->timer_pending_)) {
        std::terminate();
    }
    receiver.reset();
}

void windows_accept_domain::stage_listener(std::size_t owner, std::size_t logical, asio::ip::tcp::acceptor& listener) {
    if (owner >= impl_->worker_count_ || logical >= impl_->listener_count_) {
        throw std::out_of_range("invalid Windows accept listener index");
    }
    auto& receiver = *impl_->origins_[owner];
    assert(receiver.runtime_.handle().is_current());
    GUID identifier = WSAID_ACCEPTEX;
    LPFN_ACCEPTEX accept = nullptr;
    DWORD bytes = 0;
    if (::WSAIoctl(listener.native_handle(), SIO_GET_EXTENSION_FUNCTION_POINTER,
            &identifier, static_cast<DWORD>(sizeof(identifier)), &accept, static_cast<DWORD>(sizeof(accept)),
            &bytes, nullptr, nullptr) == SOCKET_ERROR) {
        throw std::system_error(::WSAGetLastError(), asio::system_category(), "resolve listener AcceptEx");
    }
    const std::lock_guard lock(impl_->mutex_);
    if (impl_->published_) {
        throw std::logic_error("Windows accept listeners are immutable after publication");
    }
    auto& target = impl_->descriptors_[owner * impl_->listener_count_ + logical];
    target.handle_ = listener.native_handle();
    target.accept_ = accept;
    target.executor_ = receiver.runtime_.io_context().get_executor();
    target.protocol_ = listener.local_endpoint().protocol();
    target.logical_ = logical;
}

void windows_accept_domain::publish() {
    const std::lock_guard lock(impl_->mutex_);
    if (impl_->stopping_) {
        return;
    }
    if (impl_->published_) {
        throw std::logic_error("Windows accept matrix already published");
    }
    for (const auto& listener : impl_->descriptors_) {
        if (listener.handle_ == INVALID_SOCKET || listener.accept_ == nullptr || !listener.executor_) {
            throw std::logic_error("Windows accept listener is not ready");
        }
    }
    impl_->published_ = true;
}

task<void> windows_accept_domain::run(std::size_t index) {
    return impl_->run(*impl_->origins_[index]);
}

void windows_accept_domain::serve(std::size_t index) noexcept {
    const std::lock_guard lock(impl_->mutex_);
    auto& receiver = *impl_->origins_[index];
    if (!impl_->stopping_ && impl_->published_) {
        receiver.serving_ = true;
        impl_->notify_locked(receiver);
    }
}

void windows_accept_domain::request_stop() noexcept {
    impl_->stop();
}

}  // namespace ruvia::detail

#endif
