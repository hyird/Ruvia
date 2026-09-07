#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/cookie/SetCookiePlan.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/context/ContextSessionState.h"

namespace ruvia::detail {

// Privileged access to a Context's session slot, used by the session middleware
// to load the stored blob and read what the handler left behind.
struct SessionAccess final {
    static void bind(Context& context) noexcept {
        context.sessionState().bind();
    }

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

enum class SessionPersistenceStep : std::uint8_t {
    kPersistCurrent,
    kDeleteOld,
};

struct SessionPersistencePlan final {
    std::array<SessionPersistenceStep, 2> steps{};
    std::size_t count{0};
};

[[nodiscard]] constexpr SessionPersistencePlan sessionPersistencePlan(
    std::string_view currentId, std::string_view oldIdToDelete) noexcept {
    SessionPersistencePlan plan;
    if (!currentId.empty()) {
        plan.steps[plan.count++] = SessionPersistenceStep::kPersistCurrent;
    }
    if (!oldIdToDelete.empty()) {
        plan.steps[plan.count++] = SessionPersistenceStep::kDeleteOld;
    }
    return plan;
}

enum class SessionCommitStep : std::uint8_t {
    kPersistCurrent,
    kDeleteOld,
    kPublishCurrentCookie,
};

struct SessionCommitPlan final {
    std::array<SessionCommitStep, 3> steps{};
    std::size_t count{0};
};

[[nodiscard]] constexpr SessionCommitPlan sessionCommitPlan(std::string_view currentId,
    std::string_view oldIdToDelete, bool publishCurrentCookie) noexcept {
    SessionCommitPlan plan;
    if (!currentId.empty()) {
        plan.steps[plan.count++] = SessionCommitStep::kPersistCurrent;
    }
    if (!oldIdToDelete.empty()) {
        plan.steps[plan.count++] = SessionCommitStep::kDeleteOld;
    }
    if (publishCurrentCookie && !currentId.empty()) {
        plan.steps[plan.count++] = SessionCommitStep::kPublishCurrentCookie;
    }
    return plan;
}

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
