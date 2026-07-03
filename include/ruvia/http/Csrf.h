#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "ruvia/app/Task.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/Cookies.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/MiddlewareRuntime.h"
#include "ruvia/http/Next.h"

namespace ruvia {

namespace detail {

// Fills `buffer` (which must hold at least 48 bytes) with a cryptographically
// random hex token and returns a view of it, or an empty view on RNG failure.
[[nodiscard]] std::string_view generateCsrfToken(std::span<char> buffer) noexcept;

// Length-checked constant-time compare of the double-submit CSRF token, matching
// the project convention of comparing attacker-reproducible tokens without an
// early-out on the first differing byte.
[[nodiscard]] inline bool csrfTokensEqual(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return diff == 0;
}

}  // namespace detail

// Stateless CSRF protection using the double-submit-cookie pattern (no
// server-side session store needed, so it works across SO_REUSEPORT workers).
// A safe request (GET/HEAD/OPTIONS) without an XSRF-TOKEN cookie is issued a
// fresh one (readable by JavaScript so a SPA can echo it). An unsafe request
// must repeat that cookie's value in the X-XSRF-TOKEN header; a missing or
// mismatched token is rejected with 403. Register with app.use<CsrfProtection>()
// or on a controller group.
class CsrfProtection final : public Middleware<CsrfProtection> {
public:
    Task<void> handle(Context& c, Next& next) {
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
};

}  // namespace ruvia
