#include <utility>

#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_query.h"

#include "db_pool.h"
#include "operation_interrupt.h"
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
#include <variant>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/db/db_session.h"

#include "client_tcp_transport.h"
#include "db_pipeline.h"
#include "redis_pool.h"

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

namespace ruvia {
namespace {
using detail::append_db_identifier;
using detail::append_db_qualified_identifier;
using detail::append_db_string_literal;
std::string_view action(db_referential_action a) {
    switch (a) {
        case db_referential_action::no_action:
            return "NO ACTION";
        case db_referential_action::restrict:
            return "RESTRICT";
        case db_referential_action::cascade:
            return "CASCADE";
        case db_referential_action::set_null:
            return "SET NULL";
        case db_referential_action::set_default:
            return "SET DEFAULT";
    }
    throw std::invalid_argument("unknown referential action");
}
std::string_view method(db_index_method m) {
    switch (m) {
        case db_index_method::btree:
            return "btree";
        case db_index_method::hash:
            return "hash";
        case db_index_method::gin:
            return "gin";
        case db_index_method::gist:
            return "gist";
        case db_index_method::sp_gist:
            return "spgist";
        case db_index_method::brin:
            return "brin";
    }
    throw std::invalid_argument("unknown index method");
}
std::string_view function_language_name(db_function_language language) {
    switch (language) {
        case db_function_language::plpgsql:
            return "plpgsql";
        case db_function_language::sql:
            return "sql";
    }
    throw std::invalid_argument("unknown function language");
}
std::string_view function_volatility_clause(db_function_volatility volatility) {
    switch (volatility) {
        case db_function_volatility::volatile_value:
            return {};
        case db_function_volatility::stable:
            return " STABLE";
        case db_function_volatility::immutable:
            return " IMMUTABLE";
    }
    throw std::invalid_argument("unknown function volatility");
}
std::string_view trigger_timing_clause(db_trigger_timing timing) {
    switch (timing) {
        case db_trigger_timing::before:
            return " BEFORE ";
        case db_trigger_timing::after:
            return " AFTER ";
        case db_trigger_timing::instead_of:
            return " INSTEAD OF ";
    }
    throw std::invalid_argument("unknown trigger timing");
}
std::string_view trigger_event_name(db_trigger_event event) {
    switch (event) {
        case db_trigger_event::insert:
            return "INSERT";
        case db_trigger_event::update:
            return "UPDATE";
        case db_trigger_event::delete_value:
            return "DELETE";
        case db_trigger_event::truncate:
            return "TRUNCATE";
    }
    throw std::invalid_argument("unknown trigger event");
}
std::string begin_alter_table(
    db_driver driver, std::string_view table_value, std::size_t extra_reserve) {
    std::string sql;
    sql.reserve(extra_reserve + table_value.size());
    sql += "ALTER TABLE ";
    append_db_qualified_identifier(sql, table_value, driver);
    return sql;
}

std::string begin_alter_column(db_driver driver,
    std::string_view table_value, std::string_view column_value, std::string_view clause, std::size_t extra_reserve) {
    auto sql = begin_alter_table(driver, table_value, extra_reserve + column_value.size());
    sql += clause;
    append_db_identifier(sql, column_value, driver);
    return sql;
}

std::string begin_timescale_perform(db_driver driver,
    std::string_view function, std::string_view table_value, std::size_t extra_reserve) {
    std::string name;
    name.reserve(table_value.size() + 4U);
    append_db_qualified_identifier(name, table_value, driver);
    std::string sql;
    sql.reserve(extra_reserve + name.size());
    sql += "PERFORM \"";
    sql += function;
    sql += "\"(";
    append_db_string_literal(sql, name, driver);
    return sql;
}

std::string make_drop_object_sql(db_driver driver,
    std::string_view kind, std::string_view name, bool qualified, bool if_exists) {
    std::string sql;
    sql.reserve(48U + name.size());
    sql += "DROP ";
    sql += kind;
    sql += ' ';
    if (if_exists) {
        sql += "IF EXISTS ";
    }
    if (qualified) {
        append_db_qualified_identifier(sql, name, driver);
    } else {
        append_db_identifier(sql, name, driver);
    }
    return sql;
}

std::string make_add_timescale_policy_sql(db_driver driver,
    std::string_view function, std::string_view table_value, std::string_view param_name, db_expression after,
    bool if_not_exists) {
    if (after.empty()) {
        throw std::invalid_argument("schema expression is required");
    }
    auto sql = begin_timescale_perform(driver, function, table_value, 96U);
    sql += ", \"";
    sql += param_name;
    sql += "\" => ";
    detail::append_db_expression(sql, after, driver);
    sql += ", \"if_not_exists\" => ";
    sql += if_not_exists ? "TRUE" : "FALSE";
    sql += ')';
    return sql;
}

std::string make_remove_timescale_policy_sql(db_driver driver,
    std::string_view function, std::string_view table_value, bool if_exists) {
    auto sql = begin_timescale_perform(driver, function, table_value, 72U);
    sql += ", \"if_exists\" => ";
    sql += if_exists ? "TRUE" : "FALSE";
    sql += ')';
    return sql;
}
}  // namespace

db_schema::db_schema(db_schema_options options)
    : driver_(options.driver_) {
    detail::require_db_dialect(driver_);
}
void db_schema::append(std::string sql, db_migration_atomicity a, bool procedural) {
    statements_.push_back(statement_type{std::move(sql), a, procedural});
}
void db_schema::require_postgresql() const {
    if (driver_ != db_driver::postgresql) {
        throw std::invalid_argument("schema operation requires PostgreSQL");
    }
}
// MariaDB accepts RESTRICT and CASCADE on drops only for porting and ignores
// them, so a requested CASCADE would silently leave dependents in place.
void db_schema::append_drop_behavior(std::string& s, db_drop_behavior b) const {
    switch (b) {
        case db_drop_behavior::restrict:
            return;
        case db_drop_behavior::cascade:
            if (driver_ == db_driver::mariadb) {
                throw std::invalid_argument("MariaDB drops do not support CASCADE");
            }
            s += " CASCADE";
            return;
    }
    throw std::invalid_argument("unknown drop behavior");
}
// Entity constraints follow PostgreSQL's default <table>_<name>_<suffix> naming
// on the unqualified table name.
std::string db_schema::constraint_name(std::string_view table, std::string_view relation, std::string_view suffix) const {
    const auto local = detail::unqualified_db_name(table);
    std::string name;
    name.reserve(local.size() + relation.size() + suffix.size() + 2U);
    name.append(local);
    if (!relation.empty()) {
        name.push_back('_');
        name.append(relation);
    }
    name.push_back('_');
    name.append(suffix);
    return name;
}
void db_schema::append_expression(std::string& s, db_expression e) const {
    if (e.empty()) {
        throw std::invalid_argument("schema expression is required");
    }
    detail::append_db_expression(s, e, driver_);
}
void db_schema::append_type(std::string& s, const db_type_definition& t) const {
    detail::append_db_type_name(s, t.data_type_, t.custom_name_, t.length_, t.precision_, t.scale_,
        t.array_, driver_);
}
void db_schema::append_column(std::string& s, const db_schema_column& c) const {
    if (c.generated_type_ != db_generated_type::none && c.generated_type_ != db_generated_type::stored &&
        c.generated_type_ != db_generated_type::virtual_value) {
        throw std::invalid_argument("unsupported generated column storage mode");
    }
    if (c.name_.empty()) {
        throw std::invalid_argument("column name is required");
    }
    if (c.generated_type_ == db_generated_type::none && !c.as_expression_.empty()) {
        throw std::invalid_argument("generated column expression requires generated metadata");
    }
    if (c.generated_type_ != db_generated_type::none && (c.identity_ != db_identity::none || !c.default_value_.empty())) {
        throw std::invalid_argument("generated column conflicts with identity or default");
    }
    append_db_identifier(s, c.name_, driver_);
    s += ' ';
    append_type(s, c.type_);
    if (c.generated_type_ != db_generated_type::none) {
        if (c.as_expression_.empty()) {
            throw std::invalid_argument("generated column expression is required");
        }
        if (driver_ == db_driver::postgresql && c.generated_type_ == db_generated_type::virtual_value) {
            throw std::invalid_argument("PostgreSQL schema compilation supports stored generated columns only");
        }
        if (driver_ == db_driver::mariadb && (!c.nullable_ || c.primary_key_)) {
            throw std::invalid_argument("MariaDB generated columns cannot declare NOT NULL or PRIMARY KEY");
        }
        s += " GENERATED ALWAYS AS (";
        append_expression(s, c.as_expression_);
        s += ")";
        if (driver_ == db_driver::postgresql || c.generated_type_ == db_generated_type::stored) {
            s += " STORED";
        } else {
            s += " VIRTUAL";
        }
    }
    if (c.identity_ != db_identity::none) {
        if (!c.default_value_.empty() || c.nullable_ || c.type_.array_ ||
            (c.type_.data_type_ != db_data_type::small_int && c.type_.data_type_ != db_data_type::integer && c.type_.data_type_ != db_data_type::big_int)) {
            throw std::invalid_argument("identity requires a non-null integer column without a default");
        }
        if (driver_ == db_driver::postgresql) {
            s += " GENERATED ";
            s += (c.identity_ == db_identity::always ? "ALWAYS" : "BY DEFAULT");
            s += " AS IDENTITY";
        } else {
            if (c.identity_ == db_identity::always) {
                throw std::invalid_argument("MariaDB cannot enforce GENERATED ALWAYS identity semantics");
            }
            s += " AUTO_INCREMENT";
        }
    }
    if (!c.nullable_) {
        s += " NOT NULL";
    }
    if (c.unique_) {
        s += " UNIQUE";
    }
    if (c.primary_key_) {
        s += " PRIMARY KEY";
    }
    if (!c.default_value_.empty()) {
        s += " DEFAULT ";
        append_expression(s, c.default_value_);
    }
}
void db_schema::append_constraint(std::string& s, const db_schema_constraint& c) const {
    if ((c.deferrable_ || c.initially_deferred_ || c.not_valid_) && driver_ != db_driver::postgresql) {
        throw std::invalid_argument("deferred and NOT VALID constraints require PostgreSQL");
    }
    if (c.initially_deferred_ && !c.deferrable_) {
        throw std::invalid_argument("INITIALLY DEFERRED requires DEFERRABLE");
    }
    if (c.kind_ == db_constraint_kind::check && c.deferrable_) {
        throw std::invalid_argument("CHECK constraints cannot be deferred");
    }
    if (c.not_valid_ && c.kind_ != db_constraint_kind::check && c.kind_ != db_constraint_kind::foreign_key) {
        throw std::invalid_argument("NOT VALID requires a CHECK or foreign key constraint");
    }
    if (c.kind_ != db_constraint_kind::check && c.columns_.empty()) {
        throw std::invalid_argument("constraint requires columns");
    }
    if (c.kind_ == db_constraint_kind::foreign_key && c.columns_.size() != c.referenced_columns_.size()) {
        throw std::invalid_argument("foreign key column counts differ");
    }
    if (c.name_.empty()) {
        throw std::invalid_argument("constraint name is required");
    }
    append_db_identifier(s, c.name_, driver_);
    s += ' ';
    if (c.kind_ == db_constraint_kind::check) {
        s += "CHECK (";
        append_expression(s, c.check_);
        s += ')';
    } else if (c.kind_ == db_constraint_kind::primary_key || c.kind_ == db_constraint_kind::unique) {
        s += (c.kind_ == db_constraint_kind::primary_key ? "PRIMARY KEY (" : "UNIQUE (");
        for (size_t i = 0; i < c.columns_.size(); ++i) {
            if (i) {
                s += ',';
            }
            append_db_identifier(s, c.columns_[i], driver_);
        }
        s += ')';
    } else {
        if (c.columns_.empty() || c.referenced_table_.empty() || c.referenced_columns_.empty()) {
            throw std::invalid_argument("foreign key fields are required");
        }
        s += "FOREIGN KEY (";
        for (size_t i = 0; i < c.columns_.size(); ++i) {
            if (i) {
                s += ',';
            }
            append_db_identifier(s, c.columns_[i], driver_);
        }
        s += ") REFERENCES ";
        append_db_qualified_identifier(s, c.referenced_table_, driver_);
        s += " (";
        for (size_t i = 0; i < c.referenced_columns_.size(); ++i) {
            if (i) {
                s += ',';
            }
            append_db_identifier(s, c.referenced_columns_[i], driver_);
        }
        s += ") ON DELETE ";
        s += action(c.on_delete_);
        s += " ON UPDATE ";
        s += action(c.on_update_);
    }
    if (c.deferrable_) {
        s += " DEFERRABLE";
    }
    if (c.initially_deferred_) {
        s += " INITIALLY DEFERRED";
    }
    if (c.not_valid_) {
        s += " NOT VALID";
    }
}

void db_schema::create_schema(std::string_view n, bool ine) {
    std::string s;
    s.reserve(32U + n.size());
    s += "CREATE SCHEMA ";
    if (ine) {
        s += "IF NOT EXISTS ";
    }
    append_db_identifier(s, n, driver_);
    append(std::move(s));
}
void db_schema::drop_schema(std::string_view n, db_drop_behavior b, bool ie) {
    auto s = make_drop_object_sql(driver_, "SCHEMA", n, false, ie);
    append_drop_behavior(s, b);
    append(std::move(s));
}
void db_schema::create_table(const db_table_definition& t) {
    if (t.name_.empty() || t.columns_.empty()) {
        throw std::invalid_argument("table and columns are required");
    }
    if (t.temporary_ && t.unlogged_) {
        throw std::invalid_argument("table cannot be both temporary and unlogged");
    }
    if (t.unlogged_) {
        require_postgresql();
    }
    std::size_t primary_columns = 0;
    for (std::size_t i = 0; i < t.columns_.size(); ++i) {
        primary_columns += t.columns_[i].primary_key_ ? 1 : 0;
        for (std::size_t j = 0; j < i; ++j) {
            if (t.columns_[i].name_ == t.columns_[j].name_) {
                throw std::invalid_argument("duplicate table column");
            }
        }
    }
    if (primary_columns > 1) {
        throw std::invalid_argument("composite primary keys require a table constraint");
    }
    for (const auto& constraint : t.constraints_) {
        if (constraint.not_valid_) {
            throw std::invalid_argument("NOT VALID is only supported when adding a constraint");
        }
        if (constraint.kind_ == db_constraint_kind::primary_key && primary_columns++ != 0) {
            throw std::invalid_argument("duplicate table primary key");
        }
        if (driver_ == db_driver::mariadb && constraint.kind_ == db_constraint_kind::primary_key) {
            for (const auto& column : t.columns_) {
                if (column.generated_type_ != db_generated_type::none &&
                    std::ranges::find(constraint.columns_, column.name_) != constraint.columns_.end()) {
                    throw std::invalid_argument("MariaDB generated columns cannot be primary keys");
                }
            }
        }
    }
    std::string s;
    s.reserve(32U + t.name_.size() + t.columns_.size() * 32U + t.constraints_.size() * 48U);
    s += "CREATE ";
    if (t.temporary_) {
        s += "TEMPORARY ";
    }
    if (t.unlogged_) {
        s += "UNLOGGED ";
    }
    s += "TABLE ";
    if (t.if_not_exists_) {
        s += "IF NOT EXISTS ";
    }
    append_db_qualified_identifier(s, t.name_, driver_);
    s += " (";
    for (size_t i = 0; i < t.columns_.size(); ++i) {
        if (i) {
            s += ", ";
        }
        append_column(s, t.columns_[i]);
    }
    for (auto& c : t.constraints_) {
        s += ", CONSTRAINT ";
        append_constraint(s, c);
    }
    s += ')';
    append(std::move(s));
}
void db_schema::drop_table(std::string_view t, db_drop_behavior b, bool ie) {
    auto s = make_drop_object_sql(driver_, "TABLE", t, true, ie);
    append_drop_behavior(s, b);
    append(std::move(s));
}
void db_schema::rename_table(std::string_view t, std::string_view n) {
    auto s = begin_alter_table(driver_, t, 32U + t.size() + n.size());
    s += " RENAME TO ";
    if (driver_ == db_driver::mariadb) {
        if (const auto dot = t.rfind('.'); dot != std::string_view::npos) {
            append_db_qualified_identifier(s, t.substr(0, dot), driver_);
            s += '.';
        }
    }
    append_db_identifier(s, n, driver_);
    append(std::move(s));
}
void db_schema::add_column(std::string_view t, const db_schema_column& c, bool ie) {
    auto s = begin_alter_table(driver_, t, 64U + c.name_.size());
    s += " ADD COLUMN ";
    if (ie) {
        s += "IF NOT EXISTS ";
    }
    append_column(s, c);
    append(std::move(s));
}
void db_schema::drop_column(std::string_view t, std::string_view c, db_drop_behavior b, bool ie) {
    auto s = begin_alter_table(driver_, t, 48U + c.size());
    s += " DROP COLUMN ";
    if (ie) {
        s += "IF EXISTS ";
    }
    append_db_identifier(s, c, driver_);
    append_drop_behavior(s, b);
    append(std::move(s));
}
void db_schema::rename_column(std::string_view t, std::string_view c, std::string_view n) {
    auto s = begin_alter_column(driver_, t, c, " RENAME COLUMN ", 40U + n.size());
    s += " TO ";
    append_db_identifier(s, n, driver_);
    append(std::move(s));
}
void db_schema::alter_column_type(std::string_view t, std::string_view c, const db_type_definition& ty, db_expression u) {
    require_postgresql();
    auto s = begin_alter_column(driver_, t, c, " ALTER COLUMN ", 64U);
    s += " TYPE ";
    append_type(s, ty);
    if (!u.empty()) {
        s += " USING ";
        append_expression(s, u);
    }
    append(std::move(s));
}
void db_schema::set_column_nullable(std::string_view t, std::string_view c, bool n) {
    require_postgresql();
    auto s = begin_alter_column(driver_, t, c, " ALTER COLUMN ", 48U);
    s += n ? " DROP NOT NULL" : " SET NOT NULL";
    append(std::move(s));
}
void db_schema::set_column_default(std::string_view t, std::string_view c, db_expression e) {
    auto s = begin_alter_column(driver_, t, c, " ALTER COLUMN ", 64U);
    s += " SET DEFAULT ";
    append_expression(s, e);
    append(std::move(s));
}
void db_schema::drop_column_default(std::string_view t, std::string_view c) {
    auto s = begin_alter_column(driver_, t, c, " ALTER COLUMN ", 48U);
    s += " DROP DEFAULT";
    append(std::move(s));
}
void db_schema::add_constraint(std::string_view t, const db_schema_constraint& c) {
    auto s = begin_alter_table(driver_, t, 64U + c.name_.size());
    s += " ADD CONSTRAINT ";
    append_constraint(s, c);
    append(std::move(s));
}
void db_schema::drop_constraint(std::string_view t, std::string_view n, db_drop_behavior b, bool ie) {
    auto s = begin_alter_table(driver_, t, 56U + n.size());
    s += " DROP CONSTRAINT ";
    if (ie) {
        s += "IF EXISTS ";
    }
    append_db_identifier(s, n, driver_);
    append_drop_behavior(s, b);
    append(std::move(s));
}
void db_schema::validate_constraint(std::string_view t, std::string_view n) {
    require_postgresql();
    auto s = begin_alter_column(driver_, t, n, " VALIDATE CONSTRAINT ", 48U);
    append(std::move(s));
}
void db_schema::create_index(const db_index_definition& i) {
    if (driver_ != db_driver::postgresql && (i.concurrently_ || i.method_ != db_index_method::btree || !i.include_.empty() || !i.where_.empty())) {
        throw std::invalid_argument("selected index options require PostgreSQL");
    }
    if (i.name_.empty() || i.table_.empty() || i.keys_.empty()) {
        throw std::invalid_argument("index name, table and keys are required");
    }
    std::string s;
    s.reserve(64U + i.name_.size() + i.table_.size() + i.keys_.size() * 24U + i.include_.size() * 16U);
    s += "CREATE ";
    if (i.unique_) {
        s += "UNIQUE ";
    }
    s += "INDEX ";
    if (i.concurrently_) {
        s += "CONCURRENTLY ";
    }
    if (i.if_not_exists_) {
        s += "IF NOT EXISTS ";
    }
    append_db_identifier(s, i.name_, driver_);
    if (driver_ == db_driver::mariadb) {
        s += " USING ";
        s += method(i.method_);
    }
    s += " ON ";
    append_db_qualified_identifier(s, i.table_, driver_);
    if (driver_ == db_driver::postgresql) {
        s += " USING ";
        s += method(i.method_);
    }
    s += " (";
    for (size_t n = 0; n < i.keys_.size(); ++n) {
        if (n) {
            s += ", ";
        }
        const auto& key = i.keys_[n];
        if (key.column_.empty() == key.expression_.empty()) {
            throw std::invalid_argument("an index key requires exactly one column or expression");
        }
        if (driver_ != db_driver::postgresql && (!key.expression_.empty() || !key.operator_class_.empty() || key.nulls_ != db_nulls_order::default_value)) {
            throw std::invalid_argument("selected index key options require PostgreSQL");
        }
        if (!i.keys_[n].column_.empty()) {
            append_db_identifier(s, i.keys_[n].column_, driver_);
        } else {
            s += '(';
            append_expression(s, i.keys_[n].expression_);
            s += ')';
        }
        if (!i.keys_[n].operator_class_.empty()) {
            s += ' ';
            append_db_qualified_identifier(s, i.keys_[n].operator_class_, driver_);
        }
        if (i.keys_[n].order_ == db_order_direction::desc) {
            s += " DESC";
        }
        if (i.keys_[n].nulls_ == db_nulls_order::first) {
            s += " NULLS FIRST";
        }
        if (i.keys_[n].nulls_ == db_nulls_order::last) {
            s += " NULLS LAST";
        }
    }
    s += ')';
    if (!i.include_.empty()) {
        s += " INCLUDE (";
        for (size_t n = 0; n < i.include_.size(); ++n) {
            if (n) {
                s += ',';
            }
            append_db_identifier(s, i.include_[n], driver_);
        }
        s += ')';
    }
    if (!i.where_.empty()) {
        s += " WHERE ";
        append_expression(s, i.where_);
    }
    append(std::move(s), i.concurrently_ ? db_migration_atomicity::unwrapped : db_migration_atomicity::transactional);
}
void db_schema::drop_index(std::string_view n, bool c, bool ie) {
    require_postgresql();
    std::string s;
    s.reserve(40U + n.size());
    s += "DROP INDEX ";
    if (c) {
        s += "CONCURRENTLY ";
    }
    if (ie) {
        s += "IF EXISTS ";
    }
    append_db_qualified_identifier(s, n, driver_);
    append(std::move(s), c ? db_migration_atomicity::unwrapped : db_migration_atomicity::transactional);
}
void db_schema::create_enum(std::string_view n, std::span<const std::string_view> v) {
    require_postgresql();
    if (v.empty()) {
        throw std::invalid_argument("enum values are required");
    }
    std::string s;
    s.reserve(32U + n.size() + v.size() * 12U);
    s += "CREATE TYPE ";
    append_db_qualified_identifier(s, n, driver_);
    s += " AS ENUM (";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) {
            s += ',';
        }
        append_db_string_literal(s, v[i], driver_);
    }
    s += ')';
    append(std::move(s));
}
void db_schema::add_enum_value(std::string_view n, std::string_view v, bool ie) {
    require_postgresql();
    std::string s;
    s.reserve(48U + n.size() + v.size());
    s += "ALTER TYPE ";
    append_db_qualified_identifier(s, n, driver_);
    s += " ADD VALUE ";
    if (ie) {
        s += "IF NOT EXISTS ";
    }
    append_db_string_literal(s, v, driver_);
    append(std::move(s), db_migration_atomicity::unwrapped);
}
void db_schema::drop_enum(std::string_view n, db_drop_behavior b, bool ie) {
    require_postgresql();
    auto s = make_drop_object_sql(driver_, "TYPE", n, true, ie);
    append_drop_behavior(s, b);
    append(std::move(s));
}
void db_schema::create_extension(std::string_view n, bool ie) {
    require_postgresql();
    std::string s;
    s.reserve(36U + n.size());
    s += "CREATE EXTENSION ";
    if (ie) {
        s += "IF NOT EXISTS ";
    }
    append_db_identifier(s, n, driver_);
    append(std::move(s));
}
void db_schema::create_view(std::string_view n, const db_query& q, bool replace, bool mat) {
    q.require_select_query();
    if (mat) {
        require_postgresql();
    }
    if (replace && mat) {
        throw std::invalid_argument("materialized views cannot use OR REPLACE");
    }
    auto st = q.compile(driver_, db_parameter_mode::literal);
    std::string s;
    s.reserve(32U + n.size() + st.sql().size());
    s += "CREATE ";
    if (replace) {
        s += "OR REPLACE ";
    }
    if (mat) {
        s += "MATERIALIZED ";
    }
    s += "VIEW ";
    append_db_qualified_identifier(s, n, driver_);
    s += " AS ";
    s += st.sql();
    append(std::move(s));
}
void db_schema::drop_view(std::string_view n, bool mat, db_drop_behavior b, bool ie) {
    if (mat) {
        require_postgresql();
    }
    auto s = make_drop_object_sql(driver_, mat ? "MATERIALIZED VIEW" : "VIEW", n, true, ie);
    append_drop_behavior(s, b);
    append(std::move(s));
}
void db_schema::execute(const db_query& q) {
    if (q.returns_rows()) {
        throw std::invalid_argument("schema execute requires a command without returned rows");
    }
    auto st = q.compile(driver_, db_parameter_mode::literal);
    append(std::string(st.sql()));
}
void db_schema::perform(db_expression e) {
    require_postgresql();
    std::string s;
    s.reserve(64U);
    detail::append_db_perform_statement(s, e);
    append(std::move(s), db_migration_atomicity::transactional, true);
}

void db_schema::run(const db_procedure& procedure) {
    require_postgresql();
    auto body = procedure.body();
    std::string sql;
    sql.reserve(3U + body.size() + 18U);
    sql += "DO ";
    detail::append_db_dollar_body(sql, body);
    append(std::move(sql));
}
void db_schema::create_trigger_function(std::string_view name, const db_procedure& procedure, bool replace) {
    require_postgresql();
    auto body = procedure.body();
    std::string sql;
    sql.reserve((replace ? 27U : 16U) + name.size() + 44U + body.size() + 16U);
    sql += replace ? "CREATE OR REPLACE FUNCTION " : "CREATE FUNCTION ";
    append_db_qualified_identifier(sql, name, driver_);
    sql += "() RETURNS trigger LANGUAGE plpgsql AS ";
    detail::append_db_dollar_body(sql, body);
    append(std::move(sql));
}
void db_schema::drop_trigger_function(std::string_view name, db_drop_behavior behavior, bool if_exists) {
    require_postgresql();
    std::string sql;
    sql.reserve(40U + name.size());
    sql += "DROP FUNCTION ";
    if (if_exists) {
        sql += "IF EXISTS ";
    }
    append_db_qualified_identifier(sql, name, driver_);
    sql += "()";
    append_drop_behavior(sql, behavior);
    append(std::move(sql));
}
void db_schema::create_function(const db_function_definition& function) {
    require_postgresql();
    if (function.name_.empty() || function.body_.empty() ||
        function.body_.view().find('\0') != std::string_view::npos) {
        throw std::invalid_argument("function name and non-NUL body are required");
    }
    std::string sql;
    sql.reserve((function.replace_ ? 27U : 16U) + function.name_.size() + function.parameters_.size() * 24U + 48U + function.body_.size() + 16U);
    sql += function.replace_ ? "CREATE OR REPLACE FUNCTION " : "CREATE FUNCTION ";
    append_db_qualified_identifier(sql, function.name_, driver_);
    sql += '(';
    for (std::size_t i = 0; i < function.parameters_.size(); ++i) {
        if (i != 0) {
            sql += ", ";
        }
        const auto& param = function.parameters_[i];
        if (!param.name_.empty()) {
            append_db_identifier(sql, param.name_, driver_);
            sql += ' ';
        }
        append_type(sql, param.type_);
        if (!param.default_value_.empty()) {
            sql += " DEFAULT ";
            append_expression(sql, param.default_value_);
        }
    }
    sql += ") RETURNS ";
    if (function.returns_set_) {
        sql += "SETOF ";
    }
    append_type(sql, function.return_type_);
    sql += " LANGUAGE ";
    sql += function_language_name(function.language_);
    sql += function_volatility_clause(function.volatility_);
    if (function.strict_) {
        sql += " STRICT";
    }
    sql += " AS ";
    detail::append_db_dollar_body(sql, function.body_);
    append(std::move(sql));
}
void db_schema::create_function(const db_function_definition& function, const db_procedure& body) {
    const auto proc_body = body.body();
    auto copy = function;
    copy.language_ = db_function_language::plpgsql;
    copy.body_ = proc_body;
    create_function(copy);
}
void db_schema::create_function(const db_function_definition& function, const db_query& sql_body) {
    const auto compiled = sql_body.compile(driver_, db_parameter_mode::literal);
    auto copy = function;
    copy.language_ = db_function_language::sql;
    copy.body_ = compiled.sql();
    create_function(copy);
}
void db_schema::drop_function(std::string_view name, std::span<const db_type_definition> parameter_types,
    db_drop_behavior behavior, bool if_exists) {
    require_postgresql();
    std::string sql;
    sql.reserve(28U + name.size() + parameter_types.size() * 12U + (behavior == db_drop_behavior::cascade ? 8U : 0U));
    sql += "DROP FUNCTION ";
    if (if_exists) {
        sql += "IF EXISTS ";
    }
    append_db_qualified_identifier(sql, name, driver_);
    sql += '(';
    for (std::size_t i = 0; i < parameter_types.size(); ++i) {
        if (i != 0) {
            sql += ", ";
        }
        append_type(sql, parameter_types[i]);
    }
    sql += ')';
    append_drop_behavior(sql, behavior);
    append(std::move(sql));
}
void db_schema::lock_table(std::string_view table_value, db_table_lock_mode mode, bool nowait) {
    require_postgresql();
    std::string sql;
    sql.reserve(48U + table_value.size());
    detail::append_db_lock_table_statement(sql, table_value, mode, nowait);
    append(std::move(sql));
}
void db_schema::create_trigger(const db_trigger_definition& trigger) {
    require_postgresql();
    if (trigger.events_.empty()) {
        throw std::invalid_argument("trigger requires an event");
    }
    bool update = false;
    bool truncate = false;
    for (std::size_t i = 0; i < trigger.events_.size(); ++i) {
        update |= trigger.events_[i] == db_trigger_event::update;
        truncate |= trigger.events_[i] == db_trigger_event::truncate;
        for (std::size_t j = 0; j < i; ++j) {
            if (trigger.events_[i] == trigger.events_[j]) {
                throw std::invalid_argument("duplicate trigger event");
            }
        }
    }
    if (!update && !trigger.update_columns_.empty()) {
        throw std::invalid_argument("trigger UPDATE OF requires an update event");
    }
    if (truncate && trigger.for_each_row_) {
        throw std::invalid_argument("TRUNCATE triggers run for each statement");
    }
    if (trigger.timing_ == db_trigger_timing::instead_of && (!trigger.for_each_row_ || !trigger.when_.empty() || !trigger.update_columns_.empty())) {
        throw std::invalid_argument("INSTEAD OF requires a row trigger without WHEN or UPDATE OF");
    }
    if (!trigger.old_table_.empty() || !trigger.new_table_.empty()) {
        if (trigger.timing_ != db_trigger_timing::after || trigger.events_.size() != 1 || !trigger.update_columns_.empty()) {
            throw std::invalid_argument("trigger transition tables require an AFTER trigger for one event without UPDATE OF");
        }
        const auto event = trigger.events_.front();
        if (!trigger.old_table_.empty() && event != db_trigger_event::update && event != db_trigger_event::delete_value) {
            throw std::invalid_argument("trigger OLD TABLE requires an UPDATE or DELETE event");
        }
        if (!trigger.new_table_.empty() && event != db_trigger_event::insert && event != db_trigger_event::update) {
            throw std::invalid_argument("trigger NEW TABLE requires an INSERT or UPDATE event");
        }
        if (!trigger.old_table_.empty() && trigger.old_table_ == trigger.new_table_) {
            throw std::invalid_argument("trigger OLD TABLE and NEW TABLE must have distinct names");
        }
    }
    std::string sql;
    sql.reserve(96U + trigger.name_.size() + trigger.table_.size() + trigger.function_.size() +
                trigger.update_columns_.size() * 16U + trigger.arguments_.size() * 16U);
    sql += "CREATE TRIGGER ";
    append_db_identifier(sql, trigger.name_, driver_);
    sql += trigger_timing_clause(trigger.timing_);
    bool first = true;
    for (const auto event : trigger.events_) {
        if (!first) {
            sql += " OR ";
        }
        first = false;
        sql += trigger_event_name(event);
        if (event == db_trigger_event::update && !trigger.update_columns_.empty()) {
            sql += " OF ";
            for (std::size_t i = 0; i < trigger.update_columns_.size(); ++i) {
                if (i != 0) {
                    sql += ", ";
                }
                append_db_identifier(sql, trigger.update_columns_[i], driver_);
            }
        }
    }
    sql += " ON ";
    append_db_qualified_identifier(sql, trigger.table_, driver_);
    if (!trigger.old_table_.empty() || !trigger.new_table_.empty()) {
        sql += " REFERENCING";
        if (!trigger.old_table_.empty()) {
            sql += " OLD TABLE AS ";
            append_db_identifier(sql, trigger.old_table_, driver_);
        }
        if (!trigger.new_table_.empty()) {
            sql += " NEW TABLE AS ";
            append_db_identifier(sql, trigger.new_table_, driver_);
        }
    }
    sql += trigger.for_each_row_ ? " FOR EACH ROW" : " FOR EACH STATEMENT";
    if (!trigger.when_.empty()) {
        sql += " WHEN (";
        append_expression(sql, trigger.when_);
        sql += ')';
    }
    sql += " EXECUTE FUNCTION ";
    append_db_qualified_identifier(sql, trigger.function_, driver_);
    sql += '(';
    for (std::size_t i = 0; i < trigger.arguments_.size(); ++i) {
        if (i != 0) {
            sql += ", ";
        }
        append_db_string_literal(sql, trigger.arguments_[i], driver_);
    }
    sql += ')';
    append(std::move(sql));
}
void db_schema::drop_trigger(std::string_view table_value, std::string_view name, bool if_exists) {
    require_postgresql();
    std::string sql;
    sql.reserve(40U + name.size() + table_value.size());
    sql += "DROP TRIGGER ";
    if (if_exists) {
        sql += "IF EXISTS ";
    }
    append_db_identifier(sql, name, driver_);
    sql += " ON ";
    append_db_qualified_identifier(sql, table_value, driver_);
    append(std::move(sql));
}
void db_schema::set_table_options(std::string_view table_value, std::span<const db_table_option> options) {
    require_postgresql();
    if (options.empty()) {
        throw std::invalid_argument("table options cannot be empty");
    }
    for (std::size_t i = 0; i < options.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (options[i].name_ == options[j].name_) {
                throw std::invalid_argument("duplicate table option");
            }
        }
    }
    auto sql = begin_alter_table(driver_, table_value, 32U + options.size() * 48U);
    sql += " SET (";
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (i != 0) {
            sql += ", ";
        }
        append_db_qualified_identifier(sql, options[i].name_, driver_);
        sql += " = ";
        std::visit([&](const auto& value) {
            using t_type = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<t_type, bool>) {
                sql += value ? "true" : "false";
            } else if constexpr (std::is_same_v<t_type, std::int64_t>) {
                detail::append_db_number(sql, value);
            } else {
                append_db_string_literal(sql, value, driver_);
            }
        },
            options[i].value_);
    }
    sql += ')';
    append(std::move(sql));
}
void db_schema::set_database_option(std::string_view database_value, std::string_view name, std::string_view value) {
    require_postgresql();
    std::string sql;
    sql.reserve(32U + database_value.size() + name.size() + value.size());
    sql += "ALTER DATABASE ";
    append_db_identifier(sql, database_value, driver_);
    sql += " SET ";
    append_db_qualified_identifier(sql, name, driver_);
    sql += " TO ";
    append_db_string_literal(sql, value, driver_);
    append(std::move(sql));
}
void db_schema::create_hypertable(const db_hypertable_options& options) {
    require_postgresql();
    // Validate names separately from the SQL text values consumed by Timescale.
    auto sql = begin_timescale_perform(
        driver_, "create_hypertable", options.table_, 160U + options.time_column_.size());
    detail::validate_db_identifier(options.time_column_, driver_);
    sql += ", \"by_range\"(";
    append_db_string_literal(sql, options.time_column_, driver_);
    if (!options.chunk_interval_.empty()) {
        sql += ", ";
        append_expression(sql, options.chunk_interval_);
    }
    sql += "), \"create_default_indexes\" => ";
    sql += options.create_default_indexes_ ? "TRUE" : "FALSE";
    sql += ", \"if_not_exists\" => ";
    sql += options.if_not_exists_ ? "TRUE" : "FALSE";
    sql += ", \"migrate_data\" => ";
    sql += options.migrate_data_ ? "TRUE" : "FALSE";
    sql += ')';
    append(std::move(sql), db_migration_atomicity::transactional, true);
}
void db_schema::set_chunk_time_interval(std::string_view table_value, db_expression interval) {
    require_postgresql();
    auto sql = begin_timescale_perform(driver_, "set_chunk_time_interval", table_value, 64U);
    sql += ", ";
    append_expression(sql, interval);
    sql += ')';
    append(std::move(sql), db_migration_atomicity::transactional, true);
}
void db_schema::set_compression(std::string_view table_value, const db_compression_options& options) {
    require_postgresql();
    if (!options.enabled_ && (!options.segment_by_.empty() || !options.order_by_.empty())) {
        throw std::invalid_argument("disabled compression cannot specify segment or order columns");
    }
    std::array<db_table_option, 3> values{db_table_option{"timescaledb.compress", options.enabled_}, {}, {}};
    std::size_t value_count = 1U;
    std::string segments;
    if (!options.segment_by_.empty()) {
        segments.reserve(options.segment_by_.size() * 16U);
        for (const auto& column : options.segment_by_) {
            if (!segments.empty()) {
                segments += ", ";
            }
            append_db_identifier(segments, column, driver_);
        }
        values[value_count++] = {"timescaledb.compress_segmentby", segments};
    }
    std::string order;
    if (!options.order_by_.empty()) {
        order.reserve(options.order_by_.size() * 24U);
        for (const auto& column : options.order_by_) {
            if (!order.empty()) {
                order += ", ";
            }
            append_db_identifier(order, column.column_, driver_);
            order += column.direction_ == db_order_direction::asc ? " ASC" : " DESC";
            if (column.nulls_ == db_nulls_order::first) {
                order += " NULLS FIRST";
            }
            if (column.nulls_ == db_nulls_order::last) {
                order += " NULLS LAST";
            }
        }
        values[value_count++] = {"timescaledb.compress_orderby", order};
    }
    set_table_options(table_value, std::span<const db_table_option>(values.data(), value_count));
}
void db_schema::add_compression_policy(std::string_view table_value, db_expression after, bool if_not_exists) {
    require_postgresql();
    append(make_add_timescale_policy_sql(
               driver_, "add_compression_policy", table_value, "compress_after", after, if_not_exists),
        db_migration_atomicity::transactional, true);
}
void db_schema::remove_compression_policy(std::string_view table_value, bool if_exists) {
    require_postgresql();
    append(make_remove_timescale_policy_sql(driver_, "remove_compression_policy", table_value, if_exists),
        db_migration_atomicity::transactional, true);
}
void db_schema::add_retention_policy(std::string_view table_value, db_expression after, bool if_not_exists) {
    require_postgresql();
    append(make_add_timescale_policy_sql(
               driver_, "add_retention_policy", table_value, "drop_after", after, if_not_exists),
        db_migration_atomicity::transactional, true);
}
void db_schema::remove_retention_policy(std::string_view table_value, bool if_exists) {
    require_postgresql();
    append(make_remove_timescale_policy_sql(driver_, "remove_retention_policy", table_value, if_exists),
        db_migration_atomicity::transactional, true);
}
std::vector<db_migration> db_schema::compile(std::string_view id) const {
    std::vector<db_migration> out;
    if (id.empty()) {
        throw std::invalid_argument("migration id is required");
    }
    if (driver_ == db_driver::postgresql) {
        bool unwrapped = false;
        for (auto& s : statements_) {
            if (s.atomicity_ == db_migration_atomicity::unwrapped) {
                unwrapped = true;
            }
        }
        if (unwrapped && statements_.size() != 1) {
            throw std::invalid_argument("unwrapped schema operation must be alone");
        }
        if (statements_.size() == 1 && !statements_[0].procedural_) {
            out.reserve(1);
            out.emplace_back(db_migration_options{
                std::string(id), std::string(statements_[0].sql_), statements_[0].atomicity_});
        } else if (!statements_.empty()) {
            out.reserve(1);
            std::size_t body_size = 12U;
            for (const auto& s : statements_) {
                body_size += s.sql_.size() + 1U;
            }
            std::array<char, 32> delimiter_buf;
            const auto delimiter = detail::select_db_dollar_delimiter(
                delimiter_buf, [&](std::string_view candidate) noexcept {
                    return std::ranges::any_of(statements_, [&](const auto& s) {
                        return s.sql_.find(candidate) != std::string_view::npos;
                    });
                });
            std::string sql;
            sql.reserve(3U + delimiter.size() * 2U + body_size);
            sql += "DO ";
            sql.append(delimiter);
            sql.append(" BEGIN ");
            for (const auto& s : statements_) {
                sql.append(s.sql_);
                sql.push_back(';');
            }
            sql.append(" END ");
            sql.append(delimiter);
            out.emplace_back(db_migration_options{
                std::string(id), std::move(sql), db_migration_atomicity::transactional});
        }
    } else {
        out.reserve(statements_.size());
        for (size_t i = 0; i < statements_.size(); ++i) {
            std::array<char, http_max_unsigned_decimal_size> digits;
            const auto suffix = http_format_unsigned(digits, static_cast<std::uint64_t>(i + 1U));
            std::string numbered;
            numbered.reserve(id.size() + 1U + suffix.size());
            numbered.append(id);
            numbered.push_back('_');
            numbered.append(suffix);
            out.emplace_back(db_migration_options{
                std::move(numbered), std::string(statements_[i].sql_), statements_[i].atomicity_});
        }
    }
    return out;
}

}  // namespace ruvia

namespace ruvia {
namespace {
constexpr auto pg = db_driver::postgresql;
void require_sql_state(std::string_view state_value) {
    if (state_value.size() != 5 || state_value == "00000" || !std::ranges::all_of(state_value, [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z'); })) {
        throw std::invalid_argument("SQLSTATE must have five uppercase alphanumeric characters and indicate failure");
    }
}
class procedure_append_guard final {
public:
    explicit procedure_append_guard(std::string& target, std::size_t extra_reserve = 48U)
        : target_(target),
          initial_size_(target.size()) {
        if (target_.capacity() - initial_size_ < extra_reserve) {
            target_.reserve(initial_size_ + (std::max)(extra_reserve, std::size_t{64}));
        }
    }

    procedure_append_guard(const procedure_append_guard&) = delete;
    procedure_append_guard& operator=(const procedure_append_guard&) = delete;

    void commit() noexcept {
        committed_ = true;
    }

    ~procedure_append_guard() {
        if (!committed_) {
            target_.resize(initial_size_);
        }
    }

private:
    std::string& target_;
    std::size_t initial_size_;
    bool committed_{false};
};
}  // namespace

db_procedure::db_procedure() noexcept = default;
void db_procedure::append_expression(std::string& output, db_expression value) const {
    detail::append_db_expression(output, value, pg);
}
void db_procedure::require_unique_variable(std::string_view name) const {
    if (std::ranges::any_of(variables_, [&](const auto& existing) { return existing == name; })) {
        throw std::invalid_argument("duplicate procedure variable");
    }
}
void db_procedure::declare_variable(const db_variable_definition& variable) {
    require_unique_variable(variable.name_);
    procedure_append_guard guard(declarations_, 32U + variable.name_.size());
    detail::append_db_identifier(declarations_, variable.name_, pg);
    declarations_ += ' ';
    const auto& t = variable.type_;
    detail::append_db_type_name(
        declarations_, t.data_type_, t.custom_name_, t.length_, t.precision_, t.scale_, t.array_, pg);
    if (!variable.default_value_.empty()) {
        declarations_ += " := ";
        append_expression(declarations_, variable.default_value_);
    }
    declarations_ += ";\n";
    variables_.emplace_back(variable.name_);
    guard.commit();
}
void db_procedure::declare_row(std::string_view name, std::string_view table_value) {
    require_unique_variable(name);
    procedure_append_guard guard(declarations_, 16U + name.size() + table_value.size());
    detail::append_db_identifier(declarations_, name, pg);
    declarations_ += ' ';
    detail::append_db_qualified_identifier(declarations_, table_value, pg);
    declarations_ += "%ROWTYPE;\n";
    variables_.emplace_back(name);
    guard.commit();
}
void db_procedure::declare_record(std::string_view name) {
    require_unique_variable(name);
    procedure_append_guard guard(declarations_, name.size() + 11U);
    detail::append_db_identifier(declarations_, name, pg);
    declarations_ += " RECORD;\n";
    variables_.emplace_back(name);
    guard.commit();
}
void db_procedure::assign(std::string_view target, db_expression value) {
    procedure_append_guard guard(statements_, 32U + target.size());
    detail::append_db_qualified_identifier(statements_, target, pg);
    statements_ += " := ";
    append_expression(statements_, value);
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::select_into(const db_query& query, std::span<const std::string_view> targets, bool strict) {
    if (targets.empty() || !query.returns_rows()) {
        throw std::invalid_argument("SELECT INTO requires returned rows and target variables");
    }
    const auto statement = query.compile(pg, db_parameter_mode::literal);
    procedure_append_guard guard(statements_, statement.sql().size() + 16U + targets.size() * 16U);
    statements_ += statement.sql();
    statements_ += strict ? " INTO STRICT " : " INTO ";
    bool first = true;
    for (const auto target : targets) {
        if (!first) {
            statements_ += ", ";
        }
        first = false;
        detail::append_db_qualified_identifier(statements_, target, pg);
    }
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::execute(const db_query& query) {
    if (query.returns_rows()) {
        throw std::invalid_argument("procedure execute requires a command; consume returned rows with select_into");
    }
    const auto statement = query.compile(pg, db_parameter_mode::literal);
    procedure_append_guard guard(statements_, statement.sql().size() + 2U);
    statements_ += statement.sql();
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::perform(db_expression expression) {
    procedure_append_guard guard(statements_, 48U);
    detail::append_db_perform_statement(statements_, expression);
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::apply(const db_schema& schema) {
    if (schema.driver_ != pg) {
        throw std::invalid_argument("PL/pgSQL requires a PostgreSQL schema");
    }
    for (const auto& statement : schema.statements_) {
        if (statement.atomicity_ != db_migration_atomicity::transactional) {
            throw std::invalid_argument("nontransactional schema operations cannot run in a procedure");
        }
    }
    procedure_append_guard guard(statements_, 0);
    for (const auto& statement : schema.statements_) {
        statements_ += statement.sql_;
        statements_ += ";\n";
    }
    guard.commit();
}
void db_procedure::begin_if(db_expression condition) {
    procedure_append_guard guard(statements_, 48U);
    statements_ += "IF ";
    append_expression(statements_, condition);
    statements_ += " THEN\n";
    blocks_.push_back(block_type::if_value);
    guard.commit();
}
void db_procedure::else_if(db_expression condition) {
    if (blocks_.empty() || blocks_.back() != block_type::if_value) {
        throw std::invalid_argument("ELSEIF requires an open IF without ELSE");
    }
    procedure_append_guard guard(statements_, 48U);
    statements_ += "ELSIF ";
    append_expression(statements_, condition);
    statements_ += " THEN\n";
    guard.commit();
}
void db_procedure::otherwise() {
    if (blocks_.empty() || blocks_.back() != block_type::if_value) {
        throw std::invalid_argument("ELSE requires an open IF without ELSE");
    }
    statements_ += "ELSE\n";
    blocks_.back() = block_type::else_value;
}
void db_procedure::end_if() {
    if (blocks_.empty() || (blocks_.back() != block_type::if_value && blocks_.back() != block_type::else_value)) {
        throw std::invalid_argument("END IF requires an open IF");
    }
    statements_ += "END IF;\n";
    blocks_.pop_back();
}
void db_procedure::begin_block() {
    procedure_append_guard guard(statements_, 6U);
    statements_ += "BEGIN\n";
    blocks_.push_back(block_type::begin);
    guard.commit();
}
void db_procedure::catch_sql_state(std::string_view state_value) {
    if (blocks_.empty() || (blocks_.back() != block_type::begin && blocks_.back() != block_type::exception)) {
        throw std::invalid_argument("exception handler requires an open block");
    }
    require_sql_state(state_value);
    const std::string_view prefix = blocks_.back() == block_type::begin ? "EXCEPTION\nWHEN SQLSTATE "
                                                                        : "WHEN SQLSTATE ";
    procedure_append_guard guard(statements_, prefix.size() + 20U);
    statements_.append(prefix);
    detail::append_db_string_literal(statements_, state_value, pg);
    statements_ += " THEN\nNULL;\n";
    blocks_.back() = block_type::exception;
    guard.commit();
}
void db_procedure::end_block() {
    if (blocks_.empty() || (blocks_.back() != block_type::begin && blocks_.back() != block_type::exception)) {
        throw std::invalid_argument("END requires an open block");
    }
    statements_ += "END;\n";
    blocks_.pop_back();
}
void db_procedure::begin_for_query(std::string_view target, const db_query& query) {
    if (!query.returns_rows()) {
        throw std::invalid_argument("FOR IN query loop requires a row-returning query");
    }
    const auto statement = query.compile(pg, db_parameter_mode::literal);
    procedure_append_guard guard(statements_, 16U + target.size() + statement.sql().size());
    statements_.append("FOR ");
    detail::append_db_identifier(statements_, target, pg);
    statements_.append(" IN ");
    statements_.append(statement.sql());
    statements_.append(" LOOP\n");
    blocks_.push_back(block_type::loop);
    guard.commit();
}
void db_procedure::begin_foreach(std::string_view target, db_expression array, unsigned slice) {
    procedure_append_guard guard(statements_, 48U + target.size());
    statements_ += "FOREACH ";
    detail::append_db_identifier(statements_, target, pg);
    if (slice > 0) {
        statements_ += " SLICE ";
        detail::append_db_number(statements_, static_cast<std::uint64_t>(slice));
    }
    statements_ += " IN ARRAY ";
    append_expression(statements_, array);
    statements_ += " LOOP\n";
    blocks_.push_back(block_type::loop);
    guard.commit();
}
void db_procedure::end_loop() {
    if (blocks_.empty() || blocks_.back() != block_type::loop) {
        throw std::invalid_argument("END LOOP requires an open FOR or FOREACH loop");
    }
    statements_ += "END LOOP;\n";
    blocks_.pop_back();
}
void db_procedure::execute_dynamic(db_expression command, std::span<const db_expression> using_args) {
    procedure_append_guard guard(statements_, 48U + using_args.size() * 24U);
    statements_ += "EXECUTE ";
    append_expression(statements_, command);
    if (!using_args.empty()) {
        statements_ += " USING ";
        for (std::size_t i = 0; i < using_args.size(); ++i) {
            if (i != 0) {
                statements_ += ", ";
            }
            append_expression(statements_, using_args[i]);
        }
    }
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::lock_table(std::string_view table, db_table_lock_mode mode, bool nowait) {
    procedure_append_guard guard(statements_, 48U + table.size());
    detail::append_db_lock_table_statement(statements_, table, mode, nowait);
    statements_.append(";\n");
    guard.commit();
}
void db_procedure::return_value(db_expression expression) {
    procedure_append_guard guard(statements_, 32U);
    statements_ += "RETURN";
    if (!expression.empty()) {
        statements_ += ' ';
        append_expression(statements_, expression);
    }
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::raise_exception(std::string_view message, std::string_view sql_state) {
    require_sql_state(sql_state);
    procedure_append_guard guard(statements_, 56U + message.size() + sql_state.size());
    statements_ += "RAISE EXCEPTION USING MESSAGE = ";
    detail::append_db_string_literal(statements_, message, pg);
    statements_ += ", ERRCODE = ";
    detail::append_db_string_literal(statements_, sql_state, pg);
    statements_ += ";\n";
    guard.commit();
}
std::string db_procedure::body() const {
    if (!blocks_.empty()) {
        throw std::invalid_argument("procedure has unclosed control-flow blocks");
    }
    const std::string_view statements =
        statements_.empty() ? std::string_view("NULL;\n") : std::string_view(statements_);
    std::string result;
    result.reserve((!declarations_.empty() ? 8U + declarations_.size() : 0U) + 6U + statements.size() + 3U);
    if (!declarations_.empty()) {
        result.append("DECLARE\n");
        result.append(declarations_);
    }
    result.append("BEGIN\n");
    result.append(statements);
    result.append("END");
    return result;
}

}  // namespace ruvia

namespace ruvia {
db_value::db_value(std::nullptr_t)
    : storage_(std::monostate{}) {}

db_value::db_value(const char* value)
    : storage_(value == nullptr ? storage_type(std::monostate{})
                                : storage_type(std::in_place_type<borrowed_text>, value)) {}

db_value::db_value(std::string_view value)
    : storage_(std::in_place_type<borrowed_text>, value) {}

db_value::db_value(std::in_place_type_t<std::string>, std::string value)
    : storage_(std::in_place_type<std::string>, std::move(value)) {}

db_value::db_value(const db_value& other)
    : storage_(std::visit(
          [](const auto& value) -> storage_type {
              using value_type = std::remove_cvref_t<decltype(value)>;
              if constexpr (std::is_same_v<value_type, std::string>) {
                  return storage_type(std::in_place_type<value_type>, value, value.get_allocator());
              } else {
                  return storage_type(std::in_place_type<value_type>, value);
              }
          },
          other.storage_)) {}

db_value::db_value(bool value)
    : storage_(std::in_place_type<bool>, value) {}

db_value_type db_value::type() const noexcept {
    return std::visit(
        [](const auto& value) noexcept {
            using value_type = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<value_type, std::monostate>) {
                return db_value_type::null;
            } else if constexpr (std::is_same_v<value_type, borrowed_text> ||
                                 std::is_same_v<value_type, std::string>) {
                return db_value_type::string;
            } else if constexpr (std::is_same_v<value_type, std::int64_t>) {
                return db_value_type::signed_value;
            } else if constexpr (std::is_same_v<value_type, std::uint64_t>) {
                return db_value_type::unsigned_value;
            } else if constexpr (std::is_same_v<value_type, double>) {
                return db_value_type::double_value;
            } else {
                return db_value_type::bool_value;
            }
        },
        storage_);
}

std::string_view db_value::text() const& {
    if (const auto* borrowed = std::get_if<borrowed_text>(&storage_)) {
        return borrowed->view();
    }
    return std::get<std::string>(storage_);
}

std::int64_t db_value::signed_value() const {
    return std::get<std::int64_t>(storage_);
}

std::uint64_t db_value::unsigned_value() const {
    return std::get<std::uint64_t>(storage_);
}

double db_value::double_value() const {
    return std::get<double>(storage_);
}

bool db_value::bool_value() const {
    return std::get<bool>(storage_);
}

db_field::db_field() noexcept = default;

db_field::db_field(std::nullptr_t) noexcept {}

db_field::db_field(std::string_view value)
    : storage_(std::in_place_type<std::string>, value) {}

db_field::db_field(borrowed_tag_type, std::string_view value) noexcept
    : storage_(std::in_place_type<borrowed_text>, value) {}

db_field db_field::borrowed(std::string_view value) noexcept {
    return db_field(borrowed_tag_type{}, value);
}

db_field::db_field(db_field&& other) noexcept
    : storage_(std::move(other.storage_)) {
    other.storage_.emplace<std::monostate>();
}

// NOLINTNEXTLINE(performance-noexcept-move-constructor)
db_field& db_field::operator=(db_field&& other) noexcept = default;

std::optional<std::string_view> db_field::value() const& noexcept {
    if (const auto* owned = std::get_if<std::string>(&storage_)) {
        return *owned;
    }
    if (const auto* borrowed = std::get_if<borrowed_text>(&storage_)) {
        return borrowed->view();
    }
    return std::nullopt;
}

db_row::db_row(const db_field* fields_value, std::size_t size, const std::string* column_names,
    std::size_t column_count) noexcept
    : fields_(fields_value, size),
      column_names_(column_names, column_count) {}

db_row::db_row(db_row&& other) noexcept
    : fields_(std::exchange(other.fields_, {})),
      column_names_(std::exchange(other.column_names_, {})) {}

db_row& db_row::operator=(db_row&& other) noexcept {
    if (this != &other) {
        fields_ = std::exchange(other.fields_, {});
        column_names_ = std::exchange(other.column_names_, {});
    }
    return *this;
}

bool db_row::empty() const noexcept {
    return fields_.empty();
}

std::size_t db_row::size() const noexcept {
    return fields_.size();
}

const db_field& db_row::operator[](std::size_t index) const& noexcept {
    return fields_[index];
}

bool db_row::contains(std::string_view column) const noexcept {
    return find(column) != nullptr;
}

const db_field* db_row::find(std::string_view column) const& noexcept {
    for (std::size_t index = 0; index < column_names_.size(); ++index) {
        if (column_names_[index] == column) {
            return &fields_[index];
        }
    }
    return nullptr;
}

const db_field& db_row::operator[](std::string_view column) const& {
    if (const auto* field = find(column)) {
        return *field;
    }
    throw std::out_of_range("database result has no such column");
}

const db_field* db_row::begin() const& noexcept {
    return fields_.data();
}

const db_field* db_row::end() const& noexcept {
    const auto* first = begin();
    return fields_.empty() ? first : first + fields_.size();
}

std::span<const std::string> db_row::column_names() const& noexcept {
    return column_names_;
}

db_rows::db_rows() noexcept = default;

db_rows::db_rows(db_rows&& other) noexcept
    : rows_(std::move(other.rows_)),
      fields_(std::move(other.fields_)),
      column_names_(std::move(other.column_names_)),
      raw_result_(other.raw_result_) {
    other.raw_result_.template emplace<no_raw_result_type>();
}

db_rows::~db_rows() {
    if (const auto* owned = std::get_if<owned_raw_result_type>(&raw_result_)) {
        owned->release_(owned->value_);
    }
}

db_rows& db_rows::operator=(db_rows&& other) {
    if (this == &other) {
        return *this;
    }
    if (const auto* owned = std::get_if<owned_raw_result_type>(&raw_result_)) {
        owned->release_(owned->value_);
    }
    rows_ = std::move(other.rows_);
    fields_ = std::move(other.fields_);
    column_names_ = std::move(other.column_names_);
    raw_result_ = other.raw_result_;
    other.raw_result_.template emplace<no_raw_result_type>();
    return *this;
}

bool db_rows::empty() const noexcept {
    return rows_.empty();
}

std::size_t db_rows::size() const noexcept {
    return rows_.size();
}

const db_row& db_rows::operator[](std::size_t index) const& noexcept {
    return rows_[index];
}

const db_row* db_rows::begin() const& noexcept {
    return rows_.data();
}

const db_row* db_rows::end() const& noexcept {
    const auto* first = begin();
    return rows_.empty() ? first : first + rows_.size();
}

const db_row& db_rows::front() const& noexcept {
    return rows_.front();
}

const db_row& db_rows::back() const& noexcept {
    return rows_.back();
}

std::span<const std::string> db_rows::column_names() const& noexcept {
    return column_names_;
}

db_migration_report::db_migration_report() noexcept = default;

db_migration_report& db_migration_report::operator=(db_migration_report&& other) = default;
std::span<const std::string> db_migration_report::applied() const& noexcept {
    return applied_;
}

std::span<const std::string> db_migration_report::skipped() const& noexcept {
    return skipped_;
}

bool db_migration_report::changed() const noexcept {
    return !applied_.empty();
}

}  // namespace ruvia
