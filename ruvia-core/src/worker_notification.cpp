#if !defined(_WIN32) && !defined(__linux__)
#error "worker_notification native backends are implemented for Linux and Windows only"
#endif

#include "ruvia/core/worker_notification.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef _WIN32
#include <bcrypt.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

#include <asio/bind_allocator.hpp>
#include <asio/buffer.hpp>
#include <asio/error.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/core/worker_runtime_context.h"

#include "worker_notification.h"

namespace ruvia::detail {

void* worker_notification_wait_resource::do_allocate(std::size_t bytes_value, std::size_t alignment) {
    if (allocated_ || bytes_value > buffer_size || alignment > max_alignment) {
        throw std::bad_alloc();
    }

    void* candidate_value = buffer_storage_.data();
    auto space = buffer_storage_.size();
    auto* pointer = std::align(alignment, std::max<std::size_t>(bytes_value, 1), candidate_value, space);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }

    allocated_ = true;
    allocated_pointer_ = pointer;
    allocated_bytes_ = bytes_value;
    allocated_alignment_ = alignment;
    ++allocation_count_;
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
    ++deallocation_count_;
}

bool worker_notification_wait_resource::do_is_equal(
    const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

namespace {

using clock_type = std::chrono::steady_clock;

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

[[noreturn]] void throw_native_error(const char* operation, int error) {
    throw std::system_error(error, std::system_category(), operation);
}

#ifdef _WIN32

[[nodiscard]] bool valid_handle(HANDLE handle) noexcept {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

void close_handle(HANDLE& handle) noexcept {
    const HANDLE owned = std::exchange(handle, nullptr);
    if (valid_handle(owned)) {
        static_cast<void>(CloseHandle(owned));
    }
}

void disconnect_pipe_no_throw(HANDLE pipe) noexcept {
    if (!valid_handle(pipe) || DisconnectNamedPipe(pipe)) {
        return;
    }
    if (GetLastError() != ERROR_PIPE_NOT_CONNECTED) {
        std::terminate();
    }
}

struct native_wake_pair final {
    HANDLE sender_{nullptr};
    HANDLE receiver_{nullptr};

    native_wake_pair() = default;
    native_wake_pair(const native_wake_pair&) = delete;
    native_wake_pair& operator=(const native_wake_pair&) = delete;

    native_wake_pair(native_wake_pair&& other) noexcept
        : sender_(std::exchange(other.sender_, nullptr)),
          receiver_(std::exchange(other.receiver_, nullptr)) {}

    ~native_wake_pair() {
        close_handle(sender_);
        disconnect_pipe_no_throw(receiver_);
        close_handle(receiver_);
    }
};

class pipe_connect_operation final {
public:
    explicit pipe_connect_operation(HANDLE pipe)
        : pipe_(pipe) {
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event_ == nullptr) {
            throw_native_error("CreateEventW(named-pipe connect)", static_cast<int>(GetLastError()));
        }
        overlapped_.hEvent = event_;
    }

    ~pipe_connect_operation() {
        cancel_and_drain();
        close_handle(event_);
    }

    pipe_connect_operation(const pipe_connect_operation&) = delete;
    pipe_connect_operation& operator=(const pipe_connect_operation&) = delete;

    [[nodiscard]] bool start() {
        if (ConnectNamedPipe(pipe_, &overlapped_)) {
            return true;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            return true;
        }
        if (error == ERROR_IO_PENDING) {
            pending_ = true;
            return false;
        }
        throw_native_error("ConnectNamedPipe", static_cast<int>(error));
    }

    [[nodiscard]] static DWORD remaining_milliseconds(clock_type::time_point deadline_value) noexcept {
        const auto now = clock_type::now();
        if (now >= deadline_value) {
            return 0;
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline_value - now).count();
        return static_cast<DWORD>(std::clamp<std::int64_t>(remaining, 1, 60'000));
    }

    void wait_until(clock_type::time_point deadline_value) {
        if (!pending_) {
            return;
        }
        const DWORD timeout = remaining_milliseconds(deadline_value);
        const DWORD wait_result = WaitForSingleObject(event_, timeout);
        if (wait_result == WAIT_TIMEOUT) {
            throw std::runtime_error("timed out connecting native worker notification pipe");
        }
        if (wait_result != WAIT_OBJECT_0) {
            const DWORD error = wait_result == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA;
            throw_native_error("WaitForSingleObject(named-pipe connect)", static_cast<int>(error));
        }

        DWORD bytes_transferred = 0;
        const BOOL completed = GetOverlappedResult(pipe_, &overlapped_, &bytes_transferred, FALSE);
        const DWORD error = completed ? ERROR_SUCCESS : GetLastError();
        pending_ = false;
        if (!completed) {
            throw_native_error("GetOverlappedResult(named-pipe connect)", static_cast<int>(error));
        }
    }

private:
    void cancel_and_drain() noexcept {
        if (!pending_) {
            return;
        }
        if (!CancelIoEx(pipe_, &overlapped_) && GetLastError() != ERROR_NOT_FOUND) {
            std::terminate();
        }
        if (WaitForSingleObject(event_, INFINITE) != WAIT_OBJECT_0) {
            std::terminate();
        }
        DWORD bytes_transferred = 0;
        if (!GetOverlappedResult(pipe_, &overlapped_, &bytes_transferred, FALSE) &&
            GetLastError() != ERROR_OPERATION_ABORTED) {
            std::terminate();
        }
        pending_ = false;
    }

    HANDLE pipe_{};
    HANDLE event_{};
    OVERLAPPED overlapped_{};
    bool pending_{false};
};

struct local_security_descriptor final {
    PSECURITY_DESCRIPTOR value_{nullptr};

    ~local_security_descriptor() {
        if (value_ != nullptr) {
            static_cast<void>(LocalFree(value_));
        }
    }
};

[[nodiscard]] std::array<wchar_t, 80> make_pipe_name() {
    constexpr wchar_t prefix[] = L"\\\\.\\pipe\\Ruvia-WorkerNotification-";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::array<std::uint8_t, 16> random_bytes{};
    if (BCryptGenRandom(nullptr, random_bytes.data(), static_cast<ULONG>(random_bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw std::runtime_error("BCryptGenRandom failed for native worker notification name");
    }

    std::array<wchar_t, 80> name{};
    std::size_t position = 0;
    for (std::size_t index = 0; index + 1 < std::size(prefix); ++index) {
        name[position++] = prefix[index];
    }
    for (const auto byte : random_bytes) {
        name[position++] = hex[byte >> 4];
        name[position++] = hex[byte & 0x0f];
    }
    return name;
}

void validate_pipe_client_process(HANDLE server_pipe) {
    ULONG client_process_id = 0;
    if (!GetNamedPipeClientProcessId(server_pipe, &client_process_id)) {
        throw_native_error("GetNamedPipeClientProcessId", static_cast<int>(GetLastError()));
    }
    if (client_process_id != GetCurrentProcessId()) {
        throw std::runtime_error("native worker notification pipe connected to another process");
    }
}

[[nodiscard]] native_wake_pair make_native_wake_pair(std::chrono::milliseconds timeout) {
    native_wake_pair pair;
    const auto pipe_name = make_pipe_name();

    // Restrict access to the pipe owner's SID; the random name and later client-PID
    // check prevent another local process from impersonating this worker channel.
    local_security_descriptor security_descriptor;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &security_descriptor.value_, nullptr)) {
        throw_native_error("ConvertStringSecurityDescriptorToSecurityDescriptorW",
            static_cast<int>(GetLastError()));
    }
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.lpSecurityDescriptor = security_descriptor.value_;
    security_attributes.bInheritHandle = FALSE;

    pair.receiver_ = CreateNamedPipeW(pipe_name.data(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, &security_attributes);
    if (!valid_handle(pair.receiver_)) {
        const DWORD error = GetLastError();
        pair.receiver_ = nullptr;
        throw_native_error("CreateNamedPipeW", static_cast<int>(error));
    }

    const auto deadline_value = clock_type::now() + timeout;
    pipe_connect_operation connect_operation(pair.receiver_);
    if (connect_operation.start()) {
        // ERROR_PIPE_CONNECTED here precedes our CreateFile call: validate the peer,
        // then fail closed because this instance has no writer handle owned by us.
        validate_pipe_client_process(pair.receiver_);
        throw std::runtime_error(
            "native worker notification pipe connected before its writer handle was opened");
    }

    for (;;) {
        const DWORD remaining = pipe_connect_operation::remaining_milliseconds(deadline_value);
        if (!WaitNamedPipeW(pipe_name.data(), remaining)) {
            const DWORD error = GetLastError();
            if (error == ERROR_SEM_TIMEOUT || error == ERROR_FILE_NOT_FOUND) {
                throw std::runtime_error("timed out waiting for native worker notification pipe");
            }
            throw_native_error("WaitNamedPipeW", static_cast<int>(error));
        }

        pair.sender_ = CreateFileW(pipe_name.data(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (valid_handle(pair.sender_)) {
            break;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY && clock_type::now() < deadline_value) {
            continue;
        }
        if (error == ERROR_PIPE_BUSY || error == ERROR_SEM_TIMEOUT) {
            throw std::runtime_error("timed out opening native worker notification writer");
        }
        throw_native_error("CreateFileW(named-pipe writer)", static_cast<int>(error));
    }

    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(pair.sender_, &mode, nullptr, nullptr)) {
        throw_native_error("SetNamedPipeHandleState(PIPE_NOWAIT)", static_cast<int>(GetLastError()));
    }
    connect_operation.wait_until(deadline_value);
    validate_pipe_client_process(pair.receiver_);
    return pair;
}

#else

void close_descriptor(int& descriptor) noexcept {
    const int owned = std::exchange(descriptor, -1);
    if (owned >= 0) {
        static_cast<void>(::close(owned));
    }
}

struct native_wake_pair final {
    int sender_{-1};
    int receiver_{-1};

    native_wake_pair() = default;
    native_wake_pair(const native_wake_pair&) = delete;
    native_wake_pair& operator=(const native_wake_pair&) = delete;

    native_wake_pair(native_wake_pair&& other) noexcept
        : sender_(std::exchange(other.sender_, -1)),
          receiver_(std::exchange(other.receiver_, -1)) {}

    ~native_wake_pair() {
        close_descriptor(receiver_);
        close_descriptor(sender_);
    }
};

[[nodiscard]] native_wake_pair make_native_wake_pair() {
    native_wake_pair pair;
    pair.sender_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (pair.sender_ < 0) {
        throw_native_error("eventfd", errno);
    }
    pair.receiver_ = fcntl(pair.sender_, F_DUPFD_CLOEXEC, 0);
    if (pair.receiver_ < 0) {
        throw_native_error("fcntl(F_DUPFD_CLOEXEC)", errno);
    }
    return pair;
}

#endif

}  // namespace

worker_notification_state::worker_notification_state(event_loop loop, worker_notification_options options)
    : loop_(std::move(loop)) {
    if (!loop_.valid() || !loop_.accepting()) {
        throw std::invalid_argument("worker notification requires an accepting event loop");
    }
    initialize(options);
}

worker_notification_state::worker_notification_state(
    ::ruvia::worker_runtime_context& runtime, worker_notification_options options)
    : runtime_io_context_(&runtime.io_context()),
      runtime_handle_(&runtime.handle()) {
    if (!runtime_handle_->valid() || !runtime_handle_->accepting()) {
        throw std::invalid_argument("worker notification requires an accepting worker runtime");
    }
    initialize(options);
}

void worker_notification_state::initialize(worker_notification_options options) {
    if (options.startup_timeout_ <= std::chrono::milliseconds::zero() ||
        options.startup_timeout_ > std::chrono::minutes(1)) {
        throw std::invalid_argument("worker notification startup timeout must be in (0, 1 minute]");
    }
#ifdef _WIN32
    auto pair = make_native_wake_pair(options.startup_timeout_);
#else
    auto pair = make_native_wake_pair();
#endif
    sender_ = std::exchange(pair.sender_,
#ifdef _WIN32
        nullptr
#else
        -1
#endif
    );
    receiver_ = std::exchange(pair.receiver_,
#ifdef _WIN32
        nullptr
#else
        -1
#endif
    );
}

worker_notification_state::~worker_notification_state() {
    const auto activity = activity_.load(std::memory_order_acquire);
    if ((activity & count_mask) != 0 || wait_pending_ || wait_continuation_ ||
        receiver_stream_.has_value() || wait_resource_.outstanding_allocations() != 0) {
        std::terminate();
    }
    retire_receiver();
#ifdef _WIN32
    close_handle(sender_);
#else
    close_descriptor(sender_);
#endif
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

#ifdef _WIN32
    constexpr char token = '\x01';
    DWORD bytes_written = 0;
    const BOOL written = WriteFile(sender_, &token, 1, &bytes_written, nullptr);
    if (written) {
        if (is_closed()) {
            return worker_notification_status::closed;
        }
        if (bytes_written == 1) {
            return worker_notification_status::notified;
        }
        if (bytes_written == 0) {
            // With a synchronous PIPE_NOWAIT writer, TRUE plus zero bytes means
            // the byte-mode pipe buffer is full; its receiver already has data.
            return worker_notification_status::coalesced;
        }
        std::terminate();
    }
    const DWORD error = GetLastError();
    if (is_closed()) {
        return worker_notification_status::closed;
    }
    // ERROR_NO_DATA and every other failed WriteFile result are not evidence
    // of a full pipe. Never turn a disconnect into a successful coalesce.
    static_cast<void>(error);
    std::terminate();
#else
    constexpr std::uint64_t token = 1;
    for (std::size_t interrupted = 0; interrupted < 64;) {
        const auto written = ::write(sender_, &token, sizeof(token));
        if (written == static_cast<ssize_t>(sizeof(token))) {
            return is_closed() ? worker_notification_status::closed
                               : worker_notification_status::notified;
        }
        if (written < 0 && errno == EINTR) {
            ++interrupted;
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // eventfd returns EAGAIN only when its counter cannot be incremented
            // past UINT64_MAX-1. That nonzero counter is readable. A concurrent
            // read may consume it first, but pending_ was published before this
            // write and wait_ready() observes that latch after draining.
            return is_closed() ? worker_notification_status::closed
                               : worker_notification_status::coalesced;
        }
        if (is_closed()) {
            return worker_notification_status::closed;
        }
        std::terminate();
    }
    if (is_closed()) {
        return worker_notification_status::closed;
    }
    std::terminate();
#endif
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
        receiver_stream_->async_wait(worker_notification_receiver_type::wait_read, std::move(handler));
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
    const bool is_current = runtime_handle_ != nullptr ? runtime_handle_->is_current() : loop_.is_current();
    if (!is_current) {
        throw std::logic_error("worker notification wait and close are worker-affine");
    }
}

asio::io_context& worker_notification_state::io_context() const {
    return runtime_io_context_ != nullptr ? *runtime_io_context_ : loop_.io_context();
}

void worker_notification_state::ensure_receiver() {
    if (receiver_stream_) {
        return;
    }
    try {
        receiver_stream_.emplace(io_context());
        asio::error_code error;
        receiver_stream_->assign(receiver_, error);
        if (error) {
            throw std::system_error(error, "assign worker notification receiver");
        }
#ifdef _WIN32
        receiver_ = nullptr;
#else
        receiver_ = -1;
#endif
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

bool worker_notification_state::drain_receiver(std::error_code& error) noexcept {
#ifdef _WIN32
    static_cast<void>(error);
    return false;
#else
    if (!receiver_stream_ || !receiver_stream_->is_open()) {
        return false;
    }
    std::uint64_t counter = 0;
    for (std::size_t interrupted = 0; interrupted < 64;) {
        const auto bytes_read = ::read(receiver_stream_->native_handle(), &counter, sizeof(counter));
        if (bytes_read == static_cast<ssize_t>(sizeof(counter))) {
            return true;
        }
        if (bytes_read < 0 && errno == EINTR) {
            ++interrupted;
            continue;
        }
        if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false;
        }
        if (bytes_read < 0) {
            error = std::error_code(errno, std::system_category());
        } else if (bytes_read == 0) {
            error = std::make_error_code(std::errc::connection_reset);
        } else {
            error = std::make_error_code(std::errc::io_error);
        }
        return false;
    }
    error = std::make_error_code(std::errc::interrupted);
    return false;
#endif
}

void worker_notification_state::retire_receiver() noexcept {
#ifdef _WIN32
    if (receiver_stream_) {
        const HANDLE handle = receiver_stream_->is_open() ? receiver_stream_->native_handle() : nullptr;
        disconnect_pipe_no_throw(handle);
        asio::error_code error;
        if (receiver_stream_->is_open()) {
            receiver_stream_->close(error);
            if (error) {
                std::terminate();
            }
        }
        receiver_stream_.reset();
    }
    if (valid_handle(receiver_)) {
        disconnect_pipe_no_throw(receiver_);
        close_handle(receiver_);
    }
#else
    if (receiver_stream_) {
        asio::error_code error;
        if (receiver_stream_->is_open()) {
            receiver_stream_->close(error);
            if (error) {
                std::terminate();
            }
        }
        receiver_stream_.reset();
    }
    close_descriptor(receiver_);
#endif
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
#ifdef _WIN32
        static_cast<void>(bytes_transferred);
        static_cast<void>(pending_.exchange(false, std::memory_order_acq_rel));
#else
        static_cast<void>(bytes_transferred);
        static_cast<void>(drain_receiver(wait_error_));
        static_cast<void>(pending_.exchange(false, std::memory_order_acq_rel));
#endif
    }
    wait_result_ = worker_notification_wait_status::notified;
    continuation.resume();
}

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

worker_notification::worker_notification(event_loop loop, worker_notification_options options)
    : state_(detail::make_pmr_object<detail::worker_notification_state>(
          detail::process_resource(), std::move(loop), options)),
      wait_awaiter_(*this) {}

worker_notification::worker_notification(
    worker_runtime_context& runtime, worker_notification_options options)
    : state_(detail::make_pmr_object<detail::worker_notification_state>(
          detail::process_resource(), runtime, options)),
      wait_awaiter_(*this) {}

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
