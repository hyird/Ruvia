#include "ruvia/core/failure_report.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <typeinfo>

#ifdef _WIN32
#include <array>
#include <charconv>
#include <limits>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <asio/error_code.hpp>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <optional>

#include "ruvia/core/socket.h"
#include "ruvia/core/task.h"

#if RUVIA_SANITIZER_ALLOCATOR
#include <cstdlib>
#if defined(_WIN32)
#include <malloc.h>
#endif
#else
#include <mimalloc.h>
#endif

namespace ruvia {

namespace {

#ifdef _WIN32
// Asio reports socket and resolver errors in its own system category, whose
// message() formats Win32 codes in the ANSI code page like std::system_category().
[[nodiscard]] bool is_windows_system_category(const std::error_category& category) noexcept {
    return category == std::system_category() || category == asio::system_category();
}

[[nodiscard]] bool is_message_trailing_space(wchar_t value) noexcept {
    return value == L' ' || value == L'\t' || value == L'\r' || value == L'\n';
}

// The Win32 system message for `value` in UTF-8, or empty when there is none.
[[nodiscard]] std::string windows_system_message(int value) {
    std::array<wchar_t, 1024> buffer;
    // MAX_WIDTH_MASK folds the message's line breaks into spaces.
    auto length = ::FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK, nullptr,
        static_cast<DWORD>(value), 0, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
    while (length > 0 && is_message_trailing_space(buffer[length - 1])) {
        --length;
    }
    if (length == 0) {
        return {};
    }
    const auto wide_length = static_cast<int>(length);
    const auto size = ::WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide_length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide_length, text.data(), size, nullptr, nullptr) != size) {
        return {};
    }
    return text;
}
#endif

// Reporting is loudest exactly when the process is least able to afford it: an
// allocation failure or a dead upstream fails every connection at once, and
// stderr is a synchronous write that can block the reporting thread when it is
// redirected to a slow or full file. A flood would then slow the very recovery
// it is describing, and bury the first failure -- the informative one -- under
// thousands of identical consequences.
//
// So the reporter emits at most burst lines per window and counts the rest.
// The count is not lost: the next line that gets through carries it.
constexpr std::size_t burst = 20;
constexpr auto window = std::chrono::seconds(1);

std::mutex& rate_mutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

struct rate_state final {
    std::chrono::steady_clock::time_point window_start_{};
    std::size_t emitted_{0};
    std::size_t suppressed_{0};
    bool started_{false};
};

rate_state& get_rate_state() noexcept {
    static rate_state state;
    return state;
}

// Decides whether this failure gets a line. Returns the number of failures
// suppressed since the last emitted line, to be reported alongside it.
struct rate_decision final {
    bool emit_{false};
    std::size_t suppressed_{0};
};

[[nodiscard]] rate_decision admit() noexcept {
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard guard(rate_mutex());
    auto& state_value = get_rate_state();

    if (!state_value.started_ || now - state_value.window_start_ >= window) {
        state_value.started_ = true;
        state_value.window_start_ = now;
        state_value.emitted_ = 0;
    }
    if (state_value.emitted_ >= burst) {
        ++state_value.suppressed_;
        return {};
    }
    ++state_value.emitted_;
    rate_decision decision;
    decision.emit_ = true;
    decision.suppressed_ = state_value.suppressed_;
    state_value.suppressed_ = 0;
    return decision;
}

// Holds the stdio lock of stderr, so the pieces of one line reach it together,
// without interleaving with any concurrent stdio writer.
class stream_lock final {
public:
    stream_lock() noexcept {
#if defined(_WIN32)
        _lock_file(stderr);
#else
        flockfile(stderr);
#endif
    }

    ~stream_lock() {
#if defined(_WIN32)
        _unlock_file(stderr);
#else
        funlockfile(stderr);
#endif
    }

    stream_lock(const stream_lock&) = delete;
    stream_lock& operator=(const stream_lock&) = delete;
};

void write_text(std::string_view text) noexcept {
    std::fwrite(text.data(), 1, text.size(), stderr);
}

void write_line(std::string_view context_value, std::string_view what, std::size_t occurrences,
    std::size_t suppressed) noexcept {
    char occurrences_text[32] = "";
    if (occurrences > 1) {
        std::snprintf(occurrences_text, sizeof(occurrences_text), " (x%zu)", occurrences);
    }
    char suppressed_text[48] = "";
    if (suppressed != 0) {
        std::snprintf(suppressed_text, sizeof(suppressed_text), " (+%zu suppressed)", suppressed);
    }
    // Views are written by their exact length: they need not be NUL-terminated
    // and may be longer than printf's int precision can express.
    const stream_lock lock;
    write_text("ruvia: ");
    write_text(context_value);
    write_text(" failed: ");
    write_text(what);
    write_text(occurrences_text);
    write_text(suppressed_text);
    write_text("\n");
}

}  // namespace

std::string error_code_text(const std::error_code& error) {
#ifdef _WIN32
    if (is_windows_system_category(error.category())) {
        auto text = windows_system_message(error.value());
        if (text.empty()) {
            std::array<char, std::numeric_limits<int>::digits10 + 3> digits;
            const auto end = std::to_chars(digits.data(), digits.data() + digits.size(), error.value()).ptr;
            text.assign(error.category().name()).append(":").append(digits.data(), end);
        }
        return text;
    }
#endif
    return error.message();
}

std::string system_error_text(const std::system_error& error) {
#ifdef _WIN32
    if (is_windows_system_category(error.code().category())) {
        const std::string_view what(error.what());
        const auto native = error.code().message();
        auto text = error_code_text(error.code());
        if (!what.ends_with(native)) {
            return text;
        }
        std::string result(what.substr(0, what.size() - native.size()));
        result.append(text);
        return result;
    }
#endif
    return error.what();
}
void report_unhandled_failure(
    std::string_view context_value, std::exception_ptr exception, std::size_t occurrences) noexcept {
    if (exception == nullptr) {
        return;
    }
    const auto decision = admit();
    if (!decision.emit_) {
        return;
    }
    // Rethrowing is the only way to read the exception's message. A nested
    // failure here (a what() that allocates and fails) still leaves the
    // unknown-exception line below, so the report never disappears entirely.
    try {
        std::rethrow_exception(exception);
    } catch (const std::system_error& error) {
        // On Windows what() carries a system-category message in the ANSI code
        // page; the line must stay UTF-8.
        try {
            const auto text = system_error_text(error);
            write_line(context_value, text, occurrences, decision.suppressed_);
        } catch (...) {
            // Formatting failed to allocate; the native text still reports it.
            write_line(context_value, error.what(), occurrences, decision.suppressed_);
        }
    } catch (const std::exception& error) {
        write_line(context_value, error.what(), occurrences, decision.suppressed_);
    } catch (...) {
        write_line(context_value, "unknown exception", occurrences, decision.suppressed_);
    }
}

bool equivalent_failures(const std::exception_ptr& left, const std::exception_ptr& right) noexcept {
    if (left == right) {
        return true;
    }
    if (left == nullptr || right == nullptr) {
        return false;
    }
    // A caught exception may be a copy that dies with its catch block (MSVC's
    // rethrow_exception copies), so both messages are compared while both
    // handlers are still active; a what() pointer never leaves its catch.
    bool equivalent = false;
    try {
        std::rethrow_exception(left);
    } catch (const std::exception& left_error) {
        try {
            std::rethrow_exception(right);
        } catch (const std::exception& right_error) {
            equivalent = typeid(left_error) == typeid(right_error) &&
                         std::string_view(left_error.what()) == std::string_view(right_error.what());
        } catch (...) {
        }
    } catch (...) {
    }
    return equivalent;
}

}  // namespace ruvia

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define RUVIA_SANITIZER_ALLOCATOR 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define RUVIA_SANITIZER_ALLOCATOR 1
#else
#define RUVIA_SANITIZER_ALLOCATOR 0
#endif
#else
#define RUVIA_SANITIZER_ALLOCATOR 0
#endif

namespace ruvia {

namespace {

[[nodiscard]] void* allocate_mimalloc(std::size_t bytes_value, std::size_t alignment) {
#if RUVIA_SANITIZER_ALLOCATOR
    // The system allocators may answer a zero-byte request with null.
    const std::size_t size = bytes_value == 0 ? 1 : bytes_value;
#if defined(_WIN32)
    if (void* block = _aligned_malloc(size, (std::max)(alignment, alignof(void*)))) {
        return block;
    }
#else
    void* block = nullptr;
    if (::posix_memalign(&block, (std::max)(alignment, alignof(void*)), size) == 0 && block != nullptr) {
        return block;
    }
#endif
#else
    // mimalloc returns a unique, aligned block for zero bytes.
    if (void* block = mi_malloc_aligned(bytes_value, alignment)) {
        return block;
    }
#endif
    throw std::bad_alloc();
}

void free_mimalloc(void* block) noexcept {
#if RUVIA_SANITIZER_ALLOCATOR
#if defined(_WIN32)
    _aligned_free(block);
#else
    std::free(block);
#endif
#else
    mi_free(block);
#endif
}

}  // namespace

namespace detail {
void* task_frame_allocate(std::size_t bytes_value) {
    return allocate_mimalloc(bytes_value, alignof(std::max_align_t));
}

void task_frame_deallocate(void* pointer) noexcept {
    free_mimalloc(pointer);
}

void* task_frame_allocate_aligned(std::size_t bytes_value, std::size_t alignment) {
    return allocate_mimalloc(bytes_value, alignment);
}

}  // namespace detail

void collect_thread_heap() noexcept {
#if !RUVIA_SANITIZER_ALLOCATOR
    // mi_collect() serves the calling thread's heap; forcing also purges the
    // shared arenas.
    mi_collect(true);
#endif
}
}  // namespace ruvia

namespace ruvia {

namespace {

// The lexical shape is checked here because the platform parsers behind
// asio::ip::make_address are not strict validators: Windows accepts
// `a.b.c.d:port`, `[v6]:port`, and fills in missing IPv4 octets. The address
// text must therefore already be a complete literal before it reaches asio.

[[nodiscard]] constexpr bool is_decimal_digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

[[nodiscard]] constexpr bool is_hex_digit(char value) noexcept {
    return is_decimal_digit(value) || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

// Exactly four decimal octets, each 1-3 digits without leading zeros and at
// most 255. Returns the bytes in network order on success.
[[nodiscard]] std::optional<asio::ip::address_v4::bytes_type> parse_ipv4_literal(std::string_view text) noexcept {
    asio::ip::address_v4::bytes_type bytes{};
    std::size_t position = 0;
    for (std::size_t octet = 0; octet < bytes.size(); ++octet) {
        if (octet != 0) {
            if (position >= text.size() || text[position] != '.') {
                return std::nullopt;
            }
            ++position;
        }
        std::size_t digits = 0;
        unsigned value = 0;
        while (position < text.size() && is_decimal_digit(text[position])) {
            if (digits == 3 || (digits == 1 && value == 0)) {
                return std::nullopt;
            }
            value = value * 10 + static_cast<unsigned>(text[position] - '0');
            ++digits;
            ++position;
        }
        if (digits == 0 || value > 255) {
            return std::nullopt;
        }
        bytes[octet] = static_cast<unsigned char>(value);
    }
    if (position != text.size()) {
        return std::nullopt;
    }
    return bytes;
}

// Hex digits, ':' and '.' (for an embedded IPv4 tail), optionally followed by
// one '%' and a non-empty zone that contains no further '%'; nothing else.
[[nodiscard]] bool is_ipv6_literal_shape(std::string_view text) noexcept {
    const auto zone = text.find('%');
    const auto address = text.substr(0, zone);
    if (address.find(':') == std::string_view::npos) {
        return false;
    }
    for (const char value : address) {
        if (!is_hex_digit(value) && value != ':' && value != '.') {
            return false;
        }
    }
    if (address.find('.') != std::string_view::npos) {
        const auto last_colon = address.rfind(':');
        if (last_colon == std::string_view::npos || !parse_ipv4_literal(address.substr(last_colon + 1)).has_value()) {
            return false;
        }
    }
    if (zone == std::string_view::npos) {
        return true;
    }
    const auto zone_name = text.substr(zone + 1);
    if (zone_name.empty()) {
        return false;
    }
    for (const unsigned char ch : zone_name) {
        if (ch <= 0x20 || ch >= 0x7f || ch == '%' || ch == ':' || ch == '[' || ch == ']') {
            return false;
        }
    }
    if (zone_name.front() == '+' || zone_name.front() == '-') {
        return false;
    }
    if (is_decimal_digit(zone_name.front())) {
        if (zone_name.front() == '0') {
            return false;
        }
        std::uint64_t scope_value = 0;
        for (const char ch : zone_name) {
            if (!is_decimal_digit(ch)) {
                return false;
            }
            scope_value = scope_value * 10 + static_cast<std::uint64_t>(ch - '0');
            if (scope_value > std::numeric_limits<asio::ip::scope_id_type>::max()) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

// Consumes the complete view, including checking for embedded NUL. Common
// address text uses stack storage; unusually long scope names use temporary heap
// storage that is released before returning the independent address value.
std::optional<asio::ip::address> parse_ip_address(std::string_view text) {
    if (text.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    if (const auto v4 = parse_ipv4_literal(text)) {
        return asio::ip::address(asio::ip::address_v4(*v4));
    }
    if (!is_ipv6_literal_shape(text)) {
        return std::nullopt;
    }
    std::error_code error;
    std::array<char, 64> terminated;
    const auto address = [&] {
        if (text.size() < terminated.size()) {
            std::memcpy(terminated.data(), text.data(), text.size());
            terminated[text.size()] = '\0';
            return asio::ip::make_address_v6(terminated.data(), error);
        }
        const std::string owned(text);
        return asio::ip::make_address_v6(owned.c_str(), error);
    }();
    if (error || (text.find('%') != std::string_view::npos && address.scope_id() == 0)) {
        return std::nullopt;
    }
    return asio::ip::address(address);
}

}  // namespace ruvia
