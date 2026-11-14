#include <cstddef>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "client_lifecycle.h"
#include "db_config_storage.h"
#include "db_pool.h"
#include "db_registry.h"
#include "db_session.h"
#include "event_loop.h"
#include "http_client_pool.h"
#include "redis_pool.h"
#include "startup_config.h"
#ifdef RUVIA_ENABLE_REDIS
#include "db_query_cache.h"
#include "redis_session.h"
#endif

namespace ruvia::detail {

class db_client_state final : public std::enable_shared_from_this<db_client_state> {
public:
    db_client_state(event_loop loop, const db_config& config);
#ifdef RUVIA_ENABLE_REDIS
    db_client_state(event_loop loop, const db_config& config, const redis_session& cache_store, const db_cache_config& cache_policy);
#endif

    db_client_state(const db_client_state&) = delete;
    db_client_state& operator=(const db_client_state&) = delete;

    void bind_stop() {
        lifecycle_.bind_stop();
    }
    [[nodiscard]] task<void> connect() {
        return lifecycle_.connect();
    }
    void request_close() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] task<void> shutdown() {
        return lifecycle_.shutdown();
    }
    [[nodiscard]] db_session db();

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<db_client_state>;
    static constexpr std::string_view client_name{"database"};
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] db_registry& backend() noexcept {
        return databases_;
    }

    event_loop loop_;
    worker_ref worker_;
    db_registry databases_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<db_client_state> lifecycle_;
};
db_client_state::db_client_state(event_loop loop, const db_config& config) :
    loop_(client_lifecycle<db_client_state>::require_loop(std::move(loop))),
    worker_(loop_.worker()),
    databases_(loop_.io_context(), worker_, config),
    lifecycle_(*this, loop_, worker_) {}

#ifdef RUVIA_ENABLE_REDIS
db_client_state::db_client_state(event_loop loop, const db_config& config, const redis_session& cache_store, const db_cache_config& cache_policy) :
    loop_(client_lifecycle<db_client_state>::require_loop(std::move(loop))),
    worker_(loop_.worker()),
    databases_(loop_.io_context(), worker_, config, cache_store, cache_policy),
    lifecycle_(*this, loop_, worker_) {}
#endif

void db_client_state::throw_not_ready() {
    throw std::logic_error("database client is not connected");
}

db_session db_client_state::db() {
    lifecycle_.require_ready();
    return databases_.get(lifecycle_.operation_scope(), nullptr);
}

}  // namespace ruvia::detail

namespace ruvia {

db_client::db_client(event_loop loop, const db_config& config) :
    state_(std::make_shared<detail::db_client_state>(std::move(loop), config)) {
    state_->bind_stop();
}

#ifdef RUVIA_ENABLE_REDIS
db_client::db_client(event_loop loop, const db_config& config, const redis_session& cache_store, const db_cache_config& cache_policy) :
    state_(std::make_shared<detail::db_client_state>(std::move(loop), config, cache_store, cache_policy)) {
    state_->bind_stop();
}
#endif

db_client::~db_client() {
    state_->request_close();
}

task<void> db_client::connect() & {
    return state_->connect();
}

db_session db_client::db() const& {
    return state_->db();
}

void db_client::close() noexcept {
    state_->request_close();
}

task<void> db_client::shutdown() & {
    return state_->shutdown();
}

const worker_ref& db_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] detail::db_pool_ref_type pool_ref(const detail::db_registry::pool_owner_type& owner_value) noexcept {
    return std::visit([](const auto& value) {
        return value == nullptr ? detail::db_pool_ref_type{} : detail::db_pool_ref_type{value.get()};
    }, owner_value);
}

#ifdef RUVIA_ENABLE_REDIS
[[nodiscard]] const detail::db_config_storage& pool_config(const detail::db_registry::pool_owner_type& owner_value) noexcept {
    return std::visit([](const auto& value) noexcept -> const detail::db_config_storage& {
        return value->config();
    }, owner_value);
}
#endif

}  // namespace

detail::db_registry::db_registry(asio::io_context& io_context, const worker_ref& worker_value, const db_config& default_config) :
    entries_(),
    alias_index_() {
    alias_index_.build({default_capability_alias});
    entries_.reserve(1);
    add(io_context, worker_value, db_config_storage(default_config));
}

detail::db_registry::db_registry(asio::io_context& io_context, const worker_ref& worker_value, std::span<const detail::db_definition> databases, redis_registry* redis) :
    entries_(),
    alias_index_() {
    detail::validate_capability_aliases(databases, "database alias must not be empty", "duplicate database alias");
    alias_index_.build(databases);
    entries_.reserve(databases.size());
    for (const auto& definition : databases) {
        add(io_context, worker_value, db_config_storage(definition.config_));
#ifdef RUVIA_ENABLE_REDIS
        if (definition.query_cache_) {
            if (redis == nullptr) {
                throw std::invalid_argument("database query cache requires an existing Redis registry");
            }
            const auto store_value = redis->get(definition.query_cache_->redis_alias_, cache_scope_, nullptr);
            attach_cache(entries_.size() - 1, worker_value, store_value, definition.query_cache_->policy_, definition.config_, definition.alias_);
        }
#else
        (void)redis;
#endif
    }
}

#ifdef RUVIA_ENABLE_REDIS
detail::db_registry::db_registry(asio::io_context& io_context, const worker_ref& worker_value, const db_config& config, const redis_session& redis, const db_cache_config& policy) :
    db_registry(io_context, worker_value, config) {
    attach_cache(0, worker_value, redis, db_cache_config_storage(policy), pool_config(entries_[0].pool_), default_capability_alias);
}

void detail::db_registry::attach_cache(std::size_t index, const worker_ref& worker_value, const redis_session& redis, const db_cache_config_storage& policy, const db_config_storage& config, std::string_view alias) {
    if (redis.worker().id() != worker_value.id()) {
        throw std::invalid_argument("database query cache Redis capability must belong to the same worker");
    }
    auto prefix = db_cache_prefix(policy.key_prefix_, alias, config);
    entries_[index].cache_ = std::make_unique<db_query_cache_state>(redis, policy, std::move(prefix));
}
#endif

detail::db_registry::~db_registry() = default;

detail::db_registry::entry_type::entry_type(pool_owner_type pool) noexcept :
    pool_(std::move(pool)) {}

void detail::db_registry::add(asio::io_context& io_context, const worker_ref& worker_value, db_config_storage config) {
    require_client_host_lookup(config.host_, worker_value);
    pool_owner_type owner;
    switch (config.driver_) {
        case db_driver::unspecified:
            std::terminate();
        case db_driver::mariadb:
#ifdef RUVIA_ENABLE_MARIADB
            owner = std::make_unique<db_pool<mariadb_driver>>(io_context, worker_value, std::move(config));
            break;
#else
            std::terminate();
#endif
        case db_driver::postgresql:
#ifdef RUVIA_ENABLE_POSTGRESQL
            owner = std::make_unique<db_pool<postgresql_driver>>(io_context, worker_value, std::move(config));
            break;
#else
            std::terminate();
#endif
    }

    entries_.emplace_back(std::move(owner));
}

task<void> detail::db_registry::connect() {
    for (auto& entry : entries_) {
        co_await visit_db_pool(pool_ref(entry.pool_), [](auto& pool) {
            return pool.connect();
        });
    }
}

void detail::db_registry::close_now() noexcept {
    for (auto& entry : entries_) {
#ifdef RUVIA_ENABLE_REDIS
        if (entry.cache_) {
            entry.cache_->close_now();
        }
#endif
        visit_db_pool_if_present(pool_ref(entry.pool_), [](auto& pool) noexcept {
            pool.close_now();
        });
    }
}

db_session detail::db_registry::session(const entry_type& entry, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const {
#ifdef RUVIA_ENABLE_REDIS
    detail::db_query_cache_state* const cache = entry.cache_.get();
#else
    detail::db_query_cache_state* const cache = nullptr;
#endif
    return db_session(pool_ref(entry.pool_), operation_scope, io_metrics, cache);
}

bool detail::db_registry::empty() const noexcept {
    return entries_.empty();
}

db_session detail::db_registry::get(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const {
    const auto default_pool_index = alias_index_.default_index();
    if (!default_pool_index.has_value()) {
        throw db_error(db_error::code_type::not_configured, std::nullopt, "default database is not configured");
    }
    return session(entries_[*default_pool_index], operation_scope, io_metrics);
}

db_session detail::db_registry::get(std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const {
    const auto match = alias_index_.find(alias);
    if (match.has_value()) {
        return session(entries_[*match], operation_scope, io_metrics);
    }
    throw db_error(db_error::code_type::not_configured, std::nullopt, "database is not configured");
}

}  // namespace ruvia
