#pragma once

#include <chrono>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace ruvia::detail {

class context_session_state;
struct session_access;

}  // namespace ruvia::detail

namespace ruvia {

class context;
class next;

// A request-local capability bound by session_middleware. The handle borrows
// context state and therefore cannot escape the request.
// Changes are persisted automatically before the response head (including a
// stream or websocket handshake) is sent. Once that commit begins, set(),
// clear(), and regenerate() throw std::logic_error; data() remains readable.
class session final {
public:
    [[nodiscard]] std::string_view data() const& noexcept;
    std::string_view data() const&& = delete;
    // Every non-empty write replaces an existing session identity at commit.
    // Repeated writes in one request publish one new ID; reads keep the ID.
    void set(std::string_view data);
    void clear();
    // Rotate without changing the data, for an authentication or privilege
    // transition that does not write the session. set() already requests rotation.
    // Replacing an expired or revoked session fails with HTTP 409.
    void regenerate();

private:
    explicit session(detail::context_session_state& state_value) noexcept
        : state_(&state_value) {}

    detail::context_session_state* state_;
    friend class context;
};

}  // namespace ruvia

#ifdef RUVIA_ENABLE_REDIS

#include "ruvia/core/task.h"
#include "ruvia/web/detail/middleware/middleware_registration.h"
#include "ruvia/web/middleware.h"

namespace ruvia {

// Server-side session backed by Redis (RUVIA_ENABLE_REDIS). Reads the `sid`
// cookie and loads the blob at sess:<id> into the context. Before the response
// head is sent, it persists changes with the configured TTL or deletes the
// session. A new session mints a random id in an HttpOnly
// cookie. The blob format is the application's; pair it with JSON if desired.
struct session_config final {
    std::string redis_alias_{"default"};
    std::string cookie_name_{"sid"};
    std::string key_prefix_{"sess:"};
    std::chrono::seconds ttl_{std::chrono::hours(24)};
};

class session_middleware final : public middleware {
public:
    session_middleware();
    explicit session_middleware(const session_config& config);

    session_middleware(const session_middleware&) = delete;
    session_middleware& operator=(const session_middleware&) = delete;
    session_middleware(session_middleware&&) = delete;
    session_middleware& operator=(session_middleware&&) = delete;

    task<void> handle(context& c, next& next);

private:
    friend struct detail::session_access;
    task<void> commit(context& c) const;

    struct config_storage_type final {
        config_storage_type(const session_config& source_value, std::pmr::memory_resource* resource);

        std::pmr::string redis_alias_;
        std::pmr::string cookie_name_;
        std::pmr::string key_prefix_;
        std::chrono::seconds ttl_;

    private:
        struct validated_config_type final {
            const session_config* source_;
        };

        [[nodiscard]] static validated_config_type validate(const session_config& source);
        config_storage_type(validated_config_type validated, std::pmr::memory_resource* resource);
    };

    config_storage_type config_;
};

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
