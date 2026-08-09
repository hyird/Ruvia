#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace ruvia::detail {

struct ConfigHostRules final {
    bool rejectBrackets{false};
    bool rejectSingleColon{false};
};

inline constexpr ConfigHostRules kSeparatedPortHostRules{.rejectBrackets = true, .rejectSingleColon = true};

[[nodiscard]] inline bool isValidConfigHost(std::string_view host, ConfigHostRules rules = {}) noexcept {
    if (host.empty()) {
        return false;
    }

    std::size_t colonCount = 0;
    for (const auto ch : host) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte == 0x7F || byte == '/' || byte == '\\' || (rules.rejectBrackets && (byte == '[' || byte == ']'))) {
            return false;
        }
        colonCount += byte == ':' ? 1 : 0;
    }

    return !(rules.rejectSingleColon && colonCount == 1);
}

[[nodiscard]] inline bool isAsciiHostAlnum(unsigned char byte) noexcept {
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

[[nodiscard]] inline bool isAsciiHostDigit(unsigned char byte) noexcept {
    return byte >= '0' && byte <= '9';
}

[[nodiscard]] inline bool isValidSniHost(std::string_view host) noexcept {
    if (host.empty() || host.size() > 253 || host.back() == '.') {
        return false;
    }

    std::size_t labels = 0;
    bool sawDot = false;
    bool allLabelsNumeric = true;
    std::size_t labelStart = 0;
    while (labelStart < host.size()) {
        const auto labelEnd = host.find('.', labelStart);
        const auto end = labelEnd == std::string_view::npos ? host.size() : labelEnd;
        const auto labelLength = end - labelStart;
        if (labelLength == 0 || labelLength > 63) {
            return false;
        }

        const auto first = static_cast<unsigned char>(host[labelStart]);
        const auto last = static_cast<unsigned char>(host[end - 1]);
        if (!isAsciiHostAlnum(first) || !isAsciiHostAlnum(last)) {
            return false;
        }

        bool labelNumeric = true;
        for (std::size_t i = labelStart; i < end; ++i) {
            const auto byte = static_cast<unsigned char>(host[i]);
            if (!isAsciiHostAlnum(byte) && byte != '-') {
                return false;
            }
            labelNumeric = labelNumeric && isAsciiHostDigit(byte);
        }
        allLabelsNumeric = allLabelsNumeric && labelNumeric;
        ++labels;

        if (labelEnd == std::string_view::npos) {
            break;
        }
        sawDot = true;
        labelStart = end + 1;
    }

    return !(sawDot && labels == 4 && allLabelsNumeric);
}

inline void ensureConfigHost(std::string_view host, const char* emptyMessage, const char* invalidMessage, ConfigHostRules rules = {}) {
    if (host.empty()) {
        throw std::invalid_argument(emptyMessage);
    }
    if (!isValidConfigHost(host, rules)) {
        throw std::invalid_argument(invalidMessage);
    }
}

inline void ensureSniHost(std::string_view host, const char* emptyMessage, const char* invalidMessage) {
    if (host.empty()) {
        throw std::invalid_argument(emptyMessage);
    }
    if (!isValidSniHost(host)) {
        throw std::invalid_argument(invalidMessage);
    }
}

inline void ensurePositiveSize(std::size_t value, const char* message) {
    if (value == 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensurePositiveOptionalSize(const std::optional<std::size_t>& value, const char* message) {
    if (value.has_value() && *value == 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensureNonZeroPort(std::uint16_t port, const char* message) {
    if (port == 0) {
        throw std::invalid_argument(message);
    }
}

template <typename Rep, typename Period>
void ensurePositiveDuration(std::chrono::duration<Rep, Period> value, const char* message) {
    if (value.count() <= 0) {
        throw std::invalid_argument(message);
    }
}

template <typename Rep, typename Period>
void ensurePositiveOptionalDuration(const std::optional<std::chrono::duration<Rep, Period>>& value, const char* message) {
    if (value.has_value() && value->count() <= 0) {
        throw std::invalid_argument(message);
    }
}

template <typename FirstDuration, typename... RestDurations>
void ensurePositiveOptionalDurations(const char* message, const FirstDuration& first, const RestDurations&... rest) {
    ensurePositiveOptionalDuration(first, message);
    (ensurePositiveOptionalDuration(rest, message), ...);
}

}  // namespace ruvia::detail
