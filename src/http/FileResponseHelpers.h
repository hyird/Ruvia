#pragma once

#include "ruvia/detail/NativePath.h"
#include "FileResponseResource.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace ruvia::detail {

template <typename Char>
[[nodiscard]] inline std::basic_string_view<Char> httpFileExtension(
    std::basic_string_view<Char> path) noexcept {
    for (std::size_t i = path.size(); i > 0; --i) {
        const auto c = path[i - 1];
        if (c == static_cast<Char>('/') || c == static_cast<Char>('\\')) {
            return {};
        }
        if (c == static_cast<Char>('.')) {
            return path.substr(i - 1);
        }
    }
    return {};
}

template <typename Char>
[[nodiscard]] inline bool httpExtensionEquals(
    std::basic_string_view<Char> extension,
    std::string_view expected) noexcept {
    if (extension.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        auto c = extension[i];
        if (c >= static_cast<Char>('A') && c <= static_cast<Char>('Z')) {
            c = static_cast<Char>(c + static_cast<Char>('a' - 'A'));
        }
        if (c != static_cast<Char>(expected[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::pmr::string httpLowerFileExtension(
    const std::filesystem::path& path,
    std::pmr::memory_resource* resource = fileResponseResource()) {
    const auto native = nativePathView(path);
    const auto source = httpFileExtension(native);
    if (source.empty()) {
        return std::pmr::string(resource);
    }
    std::pmr::string extension(resource);
    extension.reserve(source.size());
    for (const auto c : source) {
        auto out = c;
        if (out >= static_cast<NativePathChar>('A') &&
            out <= static_cast<NativePathChar>('Z')) {
            out = static_cast<NativePathChar>(out + static_cast<NativePathChar>('a' - 'A'));
        }
        extension.push_back(static_cast<char>(out));
    }
    return extension;
}

[[nodiscard]] inline std::string_view httpGuessContentType(const std::filesystem::path& path) {
    const auto native = nativePathView(path);
    const auto extension = httpFileExtension(native);
    if (httpExtensionEquals(extension, ".html") || httpExtensionEquals(extension, ".htm")) {
        return "text/html; charset=utf-8";
    }
    if (httpExtensionEquals(extension, ".css")) {
        return "text/css; charset=utf-8";
    }
    if (httpExtensionEquals(extension, ".js") || httpExtensionEquals(extension, ".mjs")) {
        return "text/javascript; charset=utf-8";
    }
    if (httpExtensionEquals(extension, ".json")) {
        return "application/json; charset=utf-8";
    }
    if (httpExtensionEquals(extension, ".txt") || httpExtensionEquals(extension, ".log")) {
        return "text/plain; charset=utf-8";
    }
    if (httpExtensionEquals(extension, ".png")) {
        return "image/png";
    }
    if (httpExtensionEquals(extension, ".jpg") || httpExtensionEquals(extension, ".jpeg")) {
        return "image/jpeg";
    }
    if (httpExtensionEquals(extension, ".gif")) {
        return "image/gif";
    }
    if (httpExtensionEquals(extension, ".svg")) {
        return "image/svg+xml";
    }
    if (httpExtensionEquals(extension, ".wasm")) {
        return "application/wasm";
    }
    return "application/octet-stream";
}

struct HttpByteRange final {
    std::uint64_t offset{0};
    std::uint64_t length{0};
};

[[nodiscard]] inline std::optional<std::uint64_t> httpParseUnsigned(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    std::uint64_t parsed = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] inline std::optional<HttpByteRange> httpParseByteRange(
    std::string_view header,
    std::uint64_t size) noexcept {
    constexpr std::string_view prefix = "bytes=";
    if (header.size() <= prefix.size() || header.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }

    auto spec = header.substr(prefix.size());
    if (spec.find(',') != std::string_view::npos || size == 0) {
        return std::nullopt;
    }

    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }

    const auto first = spec.substr(0, dash);
    const auto last = spec.substr(dash + 1);
    if (first.empty()) {
        const auto suffix = httpParseUnsigned(last);
        if (!suffix || *suffix == 0) {
            return std::nullopt;
        }
        const auto length = std::min(*suffix, size);
        return HttpByteRange{size - length, length};
    }

    const auto start = httpParseUnsigned(first);
    if (!start || *start >= size) {
        return std::nullopt;
    }

    std::uint64_t end = size - 1;
    if (!last.empty()) {
        const auto parsedEnd = httpParseUnsigned(last);
        if (!parsedEnd || *parsedEnd < *start) {
            return std::nullopt;
        }
        end = std::min(*parsedEnd, size - 1);
    }

    return HttpByteRange{*start, end - *start + 1};
}

[[nodiscard]] inline bool httpByteRangeSetHasMultiple(std::string_view header) noexcept {
    constexpr std::string_view prefix = "bytes=";
    if (header.size() <= prefix.size() || header.substr(0, prefix.size()) != prefix) {
        return false;
    }
    return header.substr(prefix.size()).find(',') != std::string_view::npos;
}

template <typename StringT>
inline void httpAppendUnsignedTo(StringT& output, std::uint64_t value) {
    std::array<char, 32> buffer;
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec == std::errc{}) {
        output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
    }
}

inline void httpAppendUnsigned(std::pmr::string& output, std::uint64_t value) {
    httpAppendUnsignedTo(output, value);
}

[[nodiscard]] inline std::pmr::string httpMakeFileEtag(
    std::pmr::memory_resource* resource,
    std::uint64_t size,
    std::filesystem::file_time_type modified) {
    std::pmr::string output(resource);
    output.reserve(43);
    output.push_back('"');
    httpAppendUnsigned(output, size);
    output.push_back('-');
    httpAppendUnsigned(output, static_cast<std::uint64_t>(modified.time_since_epoch().count()));
    output.push_back('"');
    return output;
}

[[nodiscard]] inline std::pmr::string httpFormatDate(
    std::pmr::memory_resource* resource,
    std::time_t time) {
    std::pmr::string output(resource);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    output.resize_and_overwrite(30, [&](char* data, std::size_t count) {
        return std::strftime(data, count, "%a, %d %b %Y %H:%M:%S GMT", &utc);
    });
    return output;
}

[[nodiscard]] inline std::time_t httpFileTimeToTimeT(std::filesystem::file_time_type value) noexcept {
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(systemTime);
}

[[nodiscard]] inline int httpMonthIndex(std::string_view value) noexcept {
    constexpr std::array<std::string_view, 12> months{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (std::size_t i = 0; i < months.size(); ++i) {
        if (value == months[i]) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

[[nodiscard]] inline std::optional<int> httpParseFixedDigits(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    int parsed = 0;
    for (const auto c : value) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        parsed = parsed * 10 + (c - '0');
    }
    return parsed;
}

[[nodiscard]] inline std::int64_t httpDaysFromCivil(int year, unsigned month, unsigned day) noexcept {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const auto yoe = static_cast<unsigned>(year - era * 400);
    const auto doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

[[nodiscard]] inline std::optional<std::time_t> httpParseImfFixdate(std::string_view value) noexcept {
    value = value.size() == 29 ? value : std::string_view{};
    if (value.empty() ||
        value[3] != ',' || value[4] != ' ' || value[7] != ' ' || value[11] != ' ' ||
        value[16] != ' ' || value[19] != ':' || value[22] != ':' || value[25] != ' ' ||
        value.substr(26, 3) != "GMT") {
        return std::nullopt;
    }

    const auto day = httpParseFixedDigits(value.substr(5, 2));
    const auto month = httpMonthIndex(value.substr(8, 3));
    const auto year = httpParseFixedDigits(value.substr(12, 4));
    const auto hour = httpParseFixedDigits(value.substr(17, 2));
    const auto minute = httpParseFixedDigits(value.substr(20, 2));
    const auto second = httpParseFixedDigits(value.substr(23, 2));
    if (!day || month == 0 || !year || !hour || !minute || !second ||
        *day < 1 || *day > 31 || *hour > 23 || *minute > 59 || *second > 60) {
        return std::nullopt;
    }

    const auto days = httpDaysFromCivil(*year, static_cast<unsigned>(month), static_cast<unsigned>(*day));
    const auto total = days * 86400 +
        static_cast<std::int64_t>(*hour) * 3600 +
        static_cast<std::int64_t>(*minute) * 60 +
        static_cast<std::int64_t>(*second);
    if constexpr (std::numeric_limits<std::time_t>::is_signed) {
        if (total < static_cast<std::int64_t>((std::numeric_limits<std::time_t>::min)()) ||
            total > static_cast<std::int64_t>((std::numeric_limits<std::time_t>::max)())) {
            return std::nullopt;
        }
    } else if (total < 0 ||
               static_cast<std::uint64_t>(total) > static_cast<std::uint64_t>((std::numeric_limits<std::time_t>::max)())) {
        return std::nullopt;
    }
    return static_cast<std::time_t>(total);
}

[[nodiscard]] inline std::string_view httpTrimWeakEtagPrefix(std::string_view value) noexcept {
    if (value.size() >= 2 && value[0] == 'W' && value[1] == '/') {
        value.remove_prefix(2);
    }
    return value;
}

[[nodiscard]] inline bool httpIsWeakEtag(std::string_view value) noexcept {
    return value.size() >= 2 && value[0] == 'W' && value[1] == '/';
}

[[nodiscard]] inline bool httpStrongEtagEquals(std::string_view left, std::string_view right) noexcept {
    return !httpIsWeakEtag(left) && !httpIsWeakEtag(right) && left == right;
}

[[nodiscard]] inline bool httpWeakEtagEquals(std::string_view left, std::string_view right) noexcept {
    return httpTrimWeakEtagPrefix(left) == httpTrimWeakEtagPrefix(right);
}

}  // namespace ruvia::detail
