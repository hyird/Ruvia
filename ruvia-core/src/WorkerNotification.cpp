#if !defined(_WIN32) && !defined(__linux__)
#error "WorkerNotification native backends are implemented for Linux and Windows only"
#endif

#include "ruvia/core/WorkerNotification.h"

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

#include "ruvia/core/EventLoop.h"
#include "ruvia/core/WorkerRuntimeContext.h"
#include "ruvia/core/memory/ProcessResource.h"

#include "WorkerNotification.h"

namespace ruvia::detail {

void* WorkerNotificationWaitResource::do_allocate(std::size_t bytes, std::size_t alignment) {
    if (allocated_ || bytes > kBufferSize || alignment > kMaxAlignment) {
        throw std::bad_alloc();
    }

    void* candidate = bufferStorage_.data();
    auto space = bufferStorage_.size();
    auto* pointer = std::align(alignment, std::max<std::size_t>(bytes, 1), candidate, space);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }

    allocated_ = true;
    allocatedPointer_ = pointer;
    allocatedBytes_ = bytes;
    allocatedAlignment_ = alignment;
    ++allocationCount_;
    return pointer;
}

void WorkerNotificationWaitResource::do_deallocate(
    void* pointer, std::size_t bytes, std::size_t alignment) noexcept {
    if (!allocated_ || pointer != allocatedPointer_ || bytes != allocatedBytes_ ||
        alignment != allocatedAlignment_) {
        std::terminate();
    }
    allocated_ = false;
    allocatedPointer_ = nullptr;
    allocatedBytes_ = 0;
    allocatedAlignment_ = 0;
    ++deallocationCount_;
}

bool WorkerNotificationWaitResource::do_is_equal(
    const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

namespace {

using Clock = std::chrono::steady_clock;

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

[[noreturn]] void throwNativeError(const char* operation, int error) {
    throw std::system_error(error, std::system_category(), operation);
}

#ifdef _WIN32

[[nodiscard]] bool validHandle(HANDLE handle) noexcept {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

void closeHandle(HANDLE& handle) noexcept {
    const HANDLE owned = std::exchange(handle, nullptr);
    if (validHandle(owned)) {
        static_cast<void>(CloseHandle(owned));
    }
}

void disconnectPipeNoThrow(HANDLE pipe) noexcept {
    if (!validHandle(pipe) || DisconnectNamedPipe(pipe)) {
        return;
    }
    if (GetLastError() != ERROR_PIPE_NOT_CONNECTED) {
        std::terminate();
    }
}

struct NativeWakePair final {
    HANDLE sender{nullptr};
    HANDLE receiver{nullptr};

    NativeWakePair() = default;
    NativeWakePair(const NativeWakePair&) = delete;
    NativeWakePair& operator=(const NativeWakePair&) = delete;

    NativeWakePair(NativeWakePair&& other) noexcept
        : sender(std::exchange(other.sender, nullptr)),
          receiver(std::exchange(other.receiver, nullptr)) {}

    ~NativeWakePair() {
        closeHandle(sender);
        disconnectPipeNoThrow(receiver);
        closeHandle(receiver);
    }
};

class PipeConnectOperation final {
public:
    explicit PipeConnectOperation(HANDLE pipe)
        : pipe_(pipe) {
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event_ == nullptr) {
            throwNativeError("CreateEventW(named-pipe connect)", static_cast<int>(GetLastError()));
        }
        overlapped_.hEvent = event_;
    }

    ~PipeConnectOperation() {
        cancelAndDrain();
        closeHandle(event_);
    }

    PipeConnectOperation(const PipeConnectOperation&) = delete;
    PipeConnectOperation& operator=(const PipeConnectOperation&) = delete;

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
        throwNativeError("ConnectNamedPipe", static_cast<int>(error));
    }

    [[nodiscard]] static DWORD remainingMilliseconds(Clock::time_point deadline) noexcept {
        const auto now = Clock::now();
        if (now >= deadline) {
            return 0;
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        return static_cast<DWORD>(std::clamp<std::int64_t>(remaining, 1, 60'000));
    }

    void waitUntil(Clock::time_point deadline) {
        if (!pending_) {
            return;
        }
        const DWORD timeout = remainingMilliseconds(deadline);
        const DWORD waitResult = WaitForSingleObject(event_, timeout);
        if (waitResult == WAIT_TIMEOUT) {
            throw std::runtime_error("timed out connecting native worker notification pipe");
        }
        if (waitResult != WAIT_OBJECT_0) {
            const DWORD error = waitResult == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA;
            throwNativeError("WaitForSingleObject(named-pipe connect)", static_cast<int>(error));
        }

        DWORD bytesTransferred = 0;
        const BOOL completed = GetOverlappedResult(pipe_, &overlapped_, &bytesTransferred, FALSE);
        const DWORD error = completed ? ERROR_SUCCESS : GetLastError();
        pending_ = false;
        if (!completed) {
            throwNativeError("GetOverlappedResult(named-pipe connect)", static_cast<int>(error));
        }
    }

private:
    void cancelAndDrain() noexcept {
        if (!pending_) {
            return;
        }
        if (!CancelIoEx(pipe_, &overlapped_) && GetLastError() != ERROR_NOT_FOUND) {
            std::terminate();
        }
        if (WaitForSingleObject(event_, INFINITE) != WAIT_OBJECT_0) {
            std::terminate();
        }
        DWORD bytesTransferred = 0;
        if (!GetOverlappedResult(pipe_, &overlapped_, &bytesTransferred, FALSE) &&
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

struct LocalSecurityDescriptor final {
    PSECURITY_DESCRIPTOR value{nullptr};

    ~LocalSecurityDescriptor() {
        if (value != nullptr) {
            static_cast<void>(LocalFree(value));
        }
    }
};

[[nodiscard]] std::array<wchar_t, 80> makePipeName() {
    constexpr wchar_t kPrefix[] = L"\\\\.\\pipe\\Ruvia-WorkerNotification-";
    constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::array<std::uint8_t, 16> randomBytes{};
    if (BCryptGenRandom(nullptr, randomBytes.data(), static_cast<ULONG>(randomBytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw std::runtime_error("BCryptGenRandom failed for native worker notification name");
    }

    std::array<wchar_t, 80> name{};
    std::size_t position = 0;
    for (std::size_t index = 0; index + 1 < std::size(kPrefix); ++index) {
        name[position++] = kPrefix[index];
    }
    for (const auto byte : randomBytes) {
        name[position++] = kHex[byte >> 4];
        name[position++] = kHex[byte & 0x0f];
    }
    return name;
}

void validatePipeClientProcess(HANDLE serverPipe) {
    ULONG clientProcessId = 0;
    if (!GetNamedPipeClientProcessId(serverPipe, &clientProcessId)) {
        throwNativeError("GetNamedPipeClientProcessId", static_cast<int>(GetLastError()));
    }
    if (clientProcessId != GetCurrentProcessId()) {
        throw std::runtime_error("native worker notification pipe connected to another process");
    }
}

[[nodiscard]] NativeWakePair makeNativeWakePair(std::chrono::milliseconds timeout) {
    NativeWakePair pair;
    const auto pipeName = makePipeName();

    // Restrict access to the pipe owner's SID; the random name and later client-PID
    // check prevent another local process from impersonating this worker channel.
    LocalSecurityDescriptor securityDescriptor;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &securityDescriptor.value, nullptr)) {
        throwNativeError("ConvertStringSecurityDescriptorToSecurityDescriptorW",
            static_cast<int>(GetLastError()));
    }
    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.lpSecurityDescriptor = securityDescriptor.value;
    securityAttributes.bInheritHandle = FALSE;

    pair.receiver = CreateNamedPipeW(pipeName.data(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, &securityAttributes);
    if (!validHandle(pair.receiver)) {
        const DWORD error = GetLastError();
        pair.receiver = nullptr;
        throwNativeError("CreateNamedPipeW", static_cast<int>(error));
    }

    const auto deadline = Clock::now() + timeout;
    PipeConnectOperation connectOperation(pair.receiver);
    if (connectOperation.start()) {
        // ERROR_PIPE_CONNECTED here precedes our CreateFile call: validate the peer,
        // then fail closed because this instance has no writer handle owned by us.
        validatePipeClientProcess(pair.receiver);
        throw std::runtime_error(
            "native worker notification pipe connected before its writer handle was opened");
    }

    for (;;) {
        const DWORD remaining = PipeConnectOperation::remainingMilliseconds(deadline);
        if (!WaitNamedPipeW(pipeName.data(), remaining)) {
            const DWORD error = GetLastError();
            if (error == ERROR_SEM_TIMEOUT || error == ERROR_FILE_NOT_FOUND) {
                throw std::runtime_error("timed out waiting for native worker notification pipe");
            }
            throwNativeError("WaitNamedPipeW", static_cast<int>(error));
        }

        pair.sender = CreateFileW(pipeName.data(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (validHandle(pair.sender)) {
            break;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY && Clock::now() < deadline) {
            continue;
        }
        if (error == ERROR_PIPE_BUSY || error == ERROR_SEM_TIMEOUT) {
            throw std::runtime_error("timed out opening native worker notification writer");
        }
        throwNativeError("CreateFileW(named-pipe writer)", static_cast<int>(error));
    }

    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(pair.sender, &mode, nullptr, nullptr)) {
        throwNativeError("SetNamedPipeHandleState(PIPE_NOWAIT)", static_cast<int>(GetLastError()));
    }
    connectOperation.waitUntil(deadline);
    validatePipeClientProcess(pair.receiver);
    return pair;
}

#else

void closeDescriptor(int& descriptor) noexcept {
    const int owned = std::exchange(descriptor, -1);
    if (owned >= 0) {
        static_cast<void>(::close(owned));
    }
}

struct NativeWakePair final {
    int sender{-1};
    int receiver{-1};

    NativeWakePair() = default;
    NativeWakePair(const NativeWakePair&) = delete;
    NativeWakePair& operator=(const NativeWakePair&) = delete;

    NativeWakePair(NativeWakePair&& other) noexcept
        : sender(std::exchange(other.sender, -1)),
          receiver(std::exchange(other.receiver, -1)) {}

    ~NativeWakePair() {
        closeDescriptor(receiver);
        closeDescriptor(sender);
    }
};

[[nodiscard]] NativeWakePair makeNativeWakePair() {
    NativeWakePair pair;
    pair.sender = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (pair.sender < 0) {
        throwNativeError("eventfd", errno);
    }
    pair.receiver = fcntl(pair.sender, F_DUPFD_CLOEXEC, 0);
    if (pair.receiver < 0) {
        throwNativeError("fcntl(F_DUPFD_CLOEXEC)", errno);
    }
    return pair;
}

#endif

}  // namespace

WorkerNotificationState::WorkerNotificationState(EventLoop loop, WorkerNotificationOptions options)
    : loop_(std::move(loop)) {
    if (!loop_.valid() || !loop_.accepting()) {
        throw std::invalid_argument("worker notification requires an accepting event loop");
    }
    initialize(options);
}

WorkerNotificationState::WorkerNotificationState(
    ::ruvia::WorkerRuntimeContext& runtime, WorkerNotificationOptions options)
    : runtimeIoContext_(&runtime.ioContext()),
      runtimeHandle_(&runtime.handle()) {
    if (!runtimeHandle_->valid() || !runtimeHandle_->accepting()) {
        throw std::invalid_argument("worker notification requires an accepting worker runtime");
    }
    initialize(options);
}

void WorkerNotificationState::initialize(WorkerNotificationOptions options) {
    if (options.startupTimeout <= std::chrono::milliseconds::zero() ||
        options.startupTimeout > std::chrono::minutes(1)) {
        throw std::invalid_argument("worker notification startup timeout must be in (0, 1 minute]");
    }
#ifdef _WIN32
    auto pair = makeNativeWakePair(options.startupTimeout);
#else
    auto pair = makeNativeWakePair();
#endif
    sender_ = std::exchange(pair.sender,
#ifdef _WIN32
        nullptr
#else
        -1
#endif
    );
    receiver_ = std::exchange(pair.receiver,
#ifdef _WIN32
        nullptr
#else
        -1
#endif
    );
}

WorkerNotificationState::~WorkerNotificationState() {
    const auto activity = activity_.load(std::memory_order_acquire);
    if ((activity & kCountMask) != 0 || waitPending_ || waitContinuation_ ||
        receiverStream_.has_value() || waitResource_.outstandingAllocations() != 0) {
        std::terminate();
    }
    retireReceiver();
#ifdef _WIN32
    closeHandle(sender_);
#else
    closeDescriptor(sender_);
#endif
}

WorkerNotificationStatus WorkerNotificationState::notify() noexcept {
    if (!enterProducer()) {
        return WorkerNotificationStatus::kClosed;
    }
    struct ProducerExit final {
        WorkerNotificationState& state;
        ~ProducerExit() {
            state.leaveProducer();
        }
    } producerExit{*this};

    if (pending_.exchange(true, std::memory_order_acq_rel)) {
        return isClosed() ? WorkerNotificationStatus::kClosed : WorkerNotificationStatus::kCoalesced;
    }
    if (isClosed()) {
        return WorkerNotificationStatus::kClosed;
    }

#ifdef _WIN32
    constexpr char token = '\x01';
    DWORD bytesWritten = 0;
    const BOOL written = WriteFile(sender_, &token, 1, &bytesWritten, nullptr);
    if (written) {
        if (isClosed()) {
            return WorkerNotificationStatus::kClosed;
        }
        if (bytesWritten == 1) {
            return WorkerNotificationStatus::kNotified;
        }
        if (bytesWritten == 0) {
            // With a synchronous PIPE_NOWAIT writer, TRUE plus zero bytes means
            // the byte-mode pipe buffer is full; its receiver already has data.
            return WorkerNotificationStatus::kCoalesced;
        }
        std::terminate();
    }
    const DWORD error = GetLastError();
    if (isClosed()) {
        return WorkerNotificationStatus::kClosed;
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
            return isClosed() ? WorkerNotificationStatus::kClosed
                              : WorkerNotificationStatus::kNotified;
        }
        if (written < 0 && errno == EINTR) {
            ++interrupted;
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // eventfd returns EAGAIN only when its counter cannot be incremented
            // past UINT64_MAX-1. That nonzero counter is readable. A concurrent
            // read may consume it first, but pending_ was published before this
            // write and waitReady() observes that latch after draining.
            return isClosed() ? WorkerNotificationStatus::kClosed
                              : WorkerNotificationStatus::kCoalesced;
        }
        if (isClosed()) {
            return WorkerNotificationStatus::kClosed;
        }
        std::terminate();
    }
    if (isClosed()) {
        return WorkerNotificationStatus::kClosed;
    }
    std::terminate();
#endif
}

bool WorkerNotificationState::waitReady() {
    requireWorker();
    if (waitPending_) {
        throw std::logic_error("worker notification permits one outstanding wait");
    }
    if (isClosed()) {
        waitResult_ = WorkerNotificationWaitStatus::kClosed;
        waitError_.clear();
        return true;
    }
    ensureReceiver();
#ifdef _WIN32
    // Overlapped named-pipe reads are the readiness operation on IOCP. Even an
    // already-latched notification is consumed by the ensuing async_read_some.
    return false;
#else
    const bool received = drainReceiver(waitError_);
    const bool latched = pending_.exchange(false, std::memory_order_acq_rel);
    if (waitError_ || received || latched) {
        waitResult_ = WorkerNotificationWaitStatus::kNotified;
        return true;
    }
    return false;
#endif
}

bool WorkerNotificationState::beginWait(std::coroutine_handle<> continuation) {
    if (waitReady()) {
        return false;
    }

    waitPending_ = true;
    waitContinuation_ = continuation;
    try {
        auto handler = asio::bind_allocator(
            std::pmr::polymorphic_allocator<std::byte>(&waitResource_),
#ifdef _WIN32
            [this](const asio::error_code& error, std::size_t bytesTransferred) noexcept {
                completeWait(error, bytesTransferred);
            }
#else
            [this](const asio::error_code& error) noexcept { completeWait(error, 0); }
#endif
        );
#ifdef _WIN32
        receiverStream_->async_read_some(asio::buffer(readBuffer_), std::move(handler));
#else
        receiverStream_->async_wait(WorkerNotificationReceiver::wait_read, std::move(handler));
#endif
    } catch (...) {
        waitPending_ = false;
        waitContinuation_ = {};
        throw;
    }
    return true;
}

WorkerNotificationWaitStatus WorkerNotificationState::takeWaitResult() {
    requireWorker();
    if (waitPending_) {
        std::terminate();
    }
    if (waitError_) {
        const auto error = std::exchange(waitError_, {});
        throw std::system_error(error, "worker notification wait");
    }
    return waitResult_;
}

void WorkerNotificationState::close() {
    requireWorker();
    const auto old = activity_.fetch_or(kClosed, std::memory_order_acq_rel);
    if ((old & kClosed) != 0) {
        return;
    }

    if (receiverStream_ && waitPending_) {
        asio::error_code error;
        receiverStream_->cancel(error);
        if (error) {
            std::terminate();
        }
#ifdef _WIN32
        // Keep the connected server HANDLE until IOCP reports the canceled read.
        return;
#else
        receiverStream_->close(error);
        if (error) {
            std::terminate();
        }
        return;
#endif
    }
    retireReceiver();
}

bool WorkerNotificationState::enterProducer() noexcept {
    auto state = activity_.load(std::memory_order_acquire);
    for (;;) {
        if ((state & kClosed) != 0) {
            return false;
        }
        if ((state & kCountMask) == kCountMask) {
            std::terminate();
        }
        if (activity_.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
}

void WorkerNotificationState::leaveProducer() noexcept {
    const auto previous = activity_.fetch_sub(1, std::memory_order_release);
    if ((previous & kCountMask) == 0) {
        std::terminate();
    }
}

bool WorkerNotificationState::isClosed() const noexcept {
    return (activity_.load(std::memory_order_acquire) & kClosed) != 0;
}

void WorkerNotificationState::requireWorker() const {
    const bool isCurrent = runtimeHandle_ != nullptr ? runtimeHandle_->isCurrent() : loop_.isCurrent();
    if (!isCurrent) {
        throw std::logic_error("worker notification wait and close are worker-affine");
    }
}

asio::io_context& WorkerNotificationState::ioContext() const {
    return runtimeIoContext_ != nullptr ? *runtimeIoContext_ : loop_.ioContext();
}

void WorkerNotificationState::ensureReceiver() {
    if (receiverStream_) {
        return;
    }
    try {
        receiverStream_.emplace(ioContext());
        asio::error_code error;
        receiverStream_->assign(receiver_, error);
        if (error) {
            throw std::system_error(error, "assign worker notification receiver");
        }
#ifdef _WIN32
        receiver_ = nullptr;
#else
        receiver_ = -1;
#endif
    } catch (...) {
        static_cast<void>(activity_.fetch_or(kClosed, std::memory_order_acq_rel));
        if (receiverStream_) {
            asio::error_code ignored;
            if (receiverStream_->is_open()) {
                receiverStream_->close(ignored);
            }
            receiverStream_.reset();
        }
        retireReceiver();
        throw;
    }
}

bool WorkerNotificationState::drainReceiver(std::error_code& error) noexcept {
#ifdef _WIN32
    static_cast<void>(error);
    return false;
#else
    if (!receiverStream_ || !receiverStream_->is_open()) {
        return false;
    }
    std::uint64_t counter = 0;
    for (std::size_t interrupted = 0; interrupted < 64;) {
        const auto bytesRead = ::read(receiverStream_->native_handle(), &counter, sizeof(counter));
        if (bytesRead == static_cast<ssize_t>(sizeof(counter))) {
            return true;
        }
        if (bytesRead < 0 && errno == EINTR) {
            ++interrupted;
            continue;
        }
        if (bytesRead < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false;
        }
        if (bytesRead < 0) {
            error = std::error_code(errno, std::system_category());
        } else if (bytesRead == 0) {
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

void WorkerNotificationState::retireReceiver() noexcept {
#ifdef _WIN32
    if (receiverStream_) {
        const HANDLE handle = receiverStream_->is_open() ? receiverStream_->native_handle() : nullptr;
        disconnectPipeNoThrow(handle);
        asio::error_code error;
        if (receiverStream_->is_open()) {
            receiverStream_->close(error);
            if (error) {
                std::terminate();
            }
        }
        receiverStream_.reset();
    }
    if (validHandle(receiver_)) {
        disconnectPipeNoThrow(receiver_);
        closeHandle(receiver_);
    }
#else
    if (receiverStream_) {
        asio::error_code error;
        if (receiverStream_->is_open()) {
            receiverStream_->close(error);
            if (error) {
                std::terminate();
            }
        }
        receiverStream_.reset();
    }
    closeDescriptor(receiver_);
#endif
}

void WorkerNotificationState::completeWait(
    const std::error_code& error, std::size_t bytesTransferred) noexcept {
    if (!waitPending_ || !waitContinuation_) {
        std::terminate();
    }
    waitPending_ = false;
    const auto continuation = std::exchange(waitContinuation_, {});

    if (isClosed()) {
        waitError_.clear();
        waitResult_ = WorkerNotificationWaitStatus::kClosed;
        retireReceiver();
        continuation.resume();
        return;
    }

    if (error) {
        waitError_ = error;
#ifdef _WIN32
    } else if (bytesTransferred == 0) {
        waitError_ = std::make_error_code(std::errc::connection_reset);
#endif
    } else {
#ifdef _WIN32
        static_cast<void>(bytesTransferred);
        static_cast<void>(pending_.exchange(false, std::memory_order_acq_rel));
#else
        static_cast<void>(bytesTransferred);
        static_cast<void>(drainReceiver(waitError_));
        static_cast<void>(pending_.exchange(false, std::memory_order_acq_rel));
#endif
    }
    waitResult_ = WorkerNotificationWaitStatus::kNotified;
    continuation.resume();
}

}  // namespace ruvia::detail

namespace ruvia {

WorkerNotification::WaitAwaiter::WaitAwaiter(WorkerNotification& owner) noexcept
    : owner_(&owner) {}

bool WorkerNotification::WaitAwaiter::await_ready() {
    return owner_->state_->waitReady();
}

bool WorkerNotification::WaitAwaiter::await_suspend(std::coroutine_handle<> continuation) {
    return owner_->state_->beginWait(continuation);
}

WorkerNotificationWaitStatus WorkerNotification::WaitAwaiter::await_resume() {
    return owner_->state_->takeWaitResult();
}

WorkerNotification::WorkerNotification(EventLoop loop, WorkerNotificationOptions options)
    : state_(detail::makePmrObject<detail::WorkerNotificationState>(
          detail::processResource(), std::move(loop), options)),
      waitAwaiter_(*this) {}

WorkerNotification::WorkerNotification(
    WorkerRuntimeContext& runtime, WorkerNotificationOptions options)
    : state_(detail::makePmrObject<detail::WorkerNotificationState>(
          detail::processResource(), runtime, options)),
      waitAwaiter_(*this) {}

WorkerNotification::~WorkerNotification() = default;

WorkerNotificationStatus WorkerNotification::notify() noexcept {
    return state_->notify();
}

WorkerNotification::WaitAwaiter& WorkerNotification::wait() noexcept {
    return waitAwaiter_;
}

void WorkerNotification::close() {
    state_->close();
}

}  // namespace ruvia
