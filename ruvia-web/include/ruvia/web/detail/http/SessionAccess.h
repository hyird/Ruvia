#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpSetCookiePlan.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/context/ContextSessionState.h"

namespace ruvia::detail {

// Privileged access to a Context's session slot, used by the session middleware
// to load the stored blob and read what the handler left behind.
struct SessionAccess final {
    static void bind(Context& context, const SessionMiddleware* owner = nullptr) {
        context.sessionState().bind(owner);
    }

    // No callback chain: the bound SessionMiddleware owns this capability's
    // single response-commit stage for both buffered and long-lived routes.
    [[nodiscard]] static Task<void> commit(Context& context);

    static void observePresentedId(Context& context, std::string_view id) {
        context.sessionState().observePresentedId(id);
    }

    static void load(Context& context, std::string_view data) {
        context.sessionState().loadRecognized(data);
    }

    [[nodiscard]] static const ContextSessionState& state(const Context& context) noexcept {
        return context.sessionState();
    }
};

inline constexpr std::string_view session_rotation_script =
    "if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end "
    "if not redis.call('SET', KEYS[2], ARGV[1], 'EX', ARGV[2], 'NX') then return 0 end "
    "redis.call('DEL', KEYS[1]) return 1";

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

inline void appendSessionCookieHeader(HttpResponse& response, std::pmr::memory_resource* resource,
    std::string_view cookieName, std::string_view id, bool secure) {
    const CookieOptions options{
        .sameSite = CookieSameSite::kLax,
        .httpOnly = CookieAttributePolicy::kEmit,
        .secure = secure ? CookieAttributePolicy::kEmit : CookieAttributePolicy::kOmit,
    };
    const SetCookiePlan plan(cookieName, id, options);
    std::pmr::string setCookie(resource);
    setCookie.resize(plan.size());
    plan.write(setCookie.data());
    response.header("Set-Cookie", setCookie, {.mode = ruvia::HttpResponseHeaderMode::kAppend});
}

inline void appendExpiredSessionCookieHeader(HttpResponse& response,
    std::pmr::memory_resource* resource, std::string_view cookieName, bool secure) {
    const CookieOptions options{
        .sameSite = CookieSameSite::kLax,
        .maxAge = std::chrono::seconds(0),
        .httpOnly = CookieAttributePolicy::kEmit,
        .secure = secure ? CookieAttributePolicy::kEmit : CookieAttributePolicy::kOmit,
    };
    const SetCookiePlan plan(cookieName, "", options);
    std::pmr::string setCookie(resource);
    setCookie.resize(plan.size());
    plan.write(setCookie.data());
    response.header("Set-Cookie", setCookie, {.mode = ruvia::HttpResponseHeaderMode::kAppend});
}

}  // namespace ruvia::detail
