#include <utility>

#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_query.h"

#include "db/db_pool.h"
#include "db/db_query_execution.h"
#include "integration/request_io_metrics.h"
#ifdef RUVIA_ENABLE_REDIS
#include "db/db_query_cache_state.h"
#endif

// The per-request database handle: it borrows a registry entry for the
// operation's lifetime and hands each call to the pool.

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/db/db_session.h"

#include "client/client_host_resolver.h"
#include "client/client_lifecycle.h"
#include "db/db_registry.h"
#include "redis/redis_registry.h"

namespace ruvia {

namespace {

task<void> discard_rows_task(task<db_rows> rows_task) {
    (void)co_await std::move(rows_task);
}

}  // namespace

db_session::db_session(detail::db_pool_ref_type client,
    ::ruvia::operation_scope& operation_scope, detail::request_io_metrics* io_metrics,
    detail::db_query_cache_state* cache) noexcept
    : cache_(cache),
      client_(client),
      io_metrics_(io_metrics),
      registration_(operation_scope, this, &db_session::expire_capability) {}

db_session::db_session(const db_session& other) noexcept
    : cache_(other.cache_),
      client_(other.client_),
      io_metrics_(other.io_metrics_),
      options_(other.options_),
      registration_(other.registration_, this) {}

#ifdef RUVIA_ENABLE_REDIS
db_query_result_cache db_session::query_result_cache() const {
    registration_.require_active();
    if (!cache_) {
        throw db_error(db_error::code_type::not_configured, std::nullopt, "database query cache is not configured");
    }
    return db_query_result_cache(*cache_, registration_.scope(), io_metrics_, options_);
}
#endif

db_session db_session::with_options(operation_options options) const {
    registration_.require_active();
    db_session copy(*this);
    copy.options_ = override_operation_options(options_, std::move(options));
    return copy;
}

const worker_ref& db_session::worker() const& {
    registration_.require_active();
    return detail::visit_db_pool(client_, [](const auto& client) -> const worker_ref& {
        return client.worker();
    });
}

void db_session::expire_capability(void* target) noexcept {
    auto& handle = *static_cast<db_session*>(target);
    handle.cache_ = nullptr;
    handle.client_ = detail::db_pool_ref_type{};
    handle.io_metrics_ = nullptr;
    handle.options_ = {};
}

db_driver db_session::query_driver() const {
    registration_.require_active();
    return detail::db_pool_driver(client_);
}

detail::db_query_backend db_session::query_backend() const {
    detail::db_query_backend backend{client_, std::nullopt, io_metrics_};
#ifdef RUVIA_ENABLE_REDIS
    backend.cache_ = cache_;
#endif
    return backend;
}

task<db_rows> db_session::query_task(const db_query& query) const {
    registration_.require_active();
    const auto backend = query_backend();
    auto plan = detail::db_query_plan::prepare(query, nullptr, backend);
#ifdef RUVIA_ENABLE_REDIS
    if (backend.cache_ != nullptr && plan.first_.cache_key_.has_value()) {
        return detail::execute_db_query_plan<false>(std::move(plan), backend, options_);
    }
#endif
    auto options_copy = options_;
    return detail::query_db_backend(
        backend, std::move(plan.first_.sql_), std::move(plan.first_.params_), options_copy);
}

task<std::pair<db_rows, db_rows>> db_session::query_and_count_task(const db_query& query, const db_query& count) const {
    registration_.require_active();
    const auto backend = query_backend();
    auto plan = detail::db_query_plan::prepare(query, &count, backend);
    return detail::execute_db_query_plan<true>(std::move(plan), backend, options_);
}

task<db_exec_result> db_session::execute_query(const db_query& query) const {
    registration_.require_active();
    auto statement = query.compile(query_driver());
    if (statement.returns_rows()) {
        throw std::invalid_argument("execute requires a statement without returned rows");
    }
    return ::ruvia::make_scoped_operation(registration_.scope(),
        detail::visit_db_pool(client_, [&](auto& client) {
            return client.execute(
                std::move(statement.sql_), std::move(statement.params_), options_, io_metrics_);
        }));
}

task<db_exec_result> db_session::execute_prepared(std::string sql) const {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(registration_.scope(),
        detail::visit_db_pool(client_, [&](auto& client) {
            return client.execute(
                std::move(sql), std::vector<db_value>{}, options_, io_metrics_);
        }));
}

task<db_rows> db_session::query(
    std::string_view sql, std::span<const db_value> params) const {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return ::ruvia::make_scoped_operation(registration_.scope(),
        detail::visit_db_pool(client_, [&](auto& client) {
            return client.query(
                std::move(statement.sql_), std::move(statement.params_), options_, io_metrics_);
        }));
}

task<db_exec_result> db_session::execute(
    std::string_view sql, std::span<const db_value> params) const {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return ::ruvia::make_scoped_operation(registration_.scope(),
        detail::visit_db_pool(client_, [&](auto& client) {
            return client.execute(
                std::move(statement.sql_), std::move(statement.params_), options_, io_metrics_);
        }));
}

task<db_stream_result> db_session::query_stream(
    std::string_view sql, std::span<const db_value> params) const {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope, query_stream_prepared(client_, std::move(statement.sql_),
                                                     std::move(statement.params_), scope, io_metrics_, options_));
}

task<db_stream_result> db_session::query_stream_prepared(detail::db_pool_ref_type client, std::string sql,
    std::vector<db_value> params,
    ::ruvia::operation_scope& operation_scope, detail::request_io_metrics* io_metrics, operation_options options) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    auto result_value = co_await detail::visit_db_pool(client, [&](auto& pool) {
        return pool.stream(std::move(sql), std::move(params), std::move(options));
    });
    result_value.bind_operation_scope(operation_scope, io_metrics);
    co_return result_value;
}

task<db_transaction> db_session::begin_transaction(db_transaction_options options) const {
    registration_.require_active();
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        begin_transaction_prepared(query_backend(), scope, options_, std::move(options)));
}

task<db_transaction> db_session::begin_transaction_prepared(detail::db_query_backend backend,
    ::ruvia::operation_scope& operation_scope, operation_options operation_options_value,
    db_transaction_options transaction_options) {
    detail::scoped_request_io_timer timer(backend.io_metrics_, detail::request_io_kind::db);
    auto transaction = co_await detail::visit_db_pool(backend.pool_, [&](auto& pool) {
        return pool.begin_transaction(std::move(operation_options_value), std::move(transaction_options));
    });
    transaction.bind_operation_scope(operation_scope, backend.io_metrics_);
    co_return transaction;
}

task<void> db_session::notify(std::string_view channel, std::string_view payload) const {
    registration_.require_active();
    if (query_driver() != db_driver::postgresql) {
        throw std::invalid_argument("database notify requires PostgreSQL");
    }
    auto statement = prepare_db_notify(channel, payload);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        discard_rows_task(detail::visit_db_pool(client_, [&](auto& client) {
            return client.query(std::move(statement.sql_), std::move(statement.params_), options_, io_metrics_);
        })));
}

db_session db_session::bind_caller_scope(::ruvia::operation_scope& scope,
    detail::request_io_metrics* io_metrics, const stop_token& stop_token_value) && {
    registration_.require_active();
    db_session bound(client_, scope, io_metrics, cache_);
    bound.options_ = override_operation_options(options_, operation_options{.timeout_ = std::nullopt, .stop_token_ = stop_token_value});
    return bound;
}

}  // namespace ruvia

namespace ruvia::detail {

class db_client_state final : public std::enable_shared_from_this<db_client_state> {
public:
    db_client_state(event_loop loop, const db_config& config);
#ifdef RUVIA_ENABLE_REDIS
    db_client_state(event_loop loop, const db_config& config, const redis_session& cache_store,
        const db_cache_config& cache_policy);
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
db_client_state::db_client_state(event_loop loop, const db_config& config)
    : loop_(client_lifecycle<db_client_state>::require_loop(std::move(loop))),
      worker_(loop_.worker()),
      databases_(loop_.io_context(), worker_, config),
      lifecycle_(*this, loop_, worker_) {}

#ifdef RUVIA_ENABLE_REDIS
db_client_state::db_client_state(event_loop loop, const db_config& config,
    const redis_session& cache_store, const db_cache_config& cache_policy)
    : loop_(client_lifecycle<db_client_state>::require_loop(std::move(loop))),
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

db_client::db_client(event_loop loop, const db_config& config)
    : state_(std::make_shared<detail::db_client_state>(std::move(loop), config)) {
    state_->bind_stop();
}

#ifdef RUVIA_ENABLE_REDIS
db_client::db_client(event_loop loop, const db_config& config,
    const redis_session& cache_store, const db_cache_config& cache_policy)
    : state_(std::make_shared<detail::db_client_state>(std::move(loop), config, cache_store, cache_policy)) {
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

#ifdef RUVIA_ENABLE_REDIS
#endif

namespace ruvia {
namespace {

[[nodiscard]] detail::db_pool_ref_type pool_ref(const detail::db_registry::pool_owner_type& owner_value) noexcept {
    return std::visit(
        [](const auto& value) {
            return value == nullptr ? detail::db_pool_ref_type{} : detail::db_pool_ref_type{value.get()};
        },
        owner_value);
}

#ifdef RUVIA_ENABLE_REDIS
[[nodiscard]] const detail::db_config_storage& pool_config(
    const detail::db_registry::pool_owner_type& owner_value) noexcept {
    return std::visit(
        [](const auto& value) noexcept -> const detail::db_config_storage& { return value->config(); }, owner_value);
}
#endif

}  // namespace

detail::db_registry::db_registry(asio::io_context& io_context, const worker_ref& worker_value,
    const db_config& default_config)
    : entries_(),
      alias_index_() {
    alias_index_.build({default_capability_alias});
    entries_.reserve(1);
    add(io_context, worker_value, db_config_storage(default_config));
}

detail::db_registry::db_registry(asio::io_context& io_context, const worker_ref& worker_value,
    std::span<const detail::db_definition> databases, redis_registry* redis)
    : entries_(),
      alias_index_() {
    detail::validate_capability_aliases(
        databases, "database alias must not be empty", "duplicate database alias");
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
            attach_cache(entries_.size() - 1, worker_value, store_value, definition.query_cache_->policy_,
                definition.config_, definition.alias_);
        }
#else
        (void)redis;
#endif
    }
}

#ifdef RUVIA_ENABLE_REDIS
detail::db_registry::db_registry(asio::io_context& io_context, const worker_ref& worker_value,
    const db_config& config,
    const redis_session& redis, const db_cache_config& policy)
    : db_registry(io_context, worker_value, config) {
    attach_cache(0, worker_value, redis, db_cache_config_storage(policy),
        pool_config(entries_[0].pool_), default_capability_alias);
}

void detail::db_registry::attach_cache(std::size_t index, const worker_ref& worker_value,
    const redis_session& redis, const db_cache_config_storage& policy,
    const db_config_storage& config, std::string_view alias) {
    if (redis.worker().id() != worker_value.id()) {
        throw std::invalid_argument("database query cache Redis capability must belong to the same worker");
    }
    auto prefix = db_cache_prefix(policy.key_prefix_, alias, config);
    entries_[index].cache_ = std::make_unique<db_query_cache_state>(redis, policy, std::move(prefix));
}
#endif

detail::db_registry::~db_registry() = default;

detail::db_registry::entry_type::entry_type(pool_owner_type pool) noexcept
    : pool_(std::move(pool)) {}

void detail::db_registry::add(
    asio::io_context& io_context, const worker_ref& worker_value, db_config_storage config) {
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
        co_await visit_db_pool(pool_ref(entry.pool_), [](auto& pool) { return pool.connect(); });
    }
}

void detail::db_registry::close_now() noexcept {
    for (auto& entry : entries_) {
#ifdef RUVIA_ENABLE_REDIS
        if (entry.cache_) {
            entry.cache_->close_now();
        }
#endif
        visit_db_pool_if_present(pool_ref(entry.pool_), [](auto& pool) noexcept { pool.close_now(); });
    }
}

db_session detail::db_registry::session(
    const entry_type& entry, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const {
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
        throw db_error(
            db_error::code_type::not_configured, std::nullopt, "default database is not configured");
    }
    return session(entries_[*default_pool_index], operation_scope, io_metrics);
}

db_session detail::db_registry::get(
    std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const {
    const auto match = alias_index_.find(alias);
    if (match.has_value()) {
        return session(entries_[*match], operation_scope, io_metrics);
    }
    throw db_error(db_error::code_type::not_configured, std::nullopt, "database is not configured");
}

}  // namespace ruvia
