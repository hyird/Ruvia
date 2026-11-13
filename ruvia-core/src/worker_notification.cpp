#include "ruvia/core/worker_notification.h"

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <asio/io_context.hpp>
#ifdef _WIN32
#include <bcrypt.h>
#include <sddl.h>

#include <asio/windows/stream_handle.hpp>
#else
#include <fcntl.h>
#include <unistd.h>

#include <asio/posix/stream_descriptor.hpp>
#endif
#include <algorithm>
#include <cerrno>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/buffer.hpp>
#include <asio/error.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_runtime.h"
#ifdef __linux__
#include <sys/eventfd.h>
#endif

#include <iterator>

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

    explicit worker_notification_wait_allocator(
        worker_notification_wait_resource& resource) noexcept
        : resource_(&resource) {}

    template <typename other_type>
    worker_notification_wait_allocator(
        const worker_notification_wait_allocator<other_type>& other) noexcept
        : resource_(other.resource_) {}

    [[nodiscard]] t_type* allocate(std::size_t count) const {
        return static_cast<t_type*>(resource_->allocate(count * sizeof(t_type), alignof(t_type)));
    }

    void deallocate(t_type* pointer, std::size_t count) const noexcept {
        resource_->deallocate(pointer, count * sizeof(t_type), alignof(t_type));
    }

    template <typename other_type>
    [[nodiscard]] bool operator==(
        const worker_notification_wait_allocator<other_type>& other) const noexcept {
        return resource_ == other.resource_;
    }

private:
    template <typename>
    friend class worker_notification_wait_allocator;

    worker_notification_wait_resource* resource_;
};

#ifdef _WIN32
using native_wake_descriptor = HANDLE;
using native_wake_stream = asio::windows::stream_handle;
inline constexpr native_wake_descriptor invalid_native_wake_descriptor = nullptr;
#else
using native_wake_descriptor = int;
using native_wake_stream = asio::posix::stream_descriptor;
inline constexpr native_wake_descriptor invalid_native_wake_descriptor = -1;
#endif

enum class native_wake_signal : std::uint8_t {
    signaled,
    full,
    failed,
};

// The platform wake channel: a nonblocking sender and the receiver descriptor that
// the worker adopts into its Asio stream. Implemented by
// worker_notification_win32.cpp (named pipe) and worker_notification_posix.cpp
// (Linux eventfd or a POSIX self-pipe).
struct native_wake_pair final {
    native_wake_descriptor sender_{invalid_native_wake_descriptor};
    native_wake_descriptor receiver_{invalid_native_wake_descriptor};
#if !defined(_WIN32) && !defined(__linux__)
    // Keep a reader alive through close/notify races, preventing SIGPIPE until
    // all producers have retired and the pair itself can be destroyed.
    native_wake_descriptor receiver_guard_{invalid_native_wake_descriptor};
#endif

    native_wake_pair() = default;
    native_wake_pair(const native_wake_pair&) = delete;
    native_wake_pair& operator=(const native_wake_pair&) = delete;
    native_wake_pair(native_wake_pair&& other) noexcept;
    native_wake_pair& operator=(native_wake_pair&& other) noexcept;
    ~native_wake_pair();

    // Safe from any thread; writes one token without blocking.
    [[nodiscard]] native_wake_signal signal() const noexcept;
    void close_receiver() noexcept;
};

// startup_timeout bounds the Windows pipe connection; POSIX setup ignores it.
[[nodiscard]] native_wake_pair make_native_wake_pair(std::chrono::milliseconds startup_timeout);

[[noreturn]] void throw_native_wake_error(const char* operation, int error);

#ifdef _WIN32
void disconnect_native_receiver(native_wake_descriptor receiver) noexcept;
#else
// Consumes native wake tokens; returns whether a notification was pending.
[[nodiscard]] bool drain_native_receiver(native_wake_descriptor receiver, std::error_code& error) noexcept;
#endif

class worker_notification_state final {
public:
    worker_notification_state(asio::io_context& io_context, const worker_ref& worker,
        worker_notification_options options);
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

#ifndef _WIN32

namespace ruvia::detail {

namespace {

void close_descriptor(int& descriptor) noexcept {
    const int owned = std::exchange(descriptor, -1);
    if (owned >= 0) {
        static_cast<void>(::close(owned));
    }
}

#ifndef __linux__
int configure_descriptor(int descriptor, int command, int value = 0) {
    for (std::size_t interrupted = 0; interrupted < 64; ++interrupted) {
        const int result = fcntl(descriptor, command, value);
        if (result >= 0) {
            return result;
        }
        if (errno != EINTR) {
            throw_native_wake_error("fcntl(worker notification pipe)", errno);
        }
    }
    throw_native_wake_error("fcntl(worker notification pipe)", EINTR);
}

void configure_pipe_descriptor(int descriptor) {
    const int descriptor_flags = configure_descriptor(descriptor, F_GETFD);
    static_cast<void>(configure_descriptor(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC));
    const int status_flags = configure_descriptor(descriptor, F_GETFL);
    static_cast<void>(configure_descriptor(descriptor, F_SETFL, status_flags | O_NONBLOCK));
}
#endif

}  // namespace

native_wake_pair::native_wake_pair(native_wake_pair&& other) noexcept
    : sender_(std::exchange(other.sender_, -1)),
      receiver_(std::exchange(other.receiver_, -1))
#ifndef __linux__
      ,
      receiver_guard_(std::exchange(other.receiver_guard_, -1))
#endif
{
}

native_wake_pair& native_wake_pair::operator=(native_wake_pair&& other) noexcept {
    std::swap(sender_, other.sender_);
    std::swap(receiver_, other.receiver_);
#ifndef __linux__
    std::swap(receiver_guard_, other.receiver_guard_);
#endif
    return *this;
}

native_wake_pair::~native_wake_pair() {
    close_receiver();
    close_descriptor(sender_);
#ifndef __linux__
    close_descriptor(receiver_guard_);
#endif
}

native_wake_signal native_wake_pair::signal() const noexcept {
#ifdef __linux__
    constexpr std::uint64_t token = 1;
#else
    constexpr char token = '\x01';
#endif
    for (std::size_t interrupted = 0; interrupted < 64;) {
        const auto written = ::write(sender_, &token, sizeof(token));
        if (written == static_cast<ssize_t>(sizeof(token))) {
            return native_wake_signal::signaled;
        }
        if (written < 0 && errno == EINTR) {
            ++interrupted;
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // A full eventfd or self-pipe is already readable. The pending
            // latch was published before this write and is observed after
            // consuming native tokens, including when a read races this write.
            return native_wake_signal::full;
        }
        return native_wake_signal::failed;
    }
    return native_wake_signal::failed;
}

void native_wake_pair::close_receiver() noexcept {
    close_descriptor(receiver_);
}

bool drain_native_receiver(native_wake_descriptor receiver, std::error_code& error) noexcept {
#ifdef __linux__
    std::uint64_t counter = 0;
#else
    std::array<char, 4096> tokens;
#endif
    for (std::size_t interrupted = 0; interrupted < 64;) {
#ifdef __linux__
        const auto bytes_read = ::read(receiver, &counter, sizeof(counter));
        if (bytes_read == static_cast<ssize_t>(sizeof(counter))) {
#else
        const auto bytes_read = ::read(receiver, tokens.data(), tokens.size());
        if (bytes_read > 0) {
#endif
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
}

native_wake_pair make_native_wake_pair(std::chrono::milliseconds) {
    native_wake_pair pair;
#ifdef __linux__
    pair.sender_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (pair.sender_ < 0) {
        throw_native_wake_error("eventfd", errno);
    }
    pair.receiver_ = fcntl(pair.sender_, F_DUPFD_CLOEXEC, 0);
    if (pair.receiver_ < 0) {
        throw_native_wake_error("fcntl(F_DUPFD_CLOEXEC)", errno);
    }
#else
    int descriptors[2]{-1, -1};
    for (std::size_t interrupted = 0;; ++interrupted) {
        if (::pipe(descriptors) == 0) {
            break;
        }
        if (errno != EINTR || interrupted == 63) {
            throw_native_wake_error("pipe(worker notification)", errno);
        }
    }
    pair.receiver_ = descriptors[0];
    pair.sender_ = descriptors[1];
    configure_pipe_descriptor(pair.receiver_);
    configure_pipe_descriptor(pair.sender_);
    pair.receiver_guard_ = configure_descriptor(pair.receiver_, F_DUPFD);
    configure_pipe_descriptor(pair.receiver_guard_);
#endif
    return pair;
}

}  // namespace ruvia::detail

#endif

#ifdef _WIN32

namespace ruvia::detail {

namespace {

using clock_type = std::chrono::steady_clock;

[[nodiscard]] bool valid_descriptor(HANDLE descriptor) noexcept {
    return descriptor != nullptr && descriptor != INVALID_HANDLE_VALUE;
}

void close_descriptor(HANDLE& descriptor) noexcept {
    const HANDLE owned = std::exchange(descriptor, nullptr);
    if (valid_descriptor(owned)) {
        static_cast<void>(CloseHandle(owned));
    }
}

class pipe_connect_operation final {
public:
    explicit pipe_connect_operation(HANDLE pipe)
        : pipe_(pipe) {
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event_ == nullptr) {
            throw_native_wake_error("CreateEventW(named-pipe connect)", static_cast<int>(GetLastError()));
        }
        overlapped_.hEvent = event_;
    }

    ~pipe_connect_operation() {
        cancel_and_drain();
        close_descriptor(event_);
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
        throw_native_wake_error("ConnectNamedPipe", static_cast<int>(error));
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
            throw_native_wake_error("WaitForSingleObject(named-pipe connect)", static_cast<int>(error));
        }

        DWORD bytes_transferred = 0;
        const BOOL completed = GetOverlappedResult(pipe_, &overlapped_, &bytes_transferred, FALSE);
        const DWORD error = completed ? ERROR_SUCCESS : GetLastError();
        pending_ = false;
        if (!completed) {
            throw_native_wake_error("GetOverlappedResult(named-pipe connect)", static_cast<int>(error));
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
        throw_native_wake_error("GetNamedPipeClientProcessId", static_cast<int>(GetLastError()));
    }
    if (client_process_id != GetCurrentProcessId()) {
        throw std::runtime_error("native worker notification pipe connected to another process");
    }
}

}  // namespace

native_wake_pair::native_wake_pair(native_wake_pair&& other) noexcept
    : sender_(std::exchange(other.sender_, nullptr)),
      receiver_(std::exchange(other.receiver_, nullptr)) {}

native_wake_pair& native_wake_pair::operator=(native_wake_pair&& other) noexcept {
    std::swap(sender_, other.sender_);
    std::swap(receiver_, other.receiver_);
    return *this;
}

native_wake_pair::~native_wake_pair() {
    close_descriptor(sender_);
    close_receiver();
}

native_wake_signal native_wake_pair::signal() const noexcept {
    constexpr char token = '\x01';
    DWORD bytes_written = 0;
    if (!WriteFile(sender_, &token, 1, &bytes_written, nullptr)) {
        // ERROR_NO_DATA and every other failed WriteFile result are not evidence
        // of a full pipe. Never turn a disconnect into a successful coalesce.
        return native_wake_signal::failed;
    }
    if (bytes_written == 1) {
        return native_wake_signal::signaled;
    }
    if (bytes_written == 0) {
        // With a synchronous PIPE_NOWAIT writer, TRUE plus zero bytes means
        // the byte-mode pipe buffer is full; its receiver already has data.
        return native_wake_signal::full;
    }
    return native_wake_signal::failed;
}

void native_wake_pair::close_receiver() noexcept {
    if (valid_descriptor(receiver_)) {
        disconnect_native_receiver(receiver_);
        close_descriptor(receiver_);
    }
}

void disconnect_native_receiver(native_wake_descriptor receiver) noexcept {
    if (!valid_descriptor(receiver) || DisconnectNamedPipe(receiver)) {
        return;
    }
    if (GetLastError() != ERROR_PIPE_NOT_CONNECTED) {
        std::terminate();
    }
}

native_wake_pair make_native_wake_pair(std::chrono::milliseconds startup_timeout) {
    native_wake_pair pair;
    const auto pipe_name = make_pipe_name();

    // Restrict access to the pipe owner's SID; the random name and later client-PID
    // check prevent another local process from impersonating this worker channel.
    local_security_descriptor security_descriptor;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &security_descriptor.value_, nullptr)) {
        throw_native_wake_error("ConvertStringSecurityDescriptorToSecurityDescriptorW",
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
    if (!valid_descriptor(pair.receiver_)) {
        const DWORD error = GetLastError();
        pair.receiver_ = nullptr;
        throw_native_wake_error("CreateNamedPipeW", static_cast<int>(error));
    }

    const auto deadline_value = clock_type::now() + startup_timeout;
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
            throw_native_wake_error("WaitNamedPipeW", static_cast<int>(error));
        }

        pair.sender_ = CreateFileW(pipe_name.data(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (valid_descriptor(pair.sender_)) {
            break;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY && clock_type::now() < deadline_value) {
            continue;
        }
        if (error == ERROR_PIPE_BUSY || error == ERROR_SEM_TIMEOUT) {
            throw std::runtime_error("timed out opening native worker notification writer");
        }
        throw_native_wake_error("CreateFileW(named-pipe writer)", static_cast<int>(error));
    }

    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(pair.sender_, &mode, nullptr, nullptr)) {
        throw_native_wake_error("SetNamedPipeHandleState(PIPE_NOWAIT)", static_cast<int>(GetLastError()));
    }
    connect_operation.wait_until(deadline_value);
    validate_pipe_client_process(pair.receiver_);
    return pair;
}

}  // namespace ruvia::detail

#endif

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

void worker_notification_wait_resource::deallocate(
    void* pointer, std::size_t, std::size_t) noexcept {
    if (!allocated() || pointer != allocation_) {
        std::terminate();
    }
    allocation_ = nullptr;
}

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

void throw_native_wake_error(const char* operation, int error) {
    throw std::system_error(error, std::system_category(), operation);
}

worker_notification_state::worker_notification_state(asio::io_context& io_context,
    const worker_ref& worker, worker_notification_options options)
    : io_context_(io_context),
      worker_(worker) {
    if (!worker_.valid() || !worker_.accepting()) {
        throw std::invalid_argument("worker notification requires an accepting worker runtime");
    }
    if (options.startup_timeout_.value() <= std::chrono::milliseconds::zero() ||
        options.startup_timeout_.value() > std::chrono::minutes(1)) {
        throw std::invalid_argument("worker notification startup timeout must be in (0, 1 minute]");
    }
    wake_ = make_native_wake_pair(options.startup_timeout_.value());
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
        auto handler = asio::bind_allocator(
            worker_notification_wait_allocator<std::byte>(wait_resource_),
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

// The only place a notification's state is built; every public constructor
// hands its runtime's io_context and worker_ref to this entry.
[[nodiscard]] std::unique_ptr<worker_notification_state>
make_worker_notification_state(
    asio::io_context& io_context, const worker_ref& worker, worker_notification_options options) {
    return std::make_unique<worker_notification_state>(io_context, worker, options);
}

}  // namespace

}  // namespace ruvia::detail

namespace ruvia {

worker_notification::wait_operation::wait_operation(worker_notification& owner_value) noexcept
    : owner_(&owner_value) {}

bool worker_notification::wait_operation::await_ready() {
    return owner_->state_->wait_ready();
}

bool worker_notification::wait_operation::await_suspend(std::coroutine_handle<> continuation) {
    return owner_->state_->begin_wait(continuation);
}

worker_notification_wait_status worker_notification::wait_operation::await_resume() {
    return owner_->state_->take_wait_result();
}

worker_notification::worker_notification(state_pointer state) noexcept
    : state_(std::move(state)),
      wait_awaiter_(*this) {}

worker_notification::worker_notification(
    worker_runtime& runtime, worker_notification_options options)
    : worker_notification(detail::make_worker_notification_state(runtime.io_context(), runtime.worker(), options)) {}

worker_notification::worker_notification(
    const event_loop& loop, worker_notification_options options)
    : worker_notification(detail::make_worker_notification_state(loop.io_context(), loop.worker(), options)) {}
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
