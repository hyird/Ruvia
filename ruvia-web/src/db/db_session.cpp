#include <utility>

#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_query.h"

#include "db/db_pool.h"
#include "integration/operation_interrupt.h"
#ifdef RUVIA_ENABLE_REDIS
#endif

// The per-request database handle: it borrows a registry entry for the
// operation's lifetime and hands each call to the pool.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/db/db_repository.h"
#include "ruvia/web/db/db_session.h"

#include "client/client_tcp_transport.h"
#include "db/db_sql_format.h"
#include "redis/redis_pool.h"
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

namespace ruvia::detail {

const db_column_info& require_db_column(std::span<const db_column_info> columns, std::string_view name) {
    const auto found = std::ranges::find(columns, name, &db_column_info::name_);
    if (found == columns.end()) {
        throw std::invalid_argument("unknown database entity column");
    }
    return *found;
}

void validate_insert_columns(std::span<const db_column_info> entity_columns, std::span<const std::string_view> columns) {
    if (columns.empty()) {
        throw std::invalid_argument("insert_from requires target columns");
    }
    for (const auto column : columns) {
        if (!require_db_column(entity_columns, column).writable_) {
            throw std::invalid_argument("insert_from cannot write a generated column");
        }
    }
}

void apply_entity_upsert(db_query& query, std::span<const db_column_info> entity_columns,
    std::span<db_assignment> updates, std::span<const std::string_view> inserted_columns,
    const db_upsert_options& options, db_driver driver, std::string_view table, std::string_view qualifier,
    std::string_view update_date, std::string_view version) {
    if ((options.skip_update_if_no_values_changed_ || !options.index_predicate_.empty() ||
            !options.update_where_.empty()) &&
        driver != db_driver::postgresql) {
        throw std::invalid_argument("conditional repository upsert requires PostgreSQL");
    }
    for (const auto& name : options.conflict_paths_) {
        require_db_column(entity_columns, name);
    }
    for (const auto& name : options.update_columns_) {
        if (!require_db_column(entity_columns, name).writable_) {
            throw std::invalid_argument("repository upsert cannot update a generated column");
        }
    }
    for (std::size_t i = 0; i < options.update_expressions_.size(); ++i) {
        const auto& item = options.update_expressions_[i];
        if (!require_db_column(entity_columns, item.column_).writable_ || item.empty() || options.do_nothing_) {
            throw std::invalid_argument("upsert expression requires a writable column and update action");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (options.update_expressions_[j].column_ == item.column_) {
                throw std::invalid_argument("duplicate upsert expression column");
            }
        }
    }
    if (options.do_nothing_ && !options.update_where_.empty()) {
        throw std::invalid_argument("upsert update condition requires an update action");
    }
    const auto alias = qualifier == table ? std::string_view{} : qualifier;
    const auto target_where = options.index_predicate_.expression(query, table, alias);
    std::size_t update_count = 0;
    db_expression update_where;
    bool do_nothing = options.do_nothing_;
    if (!do_nothing) {
        for (const auto& column : entity_columns) {
            const auto name = column.name_;
            const auto custom = std::ranges::find_if(options.update_expressions_,
                [name](const auto& item) { return item.column_ == name; });
            bool selected = custom != options.update_expressions_.end();
            if (!selected) {
                selected = options.update_columns_.empty()
                               ? column.conflict_updatable_ && std::ranges::find(inserted_columns, name) != inserted_columns.end() &&
                                     std::ranges::find(options.conflict_paths_, borrowed_text(name)) == options.conflict_paths_.end()
                               : column.writable_ && std::ranges::find(options.update_columns_, borrowed_text(name)) != options.update_columns_.end();
            }
            if (!selected) {
                continue;
            }
            const auto value = custom == options.update_expressions_.end()
                                   ? query.excluded(name)
                                   : custom->expression(query, table, alias);
            updates[update_count++] = {name, db_expr(value, {}, true)};
            if (options.skip_update_if_no_values_changed_) {
                const auto changed = query.binary(query.column(name, qualifier), db_binary_operator::is_distinct_from, value);
                update_where = update_where.empty() ? changed : query.binary(update_where, db_binary_operator::or_value, changed);
            }
        }
        if (update_count != 0) {
            const auto assigned_count = update_count;
            const auto assigned = [&](std::string_view name) {
                return std::ranges::any_of(updates.first(assigned_count), [name](const auto& item) { return item.column_ == name; });
            };
            append_lifecycle_assignments(query, qualifier, update_date, version, assigned,
                [&](std::string_view name, db_expression value) { updates[update_count++] = {name, db_expr(value, {}, true)}; });
        } else if (driver == db_driver::postgresql) {
            do_nothing = true;
        }
    }
    if (!options.update_where_.empty()) {
        const auto condition = options.update_where_.expression(query, table, alias);
        update_where = update_where.empty() ? condition : query.binary(update_where, db_binary_operator::and_value, condition);
    }
    query.on_conflict({
        .columns_ = options.conflict_paths_,
        .target_where_ = target_where,
        .update_ = std::span<const db_assignment>(updates.data(), update_count),
        .update_where_ = update_where,
        .do_nothing_ = do_nothing,
        .any_unique_key_ = options.any_unique_key_,
    });
}

std::size_t db_relation_plan::find(std::size_t parent_value, std::size_t relation) const noexcept {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].parent_ == parent_value && nodes_[i].relation_ == relation) {
            return i;
        }
    }
    return root;
}

std::string db_relation_plan::normalize(std::string_view root_alias, std::string_view path) const {
    if (path.size() > root_alias.size() && path.starts_with(root_alias) && path[root_alias.size()] == '.') {
        path.remove_prefix(root_alias.size() + 1);
    } else if (const auto dot = path.find('.'); dot != std::string_view::npos) {
        for (const auto& node : nodes_) {
            if (path.substr(0, dot) == node.alias_) {
                const auto suffix = path.substr(dot);
                std::string result_value;
                result_value.reserve(node.path_.size() + suffix.size());
                result_value.append(node.path_);
                result_value.append(suffix);
                return result_value;
            }
        }
    }
    return std::string(path);
}

bool db_relation_plan::uses_source_name(const db_query& query, std::string_view name) {
    const auto& s = query.storage();
    return (s.source_ && source_binds_name(*s.source_, name)) ||
           std::ranges::any_of(s.joins_, [&](const auto& join) { return source_binds_name(join.source_, name); });
}

bool db_relation_plan::uses_projection_name(const db_query& query, std::string_view name) {
    const auto& s = query.storage();
    return std::ranges::any_of(s.projections_, [&](auto index) {
        const auto& node_value = s.nodes_[index];
        return (node_value.kind_ == db_node_kind::column || node_value.kind_ == db_node_kind::alias) && node_value.text_ == name;
    });
}

std::optional<db_query> db_relation_plan::prepare_entity_read(const db_query& query,
    std::span<const std::string_view> primary_key, std::string_view root_alias, db_driver driver) {
    const auto& s = query.storage();
    if (s.kind_ != db_query_kind::select || primary_key.empty()) {
        throw std::invalid_argument("relation loading requires an entity SELECT with a primary key");
    }
    const bool paginated = s.limit_.has_value() || s.offset_.has_value();
    const bool scope_lock = driver == db_driver::postgresql && s.lock_ && s.lock_->tables_.empty();
    if (!paginated && !scope_lock) {
        return std::nullopt;
    }
    if (paginated) {
        if (query.has_grouping() || !s.set_operations_.empty() || !s.distinct_on_.empty()) {
            throw std::invalid_argument("paged relation loading requires an ungrouped SELECT without set operations or DISTINCT ON");
        }
        if (s.lock_ && s.lock_->skip_locked_) {
            throw std::invalid_argument("paged relation loading cannot combine SKIP LOCKED with collection expansion");
        }
    }
    auto result_value = query.clone();
    if (scope_lock) {
        result_value.storage().lock_->tables_.emplace_back(root_alias);
    }
    if (!paginated) {
        return result_value;
    }

    // Select a page of root identities before filtering the expanded result.
    // Ranking makes joined ordering deterministic for each root, and avoids
    // applying LIMIT to child rows or truncating a loaded collection.
    auto ranked = query.clone();
    auto& rank_storage = ranked.storage();
    // Refer to the outer WITH bindings. Copying their declarations into the
    // page would evaluate volatile CTEs twice and relocate DML CTEs illegally.
    rank_storage.ctes_.clear();
    auto orders = std::move(rank_storage.orders_);
    rank_storage.limit_.reset();
    rank_storage.offset_.reset();
    rank_storage.lock_.reset();
    rank_storage.distinct_ = false;
    for (auto& order : orders) {
        const auto& node_value = rank_storage.nodes_[order.expression_];
        if (node_value.kind_ == db_node_kind::column && node_value.qualifier_.empty()) {
            for (const auto index : rank_storage.projections_) {
                const auto& projection = rank_storage.nodes_[index];
                if (projection.kind_ == db_node_kind::alias && projection.text_ == node_value.text_) {
                    order.expression_ = projection.left_;
                    break;
                }
            }
        }
    }
    rank_storage.projections_.clear();
    auto window = ranked.make_node(db_node_kind::window);
    window.left_ = ranked.require_expression(ranked.call("row_number"));
    window.args_.reserve(primary_key.size());
    std::vector<std::string> key_names;
    key_names.reserve(primary_key.size());
    for (std::size_t i = 0; i < primary_key.size(); ++i) {
        auto& name = key_names.emplace_back();
        name.reserve(32);
        name.append("__ruvia_page_key_");
        append_db_number(name, static_cast<std::uint64_t>(i));
        const auto key = ranked.column(primary_key[i], root_alias);
        window.args_.push_back(ranked.require_expression(key));
        ranked.add_select(ranked.alias(key, name));
    }
    if (orders.empty()) {
        orders.reserve(window.args_.size());
        for (const auto key : window.args_) {
            orders.push_back({key, db_order_direction::asc, db_nulls_order::default_value});
        }
    }
    // Give the outer result the same stable tie order as its page.
    for (const auto key : primary_key) {
        result_value.add_order_by(result_value.column(key, root_alias));
    }
    std::vector<std::string> order_names;
    order_names.reserve(orders.size());
    for (std::size_t i = 0; i < orders.size(); ++i) {
        auto& name = order_names.emplace_back();
        name.reserve(32);
        name.append("__ruvia_page_order_");
        append_db_number(name, static_cast<std::uint64_t>(i));
        ranked.add_select(ranked.alias(db_expression(&rank_storage, orders[i].expression_), name));
    }
    window.orders_ = std::move(orders);
    const auto window_expr = ranked.push_node(std::move(window));
    ranked.add_select(ranked.alias(window_expr, "__ruvia_page_rank"));

    db_query page;
    for (const auto& key : key_names) {
        page.add_select(page.column(key, "__ruvia_ranked"));
    }
    // Read the window ordering before the ranked query is moved into the page.
    const auto& window_orders = rank_storage.nodes_[ranked.require_expression(window_expr)].orders_;
    for (std::size_t i = 0; i < order_names.size(); ++i) {
        page.add_order_by(page.column(order_names[i], "__ruvia_ranked"), window_orders[i].direction_, window_orders[i].nulls_);
    }
    page.from(std::move(ranked), "__ruvia_ranked");
    page.where(page.binary(page.column("__ruvia_page_rank", "__ruvia_ranked"),
        db_binary_operator::equal, page.value(1)));
    // Break ordering ties using the complete primary key, including composite keys.
    for (const auto& key : key_names) {
        page.add_order_by(page.column(key, "__ruvia_ranked"));
    }
    page.limit(s.limit_ ? std::optional<query_row_count>(*s.limit_) : std::nullopt)
        .offset(s.offset_ ? std::optional<query_row_count>(*s.offset_) : std::nullopt);
    db_query included;
    const auto page_alias = root_alias == "__ruvia_page" ? "__ruvia_page_1" : "__ruvia_page";
    included.select(included.value(1)).from(std::move(page), page_alias);
    for (std::size_t i = 0; i < primary_key.size(); ++i) {
        included.and_where(included.binary(included.column(key_names[i], page_alias),
            db_binary_operator::equal, included.column(primary_key[i], root_alias)));
    }
    result_value.limit(std::nullopt).offset(std::nullopt);
    result_value.and_where(result_value.exists(std::move(included)));
    return result_value;
}

std::string db_relation_plan::unique_alias(const db_query& query, std::string_view reserved) {
    constexpr std::string_view prefix = "__ruvia_relation_";
    std::string alias;
    alias.reserve(32);
    alias.append(prefix);
    for (;;) {
        alias.resize(prefix.size());
        append_db_number(alias, static_cast<std::uint64_t>(next_alias_++));
        if (alias != reserved && !uses_source_name(query, alias)) {
            return alias;
        }
    }
}

std::string db_relation_plan::unique_projection(const db_query& query) {
    constexpr std::string_view prefix = "__ruvia_column_";
    std::string name;
    name.reserve(32);
    name.append(prefix);
    for (;;) {
        name.resize(prefix.size());
        append_db_number(name, static_cast<std::uint64_t>(next_projection_++));
        if (!uses_projection_name(query, name)) {
            return name;
        }
    }
}

void db_relation_plan::conjunction(db_query& query, db_expression& predicate, db_expression term) {
    predicate = predicate.empty() ? term : query.binary(predicate, db_binary_operator::and_value, term);
}

db_projection apply_projection(db_query& query, std::span<const db_selection> selection,
    std::string_view qualifier, bool returning, std::string_view root_table, const db_projection* previous) {
    if (selection.empty()) {
        throw std::invalid_argument("projection requires at least one field");
    }
    db_projection result;
    const auto named_count = static_cast<std::size_t>(std::ranges::count_if(selection, [](const auto& field) { return !field.is_wildcard(); }));
    result.columns_.reserve((previous ? previous->columns_.size() : 0) + named_count);
    if (previous) {
        result.columns_ = previous->columns_;
        result.wildcard_ = previous->wildcard_;
    }
    const auto alias = qualifier == root_table ? std::string_view{} : qualifier;
    const auto import_field = [&](const db_selection& field) -> db_expression {
        if (field.is_wildcard()) {
            if (!field.column_.empty()) {
                throw std::invalid_argument("wildcard projections cannot have a result alias");
            }
            result.wildcard_ = true;
            return field.expression(query, root_table, alias);
        }
        if (field.column_.empty() || std::ranges::find(result.columns_, std::string_view(field.column_)) != result.columns_.end()) {
            throw std::invalid_argument("projection requires unique nonempty field names");
        }
        result.columns_.emplace_back(field.column_);
        auto expression = !field.has_expression()
                              ? query.column(field.column_, qualifier)
                              : field.expression(query, root_table, alias);
        return query.alias(expression, field.column_);
    };
    const auto publish = [&](std::span<const db_expression> values) {
        if (returning) {
            query.returning(values);
        } else if (previous) {
            for (const auto value : values) {
                query.add_select(value);
            }
        } else {
            query.select(values);
        }
    };
    import_db_expressions(query, selection, import_field, publish);
    return result;
}

std::string format_truncate_table(std::string_view table, db_driver driver) {
    std::string sql;
    sql.reserve(17U + table.size());
    sql.append("TRUNCATE TABLE ");
    append_db_qualified_identifier(sql, table, driver);
    return sql;
}

}  // namespace ruvia::detail

// An open transaction: it owns a pooled connection until commit, rollback, or
// destruction, and a transaction abandoned by an unwinding scope must roll back
// rather than leak the connection.

namespace ruvia {

db_transaction::lease_type::lease_type(detail::db_pool_ref_type client, std::size_t slot, operation_options options) noexcept
    : client_(client),
      slot_(slot),
      options_(std::move(options)) {}

void db_transaction::lease_type::abort() noexcept {
    detail::visit_db_pool_if_present(client_, [&](auto& client) noexcept { client.abort_transaction(slot_); });
}

db_transaction::db_transaction(detail::db_pool_ref_type client, std::size_t slot, operation_options options)
    : state_(std::make_unique<operation_state_type>(lease_type{client, slot, std::move(options)})) {}

db_transaction::db_transaction(db_transaction&& other) noexcept
    : state_(std::move(other.state_)),
      io_metrics_(other.io_metrics_),
      registration_(std::move(other.registration_), this) {}

db_transaction::~db_transaction() = default;

bool db_transaction::active() const noexcept {
    return state_ != nullptr && state_->active();
}

void db_transaction::bind_operation_scope(
    ::ruvia::operation_scope& scope, detail::request_io_metrics* io_metrics) noexcept {
    io_metrics_ = io_metrics;
    registration_.bind(scope, this, &db_transaction::expire_capability);
}

void db_transaction::expire_capability(void* target) noexcept {
    auto& transaction = *static_cast<db_transaction*>(target);
    transaction.reset();
}

db_driver db_transaction::query_driver() const {
    registration_.require_active();
    return detail::db_pool_driver(state_->preparation_payload().client_);
}

// A transaction never uses the shared query cache: its reads may include its
// own uncommitted writes, which must neither be published nor be replaced by
// rows cached outside the transaction.
detail::db_query_backend db_transaction::query_backend(const lease_type& lease) const {
    return detail::db_query_backend{lease.client_, lease.slot_, io_metrics_};
}

task<db_rows> db_transaction::query_task(const db_query& query) {
    registration_.require_active();
    const auto backend = query_backend(state_->preparation_payload());
    auto plan = detail::db_query_plan::prepare(query, nullptr, backend);
    operation_guard_type operation(*state_);
    return query_plan_prepared<false>(std::move(plan), backend, std::move(operation));
}

task<std::pair<db_rows, db_rows>> db_transaction::query_and_count_task(const db_query& query, const db_query& count) {
    registration_.require_active();
    const auto backend = query_backend(state_->preparation_payload());
    auto plan = detail::db_query_plan::prepare(query, &count, backend);
    operation_guard_type operation(*state_);
    return query_plan_prepared<true>(std::move(plan), backend, std::move(operation));
}

template <bool with_count>
task<detail::db_query_result<with_count>> db_transaction::query_plan_prepared(
    detail::db_query_plan plan, detail::db_query_backend backend, operation_guard_type pending) {
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    bool backend_failed = false;
    backend.backend_failed_ = &backend_failed;
    try {
        auto result_value = co_await detail::execute_db_query_plan<with_count>(std::move(plan), backend,
            lease_value.options_);
        operation.finish_active();
        co_return result_value;
    } catch (...) {
        // SQL failures have already retired the lease in the backend. Deadline
        // failures must retire it here while it is still owned by this operation.
        if (!backend_failed) {
            lease_value.abort();
        }
        throw;
    }
}

template <typename result_type, bool exec_statement>
task<result_type> db_transaction::run_statement(std::string sql, std::vector<db_value> params) {
    operation_guard_type operation(*state_);
    return ::ruvia::make_scoped_operation(registration_.scope(),
        statement_task<result_type, exec_statement>(
            std::move(sql), std::move(params), io_metrics_, std::move(operation)));
}

template <typename result_type, bool exec_statement>
task<result_type> db_transaction::statement_task(std::string sql, std::vector<db_value> params,
    detail::request_io_metrics* io_metrics, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    if constexpr (exec_statement) {
        auto result_value = co_await detail::visit_db_pool(lease_value.client_, [&](auto& client) {
            return client.execute_on_transaction_slot(
                lease_value.slot_, std::move(sql), std::move(params), lease_value.options_);
        });
        operation.finish_active();
        if constexpr (!std::is_void_v<result_type>) {
            co_return result_value;
        }
    } else {
        auto rows = co_await detail::visit_db_pool(lease_value.client_, [&](auto& client) {
            return client.query_on_transaction_slot(
                lease_value.slot_, std::move(sql), std::move(params), lease_value.options_);
        });
        if constexpr (std::is_same_v<result_type, bool>) {
            try {
                if (rows.size() != 1) {
                    throw std::runtime_error("database boolean scalar query did not return one row");
                }
                const bool result = rows.front()[std::size_t{0}].template as<bool>().value();
                operation.finish_active();
                co_return result;
            } catch (...) {
                lease_value.abort();
                throw;
            }
        } else {
            operation.finish_active();
            if constexpr (!std::is_void_v<result_type>) {
                co_return rows;
            }
        }
    }
}

template <bool commit_lease>
task<void> db_transaction::finish_task(detail::request_io_metrics* io_metrics, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    co_await detail::visit_db_pool(lease_value.client_, [&](auto& client) {
        return commit_lease
                   ? client.commit_transaction(lease_value.slot_, lease_value.options_)
                   : client.rollback_transaction(lease_value.slot_, lease_value.options_);
    });
    operation.finish_closed();
}

task<db_exec_result> db_transaction::execute_query(const db_query& query) {
    registration_.require_active();
    const auto& lease_value = state_->preparation_payload();
    auto statement = query.compile(detail::db_pool_driver(lease_value.client_));
    if (statement.returns_rows()) {
        throw std::invalid_argument("execute requires a statement without returned rows");
    }
    return run_statement<db_exec_result, true>(std::move(statement.sql_), std::move(statement.params_));
}

task<db_exec_result> db_transaction::execute_prepared(std::string sql) {
    registration_.require_active();
    return run_statement<db_exec_result, true>(std::move(sql), std::vector<db_value>{});
}

task<db_rows> db_transaction::query(
    std::string_view sql, std::span<const db_value> params) & {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return run_statement<db_rows, false>(std::move(statement.sql_), std::move(statement.params_));
}

task<db_exec_result> db_transaction::execute(
    std::string_view sql, std::span<const db_value> params) & {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return run_statement<db_exec_result, true>(std::move(statement.sql_), std::move(statement.params_));
}

task<void> db_transaction::commit() & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(
        registration_.scope(), finish_task<true>(io_metrics_, operation_guard_type(*state_)));
}

task<void> db_transaction::rollback() & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(
        registration_.scope(), finish_task<false>(io_metrics_, operation_guard_type(*state_)));
}

task<void> db_transaction::savepoint(std::string_view name) & {
    return run_identifier_command("SAVEPOINT ", name);
}

task<void> db_transaction::rollback_to_savepoint(std::string_view name) & {
    return run_identifier_command("ROLLBACK TO SAVEPOINT ", name);
}

task<void> db_transaction::release_savepoint(std::string_view name) & {
    return run_identifier_command("RELEASE SAVEPOINT ", name);
}

task<void> db_transaction::run_identifier_command(std::string_view prefix, std::string_view name) {
    registration_.require_active();
    const auto& lease_value = state_->preparation_payload();
    const auto driver = detail::db_pool_driver(lease_value.client_);
    detail::validate_db_identifier(name, driver);
    std::string sql;
    sql.reserve(prefix.size() + name.size() + 2);
    sql.append(prefix);
    detail::append_db_identifier(sql, name, driver);
    return run_statement<void, true>(std::move(sql), std::vector<db_value>{});
}

void db_transaction::require_postgresql_transaction(std::string_view feature) const {
    if (query_driver() != db_driver::postgresql) {
        throw std::invalid_argument(std::string(feature) + " requires PostgreSQL");
    }
}

task<void> db_transaction::query_void_prepared(std::string_view sql, std::span<const db_value> params) {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return run_statement<void, false>(std::move(statement.sql_), std::move(statement.params_));
}

task<bool> db_transaction::query_bool_prepared(std::string_view sql, std::span<const db_value> params) {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return run_statement<bool, false>(std::move(statement.sql_), std::move(statement.params_));
}

task<void> db_transaction::notify(std::string_view channel, std::string_view payload) & {
    require_postgresql_transaction("database notify");
    auto statement = prepare_db_notify(channel, payload);
    return run_statement<void, false>(std::move(statement.sql_), std::move(statement.params_));
}

task<void> db_transaction::advisory_lock(std::int64_t key) & {
    require_postgresql_transaction("advisory_lock");
    const std::array<db_value, 1> params{db_value(key)};
    return query_void_prepared("SELECT \"pg_advisory_xact_lock\"($1::bigint)", params);
}

task<void> db_transaction::advisory_lock(std::int32_t class_id, std::int32_t object_id) & {
    require_postgresql_transaction("advisory_lock");
    const std::array<db_value, 2> params{db_value(class_id), db_value(object_id)};
    return query_void_prepared("SELECT \"pg_advisory_xact_lock\"($1::integer, $2::integer)", params);
}

task<void> db_transaction::advisory_lock(std::string_view key, std::int64_t seed) & {
    require_postgresql_transaction("advisory_lock");
    const std::array<db_value, 2> params{db_value(key), db_value(seed)};
    return query_void_prepared("SELECT \"pg_advisory_xact_lock\"(\"hashtextextended\"($1::text, $2::bigint))", params);
}

task<bool> db_transaction::try_advisory_lock(std::int64_t key) & {
    require_postgresql_transaction("try_advisory_lock");
    const std::array<db_value, 1> params{db_value(key)};
    return query_bool_prepared("SELECT \"pg_try_advisory_xact_lock\"($1::bigint)", params);
}

task<bool> db_transaction::try_advisory_lock(std::int32_t class_id, std::int32_t object_id) & {
    require_postgresql_transaction("try_advisory_lock");
    const std::array<db_value, 2> params{db_value(class_id), db_value(object_id)};
    return query_bool_prepared("SELECT \"pg_try_advisory_xact_lock\"($1::integer, $2::integer)", params);
}

task<bool> db_transaction::try_advisory_lock(std::string_view key, std::int64_t seed) & {
    require_postgresql_transaction("try_advisory_lock");
    const std::array<db_value, 2> params{db_value(key), db_value(seed)};
    return query_bool_prepared("SELECT \"pg_try_advisory_xact_lock\"(\"hashtextextended\"($1::text, $2::bigint))", params);
}

task<void> db_transaction::declare_cursor(
    std::string_view name, std::string_view sql, std::span<const db_value> params, bool scroll) & {
    require_postgresql_transaction("declare_cursor");
    validate_db_sql_text(sql);
    detail::validate_db_identifier(name, db_driver::postgresql);
    const std::string_view mode = scroll ? " SCROLL CURSOR FOR " : " NO SCROLL CURSOR FOR ";
    std::string command;
    command.reserve(8 + name.size() + 2 + mode.size() + sql.size());
    command.append("DECLARE ");
    detail::append_db_identifier(command, name, db_driver::postgresql);
    command.append(mode);
    command.append(sql);
    auto owned_params = detail::clone_db_values(params);
    return run_statement<void, true>(std::move(command), std::move(owned_params));
}

task<db_rows> db_transaction::fetch_cursor(std::string_view name, std::uint64_t count) & {
    require_postgresql_transaction("fetch_cursor");
    if (count > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        throw std::invalid_argument("cursor fetch count exceeds signed 64-bit range");
    }
    detail::validate_db_identifier(name, db_driver::postgresql);
    std::string sql;
    sql.reserve(14 + 20 + 6 + name.size() + 2);
    sql.append("FETCH FORWARD ");
    detail::append_db_number(sql, count);
    sql.append(" FROM ");
    detail::append_db_identifier(sql, name, db_driver::postgresql);
    return run_statement<db_rows, false>(std::move(sql), std::vector<db_value>{});
}

task<void> db_transaction::close_cursor(std::string_view name) & {
    require_postgresql_transaction("close_cursor");
    return run_identifier_command("CLOSE ", name);
}

void db_transaction::reset() noexcept {
    if (state_ != nullptr) {
        state_->reset();
    }
}

}  // namespace ruvia

#ifdef RUVIA_ENABLE_REDIS
#endif

namespace ruvia {

void validate_db_sql_text(std::string_view sql) {
    if (!detail::has_sql_non_whitespace(sql)) {
        throw std::invalid_argument("SQL must not be empty");
    }
    if (sql.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("SQL must not contain NUL bytes");
    }
}

prepared_db_statement prepare_db_statement(
    std::string_view sql, std::span<const db_value> params) {
    validate_db_sql_text(sql);
    return prepared_db_statement{std::string(sql), detail::clone_db_values(params)};
}

prepared_db_statement prepare_db_notify(
    std::string_view channel, std::string_view payload) {
    if (channel.empty()) {
        throw std::invalid_argument("database notify channel must not be empty");
    }
    const std::array<db_value, 2> params{db_value(channel), db_value(payload)};
    return prepare_db_statement("SELECT \"pg_notify\"($1::text, $2::text)", params);
}

namespace detail {
namespace {

[[noreturn]] void throw_db_plan_timeout(db_driver driver) {
    throw db_error(db_error::code_type::timeout, driver, "database query timed out");
}

}  // namespace

db_query_plan db_query_plan::prepare(const db_query& query, const db_query* count, const db_query_backend& backend) {
    const auto driver = db_pool_driver(backend.pool_);
    const auto prepare_step = [&](const db_query& input) {
        auto statement = input.compile(driver);
        if (!statement.returns_rows()) {
            throw std::invalid_argument(count ? "query and count require statements that return rows"
                                              : "query requires a statement that returns rows");
        }
#ifdef RUVIA_ENABLE_REDIS
        std::optional<std::string> key;
        if (backend.cache_ != nullptr) {
            key = backend.cache_->key(input, statement, driver);
        }
        return db_query_step{std::move(statement.sql_), std::move(statement.params_), std::move(key),
            input.cache_duration()};
#else
        return db_query_step{std::move(statement.sql_), std::move(statement.params_)};
#endif
    };
    db_query_plan plan{prepare_step(query), std::nullopt};
    if (count != nullptr) {
        plan.second_.emplace(prepare_step(*count));
    }
    return plan;
}

task<db_rows> query_db_backend(const db_query_backend& backend, std::string sql,
    std::vector<db_value> params, operation_options& options) {
    return visit_db_pool(backend.pool_, [&](auto& pool) {
        if (backend.slot_.has_value()) {
            return pool.query_on_transaction_slot(
                *backend.slot_, std::move(sql), std::move(params), options);
        }
        return pool.query(std::move(sql), std::move(params), std::move(options), backend.io_metrics_);
    });
}

// One deadline and one execution chain for direct, cached, and mixed sequences.
// A statement without a cache key goes straight to the pool.
template <bool with_count>
task<db_query_result<with_count>> execute_db_query_plan(
    db_query_plan plan, db_query_backend backend, operation_options options) {
    const auto driver = db_pool_driver(backend.pool_);
    const operation_deadline deadline_value(options.timeout_);
    std::array<std::optional<db_rows>, with_count ? 2 : 1> results;
    for (std::size_t index = 0; index < results.size(); ++index) {
        auto& step = index == 0 ? plan.first_ : plan.second_.value();
        if (deadline_value.expired()) {
            throw_db_plan_timeout(driver);
        }
        auto step_options = db_remaining_options(options, deadline_value);
#ifdef RUVIA_ENABLE_REDIS
        if (backend.cache_ != nullptr && step.cache_key_.has_value()) {
            // The cache runs only on pooled backends; a transaction never
            // carries one.
            results[index].emplace(co_await backend.cache_->wrap(step.cache_duration_,
                std::move(*step.cache_key_),
                db_cache_query(backend, std::move(step.sql_), std::move(step.params_)), std::move(step_options),
                deadline_value));
        } else
#endif
        {
            scoped_request_io_timer timer(
                backend.slot_.has_value() ? backend.io_metrics_ : nullptr, request_io_kind::db);
            try {
                results[index].emplace(co_await query_db_backend(
                    backend, std::move(step.sql_), std::move(step.params_), step_options));
            } catch (...) {
                if (backend.backend_failed_ != nullptr) {
                    *backend.backend_failed_ = true;
                }
                throw;
            }
        }
        if (deadline_value.expired()) {
            throw_db_plan_timeout(driver);
        }
    }
    if constexpr (with_count) {
        co_return std::pair{std::move(*results[0]), std::move(*results[1])};
    } else {
        co_return std::move(*results[0]);
    }
}

template task<db_query_result<false>> execute_db_query_plan<false>(
    db_query_plan plan, db_query_backend backend, operation_options options);
template task<db_query_result<true>> execute_db_query_plan<true>(
    db_query_plan plan, db_query_backend backend, operation_options options);

}  // namespace detail
}  // namespace ruvia

// A streaming query result: the rows arrive one at a time from a pooled
// connection the result holds for as long as it is active, and every exit --
// exhaustion, explicit close, failure, or destruction -- must return that
// connection exactly once.

namespace ruvia {
db_stream_result::lease_type::lease_type(detail::db_pool_ref_type client, std::size_t slot, operation_options options) noexcept
    : client_(client),
      slot_(slot),
      options_(std::move(options)) {}

void db_stream_result::lease_type::abort() noexcept {
    detail::visit_db_pool_if_present(client_, [&](auto& client) noexcept { client.abort_stream(slot_); });
}

db_stream_result::state_type::state_type(
    lease_type lease, std::vector<std::string> column_names) noexcept
    : column_names_(std::move(column_names)),
      operation_(std::move(lease)) {}

db_stream_result::db_stream_result(detail::db_pool_ref_type client, std::size_t slot,
    std::vector<std::string> column_names,
    operation_options options)
    : state_(std::make_unique<state_type>(lease_type{client, slot, std::move(options)}, std::move(column_names))) {}

db_stream_result::db_stream_result(db_stream_result&& other) noexcept
    : state_(std::move(other.state_)),
      io_metrics_(other.io_metrics_),
      registration_(std::move(other.registration_), this) {}

db_stream_result::~db_stream_result() = default;

bool db_stream_result::active() const noexcept {
    return state_ != nullptr && state_->operation_.active();
}

std::span<const std::string> db_stream_result::column_names() const& {
    registration_.require_active();
    return state_ != nullptr ? std::span<const std::string>(state_->column_names_)
                             : std::span<const std::string>{};
}

void db_stream_result::bind_operation_scope(
    ::ruvia::operation_scope& scope, detail::request_io_metrics* io_metrics) noexcept {
    io_metrics_ = io_metrics;
    registration_.bind(scope, this, &db_stream_result::expire_capability);
}

void db_stream_result::expire_capability(void* target) noexcept {
    auto& result_value = *static_cast<db_stream_result*>(target);
    result_value.reset();
}

namespace {

// The stream's handle options with this operation's own: its timeout only
// tightens, a supplied token replaces the inherited one. An already-stopped
// token gives the lease back before it reaches the connection, so the stream
// stays usable.
template <typename guard_type>
[[nodiscard]] operation_options admit_stream_operation(guard_type& operation, operation_options operation_value) {
    auto effective = override_operation_options(operation.lease().options_, std::move(operation_value));
    if (effective.stop_token_.stop_requested()) {
        operation.finish_active();
        throw db_error(db_error::code_type::cancelled, std::nullopt, "database stream operation cancelled");
    }
    return effective;
}

}  // namespace

task<std::optional<db_row>> db_stream_result::read(operation_options operation) & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(registration_.scope(),
        read_task(io_metrics_, operation_guard_type(state_->operation_), state_->column_names_, std::move(operation)));
}

task<std::optional<db_row>> db_stream_result::read_task(
    detail::request_io_metrics* io_metrics, operation_guard_type pending,
    std::vector<std::string>& column_names, operation_options operation_value) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto effective = admit_stream_operation(operation, std::move(operation_value));
    auto& lease_value = operation.lease();
    auto row = co_await detail::visit_db_pool(lease_value.client_, [&](auto& client) {
        return client.read_stream_row(lease_value.slot_, lease_value.fields_, column_names, effective);
    });
    if (row) {
        operation.finish_active();
    } else {
        operation.finish_closed();
    }
    co_return row;
}

task<void> db_stream_result::close(operation_options operation) & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(
        registration_.scope(), close_task(io_metrics_, operation_guard_type(state_->operation_), std::move(operation)));
}

task<void> db_stream_result::close_task(
    detail::request_io_metrics* io_metrics, operation_guard_type pending, operation_options operation_value) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    if (operation.finished()) {
        co_return;
    }
    operation.start();
    auto effective = admit_stream_operation(operation, std::move(operation_value));
    auto& lease_value = operation.lease();
    co_await detail::visit_db_pool(
        lease_value.client_, [&](auto& client) { return client.close_stream(lease_value.slot_, effective); });
    operation.finish_closed();
}

void db_stream_result::reset() noexcept {
    if (state_ != nullptr) {
        state_->operation_.reset();
    }
}

}  // namespace ruvia
