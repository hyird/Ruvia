#include "ruvia/http/Csrf.h"

#include "CsrfInternal.h"

#include <array>
#include <cstddef>

#include <openssl/rand.h>

namespace ruvia::detail {

std::string_view generateCsrfToken(std::span<char> buffer) noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    constexpr std::size_t kRandomBytes = 24;  // 24 bytes -> 48 hex characters
    if (buffer.size() < kRandomBytes * 2) {
        return {};
    }
    unsigned char raw[kRandomBytes];
    if (RAND_bytes(raw, static_cast<int>(kRandomBytes)) != 1) {
        return {};
    }
    for (std::size_t i = 0; i < kRandomBytes; ++i) {
        buffer[i * 2] = kHex[(raw[i] >> 4) & 0x0F];
        buffer[i * 2 + 1] = kHex[raw[i] & 0x0F];
    }
    return std::string_view(buffer.data(), kRandomBytes * 2);
}

}  // namespace ruvia::detail

namespace ruvia {

Task<void> CsrfProtection::handle(Context& c, Next& next) {
    const auto method = c.req().method();
    const bool safe = method == "GET" ||
        method == "HEAD" ||
        method == "OPTIONS";
    const auto cookie = c.req().cookie("XSRF-TOKEN");
    if (!safe) {
        const auto header = c.req().header("X-XSRF-TOKEN");
        if (!cookie || cookie->empty() || !header || header->empty() ||
            !detail::csrfTokensEqual(*cookie, *header)) {
            c.res(c.error(403, "csrf_token_mismatch", "CSRF token missing or invalid"));
            co_return;
        }
    } else if (!cookie) {
        std::array<char, 64> buffer;
        const auto token = detail::generateCsrfToken(buffer);
        CookieOptions options;
        options.path = "/";
        options.sameSite = "Lax";
        options.secure = c.req().raw().isSecure();
        c.setCookie("XSRF-TOKEN", token, options);
    }
    co_await next();
}

}  // namespace ruvia
