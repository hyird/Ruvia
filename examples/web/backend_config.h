#pragma once

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/web/app.h"
#include "ruvia/web/http_client_types.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db.h"
#endif

namespace example {

// Startup-only lookup shared by the examples. Process variables take precedence
// over an optional application::env() loaded from .env beside the executable. A default
// ruvia::env is empty: it does not import the process environment automatically.
class environment final {
public:
    explicit environment(const ruvia::env* dotenv = nullptr) noexcept
        : dotenv_(dotenv) {}

    std::optional<std::string_view> get(std::string_view name) const {
        const std::string key(name);
        if (const auto* value = std::getenv(key.c_str())) {
            return std::string_view(value);
        }
        return dotenv_ ? dotenv_->get(name) : std::nullopt;
    }

    template <typename value_type>
        requires std::is_arithmetic_v<value_type>
    std::optional<value_type> get(std::string_view name) const {
        const auto text = get(name);
        if (!text) {
            return std::nullopt;
        }
        if constexpr (std::is_same_v<value_type, bool>) {
            if (*text == "true" || *text == "1") {
                return true;
            }
            if (*text == "false" || *text == "0") {
                return false;
            }
        } else {
            value_type value{};
            const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
            if (error == std::errc{} && end == text->data() + text->size()) {
                return value;
            }
        }
        throw std::invalid_argument("invalid example setting: " + std::string(name));
    }

    // A file setting; an absent or empty value selects none.
    std::optional<std::filesystem::path> path(std::string_view name) const {
        const auto text = get(name);
        if (!text || text->empty()) {
            return std::nullopt;
        }
        return std::filesystem::path(*text);
    }

private:
    const ruvia::env* dotenv_;
};

}  // namespace example

namespace example {

// Shared startup configuration for SQL and Redis examples. With prefix
// RUVIA_DB (or RUVIA_REDIS), TLS stays verified unless RUVIA_DB_TLS=false is
// explicitly selected for a trusted local server. RUVIA_DB_CA selects a CA;
// RUVIA_DB_CERT/KEY supply a client identity; RUVIA_DB_SERVER_NAME overrides
// the verified name for drivers that support it. An unset CA uses system trust.
inline ruvia::client_tls_config backend_tls(std::string_view prefix, const environment& env = environment{}) {
    const std::string base(prefix);
    ruvia::client_tls_config config{
        .mode_ = env.get<bool>(base + "_TLS").value_or(true)
                     ? ruvia::client_tls_mode::verify_identity
                     : ruvia::client_tls_mode::disabled,
        .ca_file_ = env.path(base + "_CA"),
    };
    const auto certificate = env.path(base + "_CERT");
    const auto key = env.path(base + "_KEY");
    if (certificate || key) {
        // Only one of the two is a configuration error, reported by the client.
        config.identity_ = ruvia::tls_identity_config{
            .certificate_chain_file_ = certificate.value_or(std::filesystem::path{}),
            .private_key_file_ = key.value_or(std::filesystem::path{}),
        };
    }
    if (auto name = env.get(base + "_SERVER_NAME"); name && !name->empty()) {
        config.server_name_.emplace(*name);
    }
    return config;
}

}  // namespace example

namespace example {

inline void assign_setting(std::string& target, const environment& env, std::string_view name) {
    if (const auto value = env.get(name)) {
        target.assign(*value);
    }
}

#ifdef RUVIA_ENABLE_DATABASE
inline ruvia::db_config database_config(const environment& env, ruvia::db_driver driver) {
    ruvia::db_config config{.driver_ = driver};
    assign_setting(config.host_, env, "RUVIA_DB_HOST");
    assign_setting(config.username_, env, "RUVIA_DB_USER");
    assign_setting(config.password_, env, "RUVIA_DB_PASSWORD");
    assign_setting(config.database_, env, "RUVIA_DB_DATABASE");
    config.tls_ = backend_tls("RUVIA_DB", env);
    if (const auto port = env.get<std::uint16_t>("RUVIA_DB_PORT")) {
        config.port_ = *port;
    }
    if (const auto size = env.get<std::size_t>("RUVIA_DB_POOL_SIZE")) {
        config.pool_size_per_worker_ = *size;
    }
    config.connect_timeout_ = std::chrono::seconds(5);
    config.query_timeout_ = std::chrono::seconds(10);
    return config;
}

// Startup bridge on the host thread: migrates on a short-lived loop, whose
// blocking pool looks a DNS RUVIA_DB_HOST up, before the demo or app starts.
inline ruvia::db_migration_report apply_migrations(
    const ruvia::db_config& config, std::span<const ruvia::db_migration> migrations) {
    ruvia::blocking_pool host_lookups({.thread_count_ = 1});
    ruvia::event_loop_pool loops({.loop_count_ = 1, .blocking_pool_ = &host_lookups});
    const ruvia::db_migrator migrator(loops.loop(0), config);
    loops.start();
    auto run = loops.loop(0).start(migrator.migrate(migrations));
    run.wait();
    loops.join();
    return std::move(run).get();
}
#endif

#ifdef RUVIA_ENABLE_REDIS
inline ruvia::redis_config redis_config(const environment& env, std::string_view prefix = "RUVIA_REDIS") {
    const std::string base(prefix);
    ruvia::redis_config config;
    assign_setting(config.host_, env, base + "_HOST");
    assign_setting(config.username_, env, base + "_USER");
    assign_setting(config.password_, env, base + "_PASSWORD");
    config.tls_ = backend_tls(prefix, env);
    if (const auto port = env.get<std::uint16_t>(base + "_PORT")) {
        config.port_ = *port;
    }
    if (const auto database = env.get<std::uint32_t>(base + "_DATABASE")) {
        config.database_ = *database;
    }
    if (const auto size = env.get<std::uint32_t>(base + "_POOL_SIZE_PER_WORKER")) {
        config.pool_size_per_worker_ = *size;
    }
    if (const auto size = env.get<std::uint32_t>(base + "_BLOCKING_POOL_SIZE_PER_WORKER")) {
        config.blocking_pool_size_per_worker_ = *size;
    }
    return config;
}
#endif

}  // namespace example
