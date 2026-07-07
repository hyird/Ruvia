#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace ruvia::detail {

struct ConfigHostRules final {
    bool rejectBrackets{false};
    bool rejectSingleColon{false};
};

inline constexpr ConfigHostRules kSeparatedPortHostRules{
    .rejectBrackets = true,
    .rejectSingleColon = true};

[[nodiscard]] inline bool isValidConfigHost(
    std::string_view host,
    ConfigHostRules rules = {}) noexcept {
    if (host.empty()) {
        return false;
    }

    std::size_t colonCount = 0;
    for (const auto ch : host) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte == 0x7F ||
            byte == '/' || byte == '\\' ||
            (rules.rejectBrackets && (byte == '[' || byte == ']'))) {
            return false;
        }
        colonCount += byte == ':' ? 1 : 0;
    }

    return !(rules.rejectSingleColon && colonCount == 1);
}

inline void ensureConfigHost(
    std::string_view host,
    const char* emptyMessage,
    const char* invalidMessage,
    ConfigHostRules rules = {}) {
    if (host.empty()) {
        throw std::invalid_argument(emptyMessage);
    }
    if (!isValidConfigHost(host, rules)) {
        throw std::invalid_argument(invalidMessage);
    }
}

inline void ensurePositiveSize(std::size_t value, const char* message) {
    if (value == 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensureNonZeroPort(std::uint16_t port, const char* message) {
    if (port == 0) {
        throw std::invalid_argument(message);
    }
}

template <typename Rep, typename Period>
void ensureNonNegativeDuration(std::chrono::duration<Rep, Period> value, const char* message) {
    if (value.count() < 0) {
        throw std::invalid_argument(message);
    }
}

template <typename FirstDuration, typename... RestDurations>
void ensureNonNegativeDurations(
    const char* message,
    FirstDuration first,
    RestDurations... rest) {
    if (first.count() < 0 || ((rest.count() < 0) || ...)) {
        throw std::invalid_argument(message);
    }
}

template <typename Rep, typename Period>
void ensurePositiveDuration(std::chrono::duration<Rep, Period> value, const char* message) {
    if (value.count() <= 0) {
        throw std::invalid_argument(message);
    }
}

}  // namespace ruvia::detail
