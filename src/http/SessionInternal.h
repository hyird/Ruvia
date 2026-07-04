#pragma once

#include "ruvia/http/Context.h"

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

}  // namespace ruvia::detail
