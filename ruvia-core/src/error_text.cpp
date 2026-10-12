#include "ruvia/core/error_text.h"

#ifdef _WIN32
#include <array>
#include <string_view>

#include <asio/error_code.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

}  // namespace ruvia
