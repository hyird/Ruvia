#pragma once

#include "ruvia/http/Context.h"
#include "ruvia/http/HttpResponse.h"

#include <memory_resource>
#include <string_view>

namespace ruvia::detail {

// Privileged access to a Context's session slot, used by the session middleware
// to load the stored blob and read what the handler left behind.
struct SessionAccess final {
    static void setId(Context& context, std::string_view id) {
        context.sessionIdStorage().assign(id.data(), id.size());
    }

    static void load(Context& context, std::string_view data) {
        assignStableString(context.sessionDataStorage(), data);
    }

    [[nodiscard]] static bool dirty(const Context& context) noexcept {
        return context.sessionDirty_;
    }

    [[nodiscard]] static std::string_view id(const Context& context) noexcept {
        return context.sessionId();
    }

    [[nodiscard]] static std::string_view data(const Context& context) noexcept {
        return context.session();
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
    std::pmr::string setCookie(resource);
    setCookie.append("sid=");
    setCookie.append(id.data(), id.size());
    setCookie.append("; Path=/; HttpOnly; SameSite=Lax");
    if (secure) {
        setCookie.append("; Secure");
    }
    response.header("Set-Cookie", setCookie, {.append = true});
}

}  // namespace ruvia::detail
