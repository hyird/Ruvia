#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/http_response.h"
#include "ruvia/http/http_set_cookie_plan.h"
#include "ruvia/web/context.h"

#include "context/context_session_state.h"

namespace ruvia::detail {

// Privileged access to a context's session slot, used by the session middleware
// to load the stored blob and read what the handler left behind.
struct session_access final {
    static void bind(context& context_value, const session_middleware* owner = nullptr) {
        context_value.session_state().bind(owner);
    }

    // No callback chain: the bound session_middleware owns this capability's
    // single response-commit stage for both buffered and long-lived routes.
    [[nodiscard]] static task<void> commit(context& context);

    static void observe_presented_id(context& context_value, std::string_view id) {
        context_value.session_state().observe_presented_id(id);
    }

    static void load(context& context_value, std::string_view data) {
        context_value.session_state().load_recognized(data);
    }

    [[nodiscard]] static const context_session_state& state(const context& context_value) noexcept {
        return context_value.session_state();
    }
};

inline constexpr std::string_view session_rotation_script =
    "if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end "
    "if not redis.call('SET', KEYS[2], ARGV[1], 'EX', ARGV[2], 'NX') then return 0 end "
    "redis.call('DEL', KEYS[1]) return 1";

[[nodiscard]] inline bool is_valid_session_id(std::string_view id) noexcept {
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

inline void append_session_cookie_header(http_response& response, std::pmr::memory_resource* resource,
    std::string_view cookie_name, std::string_view id, bool secure) {
    const cookie_options options{
        .same_site_ = cookie_same_site::lax,
        .http_only_ = cookie_attribute_policy::emit,
        .secure_ = secure ? cookie_attribute_policy::emit : cookie_attribute_policy::omit,
    };
    const set_cookie_plan plan(cookie_name, id, options);
    std::pmr::string set_cookie(resource);
    set_cookie.resize(plan.size());
    plan.write(set_cookie.data());
    response.header("Set-Cookie", set_cookie, {.mode_ = ruvia::http_response_header_mode::append});
}

inline void append_expired_session_cookie_header(http_response& response,
    std::pmr::memory_resource* resource, std::string_view cookie_name, bool secure) {
    const cookie_options options{
        .same_site_ = cookie_same_site::lax,
        .max_age_ = std::chrono::seconds(0),
        .http_only_ = cookie_attribute_policy::emit,
        .secure_ = secure ? cookie_attribute_policy::emit : cookie_attribute_policy::omit,
    };
    const set_cookie_plan plan(cookie_name, "", options);
    std::pmr::string set_cookie(resource);
    set_cookie.resize(plan.size());
    plan.write(set_cookie.data());
    response.header("Set-Cookie", set_cookie, {.mode_ = ruvia::http_response_header_mode::append});
}

}  // namespace ruvia::detail
