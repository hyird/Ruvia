#include "native_wake_pair.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <bcrypt.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <sys/eventfd.h>
#endif

#include "timeout_duration.h"

namespace ruvia::detail {

void throw_native_wake_error(const char* operation, int error) {
    throw std::system_error(error, std::system_category(), operation);
}

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

native_wake_pair::native_wake_pair(native_wake_pair&& other) noexcept :
    sender_(std::exchange(other.sender_, -1)),
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
    explicit pipe_connect_operation(HANDLE pipe) :
        pipe_(pipe) {
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
        const auto remaining = ceil_milliseconds(deadline_value - now).count();
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
        if (!GetOverlappedResult(pipe_, &overlapped_, &bytes_transferred, FALSE) && GetLastError() != ERROR_OPERATION_ABORTED) {
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
    if (BCryptGenRandom(nullptr, random_bytes.data(), static_cast<ULONG>(random_bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
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

native_wake_pair::native_wake_pair(native_wake_pair&& other) noexcept :
    sender_(std::exchange(other.sender_, nullptr)),
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
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &security_descriptor.value_, nullptr)) {
        throw_native_wake_error("ConvertStringSecurityDescriptorToSecurityDescriptorW", static_cast<int>(GetLastError()));
    }
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.lpSecurityDescriptor = security_descriptor.value_;
    security_attributes.bInheritHandle = FALSE;

    pair.receiver_ = CreateNamedPipeW(pipe_name.data(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, &security_attributes);
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
        throw std::runtime_error("native worker notification pipe connected before its writer handle was opened");
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

        pair.sender_ = CreateFileW(pipe_name.data(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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
