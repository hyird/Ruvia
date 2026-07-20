#pragma once

#include "ruvia/web/Context.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/SetCookiePlan.h"

#include <cstddef>
#include <chrono>
#include <memory_resource>
#include <string_view>

namespace ruvia::detail {

// Privileged access to a Context's session slot, used by the session middleware
// to load the stored blob and read what the handler left behind.
struct SessionAccess final {
    static void observePresentedId(Context& context, std::string_view id) {
        context.sessionState_.observePresentedId(id);
    }

    static void load(Context& context, std::string_view data) {
        context.sessionState_.loadRecognized(data);
    }

    [[nodiscard]] static const ContextSessionState& state(const Context& context) noexcept {
        return context.sessionState_;
    }
};

[[nodiscard]] inline bool isValidSessionId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 128) {
        return false;
    }
    for (const char ch : id) {
        const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

inline void appendSessionCookieHeader(
    HttpResponse& response,
    std::pmr::memory_resource* resource,
    std::string_view id,
    bool secure) {
    CookieOptions options;
    options.httpOnly = true;
    options.secure = secure;
    options.sameSite = CookieSameSite::kLax;
    const SetCookiePlan plan("sid", id, options);
    std::pmr::string setCookie(resource);
    setCookie.resize(plan.size());
    plan.write(setCookie.data());
    response.header("Set-Cookie", setCookie, {.append = true});
}

inline void appendExpiredSessionCookieHeader(
    HttpResponse& response,
    std::pmr::memory_resource* resource,
    bool secure) {
    CookieOptions options;
    options.httpOnly = true;
    options.secure = secure;
    options.sameSite = CookieSameSite::kLax;
    options.maxAge = std::chrono::seconds(0);
    const SetCookiePlan plan("sid", "", options);
    std::pmr::string setCookie(resource);
    setCookie.resize(plan.size());
    plan.write(setCookie.data());
    response.header("Set-Cookie", setCookie, {.append = true});
}

}  // namespace ruvia::detail
