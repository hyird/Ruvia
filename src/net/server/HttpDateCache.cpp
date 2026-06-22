#include "HttpDateCache.h"

#include <array>
#include <ctime>
#include <cstring>

namespace ruvia::detail {

namespace {

// "Date: <value>\r\n" — the cache is the single owner of this wire format.
// Both the full-line accessor (HTTP/1, h2c upgrade text response) and the
// value-only accessor (HPACK :date for HTTP/2) derive their views from these
// constants, so the framing lives in exactly one place.
inline constexpr std::string_view kDateHeaderPrefix = "Date: ";
inline constexpr std::string_view kDateHeaderSuffix = "\r\n";

struct DateCache final {
    std::array<char, 64> line{};
    std::size_t size{0};
    std::time_t second{-1};
};

[[nodiscard]] DateCache& workerDateCache() noexcept {
    thread_local DateCache cache;
    return cache;
}

}  // namespace

void refreshCachedDateHeader(std::time_t now) noexcept {
    auto& cache = workerDateCache();
    if (cache.second == now && cache.size != 0) {
        return;
    }

    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::memcpy(cache.line.data(), kDateHeaderPrefix.data(), kDateHeaderPrefix.size());
    const auto written = std::strftime(
        cache.line.data() + kDateHeaderPrefix.size(),
        cache.line.size() - kDateHeaderPrefix.size(),
        "%a, %d %b %Y %H:%M:%S GMT",
        &utc);
    if (written == 0) {
        cache.size = 0;
    } else {
        cache.size = kDateHeaderPrefix.size() + written;
        cache.line[cache.size++] = kDateHeaderSuffix[0];
        cache.line[cache.size++] = kDateHeaderSuffix[1];
    }
    cache.second = now;
}

std::string_view cachedDateHeader() noexcept {
    auto& cache = workerDateCache();
    const auto now = std::time(nullptr);
    if (cache.second != now || cache.size == 0) {
        refreshCachedDateHeader(now);
    }
    return std::string_view(cache.line.data(), cache.size);
}

std::string_view cachedDateValue() noexcept {
    const auto line = cachedDateHeader();
    constexpr std::size_t framing = kDateHeaderPrefix.size() + kDateHeaderSuffix.size();
    if (line.size() <= framing) {
        return {};
    }
    return line.substr(kDateHeaderPrefix.size(), line.size() - framing);
}

}  // namespace ruvia::detail
