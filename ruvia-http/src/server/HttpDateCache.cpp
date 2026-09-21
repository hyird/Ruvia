#include "ruvia/http/detail/server/HttpDateCache.h"

#include <array>
#include <cstring>
#include <ctime>

#include "ruvia/http/detail/field/HttpImfFixdate.h"

namespace ruvia::detail {

namespace {

// "Date: <value>\r\n" -- the cache is the single owner of this wire format.
// Both the full-line accessor (HTTP/1 text response) and the
// value-only accessor (HPACK :date for HTTP/2) derive their views from these
// constants, so the framing lives in exactly one place.
inline constexpr std::string_view kDateHeaderPrefix = "Date: ";
inline constexpr std::string_view kDateHeaderSuffix = "\r\n";

struct DateCache final {
    std::array<char, 64> line{};
    std::size_t size{0};
    std::time_t second{};
    bool initialized{false};
};

[[nodiscard]] DateCache& workerDateCache() noexcept {
    thread_local DateCache cache;
    return cache;
}

}  // namespace

std::string_view cachedDateHeader(std::time_t now) noexcept {
    auto& cache = workerDateCache();
    if (!cache.initialized || cache.second != now) {
        cache.second = now;
        cache.initialized = true;
        cache.size = 0;
        // RFC 9110 section 6.6.1: do not generate Date without a usable clock.
        if (now != std::time_t{-1}) {
            if (const auto date = httpFormatDate(now)) {
                std::memcpy(cache.line.data(), kDateHeaderPrefix.data(), kDateHeaderPrefix.size());
                std::memcpy(cache.line.data() + kDateHeaderPrefix.size(), date->data(), date->size());
                cache.size = kDateHeaderPrefix.size() + date->size();
                cache.line[cache.size++] = kDateHeaderSuffix[0];
                cache.line[cache.size++] = kDateHeaderSuffix[1];
            }
        }
    }
    return std::string_view(cache.line.data(), cache.size);
}

std::string_view cachedDateValue(std::time_t now) noexcept {
    const auto line = cachedDateHeader(now);
    constexpr std::size_t framing = kDateHeaderPrefix.size() + kDateHeaderSuffix.size();
    if (line.size() <= framing) {
        return {};
    }
    return line.substr(kDateHeaderPrefix.size(), line.size() - framing);
}

}  // namespace ruvia::detail
