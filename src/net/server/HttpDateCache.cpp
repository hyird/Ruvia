#include "HttpDateCache.h"

#include <array>
#include <ctime>
#include <cstring>

namespace ruvia::detail {

namespace {

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
    constexpr std::string_view prefix = "Date: ";
    std::memcpy(cache.line.data(), prefix.data(), prefix.size());
    const auto written = std::strftime(
        cache.line.data() + prefix.size(),
        cache.line.size() - prefix.size(),
        "%a, %d %b %Y %H:%M:%S GMT",
        &utc);
    if (written == 0) {
        cache.size = 0;
    } else {
        cache.size = prefix.size() + written;
        cache.line[cache.size++] = '\r';
        cache.line[cache.size++] = '\n';
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

}  // namespace ruvia::detail
