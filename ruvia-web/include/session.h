#pragma once

#ifdef RUVIA_ENABLE_REDIS

#include <chrono>
#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "borrowed_text.h"
#include "http_cookie.h"
#include "middleware.h"
#include "operation_options.h"
#include "task.h"

namespace ruvia::detail {

class context_session_state;
struct session_access;

}  // namespace ruvia::detail

namespace ruvia {

// A request-local capability bound by session_middleware. The handle borrows
// context state and therefore cannot escape the request.
// Changes are persisted automatically before the response head (including a
// stream head or a websocket/CONNECT handshake) is sent. Once that commit
// begins, set(), clear(), and regenerate() throw std::logic_error; payload()
// remains readable.
class session final {
public:
    [[nodiscard]] std::string_view payload() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view payload() const&& = delete;
    [[nodiscard]] bool empty() const noexcept {
        return payload().empty();
    }
    // Every non-empty write replaces an existing session identity at commit.
    // Repeated writes in one request publish one new ID; reads keep the ID.
    void set(std::string_view payload);
    template <std::same_as<std::string> string_type>
    void set(string_type&& payload) {
        set_owned(std::move(payload));
    }
    void clear();
    // Rotate without changing the payload, for an authentication or privilege
    // transition that does not write the session. set() already requests rotation.
    // Replacing an expired or revoked session fails with HTTP 409.
    void regenerate();

private:
    explicit session(detail::context_session_state& state_value) noexcept :
        state_(&state_value) {}

    void set_owned(std::string&& payload);

    detail::context_session_state* state_;
    friend class context;
};

// Server-side session backed by Redis (RUVIA_ENABLE_REDIS). Reads the configured
// cookie only when its ID is exactly 48 lowercase hexadecimal characters, the
// fixed length minted by this middleware; other IDs are treated as absent.
// Loads the blob at key_prefix_ + id (by default sess:<id>) into the context.
// Before the response head is sent, persists changes with the configured TTL
// or deletes the session. A new session mints a random ID in an HttpOnly cookie.
// The blob format is the application's; pair it with JSON if desired.
class redis_executor;

struct session_config final {
    std::string redis_alias_{"default"};
    std::string cookie_name_{"sid"};
    std::string key_prefix_{"sess:"};
    // Must be positive and representable in std::chrono::milliseconds;
    // otherwise construction throws std::invalid_argument.
    std::chrono::seconds ttl_{std::chrono::hours(24)};
    std::string path_{"/"};
    std::string domain_{};
    cookie_same_site same_site_{cookie_same_site::lax};
    // nullopt selects Secure from connection.scheme() == https; an explicit
    // bool forces emit or omit.
    std::optional<bool> secure_{};
    // Optional HMAC secret: when non-empty, signs the session ID cookie and
    // verifies incoming cookies with constant-time HMAC-SHA256 before Redis lookup.
    std::string secret_{};
};

// Standalone Redis session store for custom handlers, WebSocket handshakes, or
// standalone routers that manage session lifecycles outside session_middleware.
class session_store final {
public:
    explicit session_store(const session_config& config = {});

    [[nodiscard]] std::string_view cookie_name() const& noexcept RUVIA_LIFETIMEBOUND {
        return cookie_name_;
    }
    std::string_view cookie_name() const&& = delete;
    [[nodiscard]] std::string_view key_prefix() const& noexcept RUVIA_LIFETIMEBOUND {
        return key_prefix_;
    }
    std::string_view key_prefix() const&& = delete;
    [[nodiscard]] std::chrono::seconds ttl() const noexcept {
        return ttl_;
    }

    // Extracts and (when secret_ is configured) verifies the 48-hex session ID
    // from a raw cookie value; returns nullopt if invalid or tampered.
    [[nodiscard]] std::optional<std::string_view> verify_cookie_id(std::string_view cookie_value RUVIA_LIFETIMEBOUND) const noexcept;
    // Formats the raw or HMAC-signed cookie value for `session_id`.
    [[nodiscard]] std::string format_cookie_value(std::string_view session_id) const;
    [[nodiscard]] cookie_options cookie_attributes(bool connection_secure) const noexcept;
    [[nodiscard]] cookie_options deletion_cookie_attributes(bool connection_secure) const noexcept;

    // Mints a fresh 48-hex session ID, stores `payload` under TTL, and returns the new ID.
    [[nodiscard]] task<std::string> create(const redis_executor& redis, std::string_view payload, operation_options options = {}) const;
    // Loads the payload for a validated 48-hex `session_id`, or nullopt if absent/invalid.
    [[nodiscard]] task<std::optional<std::string>> load(const redis_executor& redis, std::string_view session_id, operation_options options = {}) const;
    // Atomically rotates `old_session_id` -> fresh 48-hex ID with `payload` and TTL;
    // returns nullopt if `old_session_id` already expired or was revoked.
    [[nodiscard]] task<std::optional<std::string>> rotate(const redis_executor& redis, std::string_view old_session_id, std::string_view payload, operation_options options = {}) const;
    [[nodiscard]] task<bool> destroy(const redis_executor& redis, std::string_view session_id, operation_options options = {}) const;

private:
    friend class session_middleware;
    friend struct detail::session_access;

    std::string redis_alias_;
    std::string cookie_name_;
    std::string key_prefix_;
    std::chrono::seconds ttl_;
    std::string path_;
    std::string domain_;
    cookie_same_site same_site_;
    std::optional<bool> secure_;
    std::string secret_;
};

class session_middleware final {
public:
    session_middleware();
    explicit session_middleware(const session_config& config);

    session_middleware(const session_middleware&) = delete;
    session_middleware& operator=(const session_middleware&) = delete;
    session_middleware(session_middleware&&) = delete;
    session_middleware& operator=(session_middleware&&) = delete;

    task<void> invoke(context& c, next& next);

private:
    friend struct detail::session_access;

    session_store store_;
};

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
