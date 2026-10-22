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

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <asio/error_code.hpp>
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
            text.assign(error.category().name()).append(":").append(std::to_string(error.value()));
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
