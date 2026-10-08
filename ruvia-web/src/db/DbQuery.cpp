#include "ruvia/web/db/DbQuery.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>

#include "ruvia/core/memory/PmrResource.h"

#include "db/DbSqlFormat.h"
#include "query_storage.h"

namespace ruvia::detail {

namespace {
void requireName(std::string_view name, bool qualified = false) {
    if (name.empty() || name.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("database identifier must be nonempty and contain no NUL");
    }
    if (qualified && (name.front() == '.' || name.back() == '.' || name.find("..") != std::string_view::npos)) {
        throw std::invalid_argument("database qualified identifier has an empty component");
    }
}
void setSourceOptions(DbQuerySource& source, const DbSourceOptions& options, std::pmr::memory_resource* resource) {
    source.lateral = options.lateral;
    source.ordinality = options.withOrdinality;
    for (const auto& column : options.columns) {
        requireName(column.name);
        source.columns.emplace_back(column, resource);
    }
}
}  // namespace

}  // namespace ruvia::detail

namespace ruvia {

using detail::DbNodeKind;
using detail::DbQueryKind;
using detail::DbSourceKind;
using detail::noDbNode;

void DbQuery::StorageDeleter::operator()(detail::DbQueryStorage* storage) const noexcept {
    if (storage != nullptr) {
        std::destroy_at(storage);
        resource->deallocate(storage, sizeof(detail::DbQueryStorage), alignof(detail::DbQueryStorage));
    }
}

DbQuery::DbQuery(std::pmr::memory_resource* resource)
    : storage_(nullptr, StorageDeleter{detail::pmrResourceOrDefault(resource)}) {
    auto* resolved = storage_.get_deleter().resource;
    void* block = resolved->allocate(sizeof(detail::DbQueryStorage), alignof(detail::DbQueryStorage));
    try {
        storage_.reset(std::construct_at(static_cast<detail::DbQueryStorage*>(block), resolved));
    } catch (...) {
        resolved->deallocate(block, sizeof(detail::DbQueryStorage), alignof(detail::DbQueryStorage));
        throw;
    }
}
DbQuery::DbQuery(StorageOwner storage) noexcept
    : storage_(std::move(storage)) {}
DbQuery::DbQuery(DbQuery&&) noexcept = default;
DbQuery& DbQuery::operator=(DbQuery&&) noexcept = default;
DbQuery::~DbQuery() = default;

detail::DbQueryStorage& DbQuery::storage() {
    if (!storage_) {
        throw std::logic_error("database query was moved from");
    }
    return *storage_;
}
const detail::DbQueryStorage& DbQuery::storage() const {
    if (!storage_) {
        throw std::logic_error("database query was moved from");
    }
    return *storage_;
}
std::pmr::memory_resource* DbQuery::resource() const {
    return storage().resource;
}
bool DbQuery::returnsRows() const {
    return storage().returnsRows();
}
void DbQuery::requireSelectQuery() const {
    if (storage().kind != DbQueryKind::kSelect && storage().kind != DbQueryKind::kValues) {
        throw std::invalid_argument("subqueries require SELECT or VALUES; use a CTE for DML RETURNING");
    }
    for (const auto& cte : storage().ctes) {
        storage().queries.at(cte.query).requireSelectQuery();
    }
}
bool DbQuery::hasWrites() const {
    const auto& s = storage();
    return (s.kind != DbQueryKind::kSelect && s.kind != DbQueryKind::kValues) ||
           std::ranges::any_of(s.queries, [](const auto& query) { return query.hasWrites(); });
}
bool DbQuery::hasWhere() const {
    return storage().predicate != noDbNode;
}
bool DbQuery::hasGrouping() const {
    return !storage().groups.empty() || storage().having != noDbNode;
}
bool DbQuery::usesSourceName(std::string_view name) const {
    const auto matches = [name](const detail::DbQuerySource& source) {
        if (!source.alias.empty()) {
            return source.alias == name;
        }
        const auto full = std::string_view(source.name);
        const auto dot = full.rfind('.');
        return full == name || full.substr(dot == std::string_view::npos ? 0 : dot + 1) == name;
    };
    const auto& s = storage();
    return (s.source && matches(*s.source)) || std::ranges::any_of(s.joins,
                                                   [&](const auto& join) { return matches(join.source); });
}
bool DbQuery::usesProjectionName(std::string_view name) const {
    const auto& s = storage();
    return std::ranges::any_of(s.projections, [&](auto index) {
        const auto& node = s.nodes[index];
        return (node.kind == DbNodeKind::kColumn || node.kind == DbNodeKind::kAlias) && node.text == name;
    });
}

std::optional<DbQuery> DbQuery::prepareEntityRead(
    std::span<const std::string_view> primaryKey, std::string_view rootAlias,
    DbDriver driver) const {
    const auto& s = storage();
    if (s.kind != DbQueryKind::kSelect || primaryKey.empty()) {
        throw std::invalid_argument("relation loading requires an entity SELECT with a primary key");
    }
    const bool paginated = s.limit.has_value() || s.offset.has_value();
    const bool scopeLock = driver == DbDriver::kPostgreSql && s.lock && s.lock->tables.empty();
    if (!paginated && !scopeLock) {
        return std::nullopt;
    }
    auto result = clone(s.resource);
    if (scopeLock) {
        result.storage().lock->tables.emplace_back(rootAlias);
    }
    if (!paginated) {
        return result;
    }
    if (hasGrouping() || !s.setOperations.empty() || !s.distinctOn.empty()) {
        throw std::invalid_argument("paged relation loading requires an ungrouped SELECT without set operations or DISTINCT ON");
    }
    if (s.lock && s.lock->skipLocked) {
        throw std::invalid_argument("paged relation loading cannot combine SKIP LOCKED with collection expansion");
    }

    // Select a page of root identities before filtering the expanded result.
    // Ranking makes joined ordering deterministic for each root, and avoids
    // applying LIMIT to child rows or truncating a loaded collection.
    auto ranked = clone(s.resource);
    auto& rankStorage = ranked.storage();
    // Refer to the outer WITH bindings. Copying their declarations into the
    // page would evaluate volatile CTEs twice and relocate DML CTEs illegally.
    rankStorage.ctes.clear();
    auto orders = std::move(rankStorage.orders);
    rankStorage.limit.reset();
    rankStorage.offset.reset();
    rankStorage.lock.reset();
    rankStorage.distinct = false;
    for (auto& order : orders) {
        const auto& node = rankStorage.nodes[order.expression];
        if (node.kind == DbNodeKind::kColumn && node.qualifier.empty()) {
            for (const auto index : rankStorage.projections) {
                const auto& projection = rankStorage.nodes[index];
                if (projection.kind == DbNodeKind::kAlias && projection.text == node.text) {
                    order.expression = projection.left;
                    break;
                }
            }
        }
    }
    rankStorage.projections.clear();
    detail::DbQueryNode window(DbNodeKind::kWindow, s.resource);
    window.left = ranked.requireExpression(ranked.call("row_number"));
    std::pmr::vector<std::pmr::string> keyNames(s.resource), orderNames(s.resource);
    for (std::size_t i = 0; i < primaryKey.size(); ++i) {
        auto& name = keyNames.emplace_back("__ruvia_page_key_");
        detail::appendDbNumber(name, static_cast<std::uint64_t>(i));
        const auto key = ranked.column(primaryKey[i], rootAlias);
        window.args.push_back(ranked.requireExpression(key));
        ranked.addSelect(ranked.alias(key, name));
    }
    if (orders.empty()) {
        for (const auto key : window.args) {
            orders.push_back({key, DbOrderDirection::kAsc, DbNullsOrder::kDefault});
        }
    }
    // Give the outer result the same stable tie order as its page.
    for (const auto key : primaryKey) {
        result.addOrderBy(result.column(key, rootAlias));
    }
    for (std::size_t i = 0; i < orders.size(); ++i) {
        auto& name = orderNames.emplace_back("__ruvia_page_order_");
        detail::appendDbNumber(name, static_cast<std::uint64_t>(i));
        ranked.addSelect(ranked.alias(Expr(&rankStorage, orders[i].expression), name));
    }
    window.orders = orders;
    rankStorage.nodes.push_back(std::move(window));
    ranked.addSelect(ranked.alias(Expr(&rankStorage, rankStorage.nodes.size() - 1), "__ruvia_page_rank"));

    DbQuery page(s.resource);
    for (const auto& key : keyNames) {
        page.addSelect(page.column(key, "__ruvia_ranked"));
    }
    page.from(ranked, "__ruvia_ranked");
    page.where(page.binary(page.column("__ruvia_page_rank", "__ruvia_ranked"),
        DbBinaryOperator::kEqual, page.value(1)));
    for (std::size_t i = 0; i < orders.size(); ++i) {
        page.addOrderBy(page.column(orderNames[i], "__ruvia_ranked"), orders[i].direction, orders[i].nulls);
    }
    // Break ordering ties using the complete primary key, including composite keys.
    for (const auto& key : keyNames) {
        page.addOrderBy(page.column(key, "__ruvia_ranked"));
    }
    page.limit(s.limit).offset(s.offset);
    DbQuery included(s.resource);
    const auto pageAlias = rootAlias == "__ruvia_page" ? "__ruvia_page_1" : "__ruvia_page";
    included.select(included.value(1)).from(page, pageAlias);
    for (std::size_t i = 0; i < primaryKey.size(); ++i) {
        included.andWhere(included.binary(included.column(keyNames[i], pageAlias),
            DbBinaryOperator::kEqual, included.column(primaryKey[i], rootAlias)));
    }
    result.limit(std::nullopt).offset(std::nullopt);
    result.andWhere(result.exists(included));
    return result;
}
std::size_t DbQuery::requireExpression(Expr expression) const {
    if (expression.owner_ != storage_.get() || expression.node_ >= storage().nodes.size()) {
        throw std::invalid_argument("database expression belongs to another query; import it first");
    }
    return expression.node_;
}

DbQuery::StorageOwner DbQuery::copyStorage(const detail::DbQueryStorage& source, std::pmr::memory_resource* resource) {
    DbQuery copy(resource);
    auto& target = copy.storage();
    target.cacheEnabled = source.cacheEnabled;
    target.cacheDuration = source.cacheDuration;
    target.cacheId = source.cacheId;
    target.kind = source.kind;
    target.target = source.target;
    target.targetAlias = source.targetAlias;
    target.columns = source.columns;
    target.projections = source.projections;
    target.groups = source.groups;
    target.orders = source.orders;
    target.distinct = source.distinct;
    target.distinctOn = source.distinctOn;
    target.returning = source.returning;
    target.rows = source.rows;
    target.insertQuery = source.insertQuery;
    target.predicate = source.predicate;
    target.having = source.having;
    target.limit = source.limit;
    target.offset = source.offset;
    target.setOperations = source.setOperations;
    target.nodes.reserve(source.nodes.size());
    for (const auto& node : source.nodes) {
        target.nodes.emplace_back(node, target.resource);
    }
    target.queries.reserve(source.queries.size());
    for (const auto& query : source.queries) {
        target.queries.push_back(query.clone(target.resource));
    }
    if (source.source) {
        target.source.emplace(*source.source, target.resource);
    }
    for (const auto& join : source.joins) {
        target.joins.emplace_back(join, target.resource);
    }
    for (const auto& assignment : source.assignments) {
        target.assignments.emplace_back(assignment.column, assignment.expression, target.resource);
    }
    for (const auto& cte : source.ctes) {
        target.ctes.emplace_back(cte, target.resource);
    }
    if (source.conflict) {
        target.conflict.emplace(*source.conflict, target.resource);
    }
    if (source.lock) {
        target.lock.emplace(*source.lock, target.resource);
    }
    return std::move(copy.storage_);
}
DbQuery DbQuery::clone(std::pmr::memory_resource* resource) const {
    return DbQuery(copyStorage(storage(), detail::pmrResourceOrDefault(resource)));
}

bool DbQuery::cacheable() const {
    const auto& s = storage();
    return (s.kind == DbQueryKind::kSelect || s.kind == DbQueryKind::kValues) && !s.lock &&
           std::ranges::all_of(s.queries, [](const auto& query) { return query.cacheable(); });
}
std::optional<bool> DbQuery::cacheEnabled() const {
    return storage().cacheEnabled;
}
std::optional<std::chrono::milliseconds> DbQuery::cacheDuration() const {
    return storage().cacheDuration;
}
std::string_view DbQuery::cacheId() const {
    return storage().cacheId;
}
void DbQuery::copyCache(const DbQuery& source, std::string_view suffix) {
    auto& s = storage();
    s.cacheEnabled = source.cacheEnabled();
    s.cacheDuration = source.cacheDuration();
    s.cacheId = source.cacheId();
    if (!s.cacheId.empty()) {
        s.cacheId.append(suffix);
    }
}
DbQuery& DbQuery::cache(const DbCacheSetting& setting) {
    return std::visit([&](const auto& value) -> DbQuery& {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<T, DbCacheOptions>) {
            return cache(std::string_view(value.id), value.milliseconds);
        } else if constexpr (std::same_as<T, std::chrono::milliseconds>) {
            return cache(std::string_view{}, value);
        } else {
            auto& s = storage();
            s.cacheEnabled.reset();
            if constexpr (std::same_as<T, bool>) {
                s.cacheEnabled = value;
            }
            s.cacheDuration.reset();
            s.cacheId.clear();
            return *this;
        }
    },
        setting);
}
DbQuery& DbQuery::cache(std::string_view id, std::optional<std::chrono::milliseconds> milliseconds) {
    if (milliseconds && milliseconds->count() <= 0) {
        throw std::invalid_argument("cache duration must be positive");
    }
    auto& s = storage();
    s.cacheId = id;
    s.cacheDuration = milliseconds;
    s.cacheEnabled = true;
    return *this;
}

DbQuery::Expr DbQuery::column(std::string_view name, std::string_view table) {
    detail::requireName(name);
    if (!table.empty()) {
        detail::requireName(table, true);
    }
    auto& s = storage();
    detail::DbQueryNode node(DbNodeKind::kColumn, s.resource);
    node.text = name;
    node.qualifier = table;
    s.nodes.push_back(std::move(node));
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::star(std::string_view table) {
    if (!table.empty()) {
        detail::requireName(table, true);
    }
    auto& s = storage();
    detail::DbQueryNode node(DbNodeKind::kStar, s.resource);
    node.qualifier = table;
    s.nodes.push_back(std::move(node));
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::value(DbValue value) {
    auto& s = storage();
    detail::DbQueryNode node(DbNodeKind::kValue, s.resource);
    auto owned = detail::cloneDbValueForResource(value, s.resource);
    std::destroy_at(&node.value);
    std::construct_at(&node.value, std::move(owned));
    s.nodes.push_back(std::move(node));
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::nullValue() {
    return value(DbValue(nullptr));
}
DbQuery::Expr DbQuery::excluded(std::string_view column) {
    auto result = this->column(column);
    storage().nodes[result.node_].kind = DbNodeKind::kExcluded;
    return result;
}
DbQuery::Expr DbQuery::defaultValue() {
    auto& s = storage();
    s.nodes.emplace_back(DbNodeKind::kDefault, s.resource);
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::sql(std::span<const std::string_view> parts, std::span<const Expr> args) {
    if (parts.size() != args.size() + 1 || (args.empty() && parts.front().empty())) {
        throw std::invalid_argument("SQL expression requires one more syntax part than arguments");
    }
    auto& s = storage();
    detail::DbQueryNode node(DbNodeKind::kSql, s.resource);
    for (auto part : parts) {
        if (part.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("SQL expression cannot contain NUL");
        }
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        node.named.emplace_back(parts[i], requireExpression(args[i]), s.resource);
    }
    node.text = parts.back();
    s.nodes.push_back(std::move(node));
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::call(std::string_view function, std::span<const Expr> args, std::span<const DbNamedArgument> named) {
    detail::requireName(function, true);
    auto& s = storage();
    detail::DbQueryNode node(DbNodeKind::kFunction, s.resource);
    node.text = function;
    for (const auto arg : args) {
        node.args.push_back(requireExpression(arg));
    }
    for (const auto& arg : named) {
        detail::requireName(arg.name);
        if (std::ranges::any_of(node.named, [&](const auto& existing) { return std::string_view(existing.name) == arg.name; })) {
            throw std::invalid_argument("duplicate named database function argument");
        }
        node.named.emplace_back(arg.name, requireExpression(arg.value), s.resource);
    }
    s.nodes.push_back(std::move(node));
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::coalesce(std::span<const Expr> args) {
    if (args.empty()) {
        throw std::invalid_argument("COALESCE requires an argument");
    }
    auto result = call("coalesce", args);
    storage().nodes[result.node_].kind = DbNodeKind::kCoalesce;
    return result;
}
DbQuery::Expr DbQuery::nullIf(Expr value, Expr other) {
    const std::array args{value, other};
    auto result = call("nullif", args);
    storage().nodes[result.node_].kind = DbNodeKind::kNullIf;
    return result;
}
DbQuery::Expr DbQuery::greatest(std::span<const Expr> args) {
    if (args.empty()) {
        throw std::invalid_argument("GREATEST requires an argument");
    }
    auto result = call("greatest", args);
    storage().nodes[result.node_].kind = DbNodeKind::kGreatest;
    return result;
}
DbQuery::Expr DbQuery::least(std::span<const Expr> args) {
    if (args.empty()) {
        throw std::invalid_argument("LEAST requires an argument");
    }
    auto result = call("least", args);
    storage().nodes[result.node_].kind = DbNodeKind::kLeast;
    return result;
}
DbQuery::Expr DbQuery::binary(Expr lhs, DbBinaryOperator op, Expr rhs) {
    auto& s = storage();
    const auto left = requireExpression(lhs), right = requireExpression(rhs);
    detail::DbQueryNode node(DbNodeKind::kBinary, s.resource);
    node.left = left;
    node.right = right;
    node.binary = op;
    s.nodes.push_back(std::move(node));
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::unary(DbUnaryOperator op, Expr value) {
    auto& s = storage();
    const auto child = requireExpression(value);
    detail::DbQueryNode node(DbNodeKind::kUnary, s.resource);
    node.left = child;
    node.unary = op;
    s.nodes.push_back(std::move(node));
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::between(Expr value, Expr lower, Expr upper, bool negate) {
    auto& s = storage();
    detail::DbQueryNode node(DbNodeKind::kBetween, s.resource);
    node.args = {requireExpression(value), requireExpression(lower), requireExpression(upper)};
    node.flag = negate;
    s.nodes.push_back(std::move(node));
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::tuple(std::span<const Expr> values) {
    auto& s = storage();
    detail::DbQueryNode node(DbNodeKind::kTuple, s.resource);
    for (const auto value : values) {
        node.args.push_back(requireExpression(value));
    }
    s.nodes.push_back(std::move(node));
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::list(std::span<const Expr> values) {
    auto result = tuple(values);
    storage().nodes[result.node_].kind = DbNodeKind::kList;
    return result;
}
DbQuery::Expr DbQuery::array(std::span<const Expr> values) {
    auto result = tuple(values);
    storage().nodes[result.node_].kind = DbNodeKind::kArray;
    return result;
}
DbQuery::Expr DbQuery::any(Expr array) {
    auto result = unary(DbUnaryOperator::kNot, array);
    storage().nodes[result.node_].kind = DbNodeKind::kAny;
    return result;
}
DbQuery::Expr DbQuery::all(Expr array) {
    auto result = any(array);
    storage().nodes[result.node_].kind = DbNodeKind::kAll;
    return result;
}
DbQuery::Expr DbQuery::cast(Expr value, const DbTypeDefinition& type) {
    const auto child = requireExpression(value);
    auto& s = storage();
    detail::DbQueryNode node(DbNodeKind::kCast, s.resource);
    node.left = child;
    node.type.dataType = type.dataType;
    node.type.customName = type.customName;
    node.type.length = type.length;
    node.type.precision = type.precision;
    node.type.scale = type.scale;
    node.type.array = type.array;
    s.nodes.push_back(std::move(node));
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::alias(Expr value, std::string_view name) {
    detail::requireName(name);
    const auto child = requireExpression(value);
    auto& s = storage();
    detail::DbQueryNode node(DbNodeKind::kAlias, s.resource);
    node.left = child;
    node.text = name;
    s.nodes.push_back(std::move(node));
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::caseWhen(std::span<const DbCaseBranch> branches, Expr otherwise) {
    if (branches.empty()) {
        throw std::invalid_argument("CASE requires a branch");
    }
    auto& s = storage();
    detail::DbQueryNode node(DbNodeKind::kCase, s.resource);
    for (const auto& branch : branches) {
        node.args.push_back(requireExpression(branch.when));
        node.args.push_back(requireExpression(branch.then));
    }
    if (!otherwise.empty()) {
        node.left = requireExpression(otherwise);
    }
    s.nodes.push_back(std::move(node));
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::subquery(const DbQuery& query) {
    query.requireSelectQuery();
    auto snapshot = query.clone(resource());
    auto& s = storage();
    const auto index = s.queries.size();
    s.queries.push_back(std::move(snapshot));
    detail::DbQueryNode node(DbNodeKind::kSubquery, s.resource);
    node.query = index;
    s.nodes.push_back(std::move(node));
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::exists(const DbQuery& query) {
    auto result = subquery(query);
    storage().nodes[result.node_].kind = DbNodeKind::kExists;
    return result;
}
DbQuery::Expr DbQuery::aggregate(std::string_view function, std::span<const Expr> args, bool distinct, std::span<const DbOrderTerm> order) {
    auto result = call(function, args);
    auto& node = storage().nodes[result.node_];
    node.kind = DbNodeKind::kAggregate;
    node.flag = distinct;
    for (const auto& term : order) {
        node.orders.push_back({requireExpression(term.expression), term.direction, term.nulls});
    }
    return result;
}
DbQuery::Expr DbQuery::filter(Expr aggregate, Expr predicate) {
    auto result = binary(aggregate, DbBinaryOperator::kAnd, predicate);
    storage().nodes[result.node_].kind = DbNodeKind::kFilter;
    return result;
}
DbQuery::Expr DbQuery::over(Expr function, const DbWindowOptions& window) {
    auto& s = storage();
    detail::DbQueryNode node(DbNodeKind::kWindow, s.resource);
    node.left = requireExpression(function);
    for (const auto expression : window.partitionBy) {
        node.args.push_back(requireExpression(expression));
    }
    for (const auto& term : window.orderBy) {
        node.orders.push_back({requireExpression(term.expression), term.direction, term.nulls});
    }
    node.frame = window.frame;
    s.nodes.push_back(std::move(node));
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::withinGroup(Expr function, std::span<const DbOrderTerm> order) {
    if (order.empty()) {
        throw std::invalid_argument("WITHIN GROUP requires ordering");
    }
    auto& s = storage();
    detail::DbQueryNode node(DbNodeKind::kWithinGroup, s.resource);
    node.left = requireExpression(function);
    for (const auto& term : order) {
        node.orders.push_back({requireExpression(term.expression), term.direction, term.nulls});
    }
    s.nodes.push_back(std::move(node));
    return Expr(&s, s.nodes.size() - 1);
}
DbQuery::Expr DbQuery::extract(DbDatePart part, Expr value) {
    auto result = unary(DbUnaryOperator::kNot, value);
    auto& node = storage().nodes[result.node_];
    node.kind = DbNodeKind::kExtract;
    node.datePart = part;
    return result;
}
DbQuery::Expr DbQuery::subscript(Expr array, Expr index) {
    auto result = binary(array, DbBinaryOperator::kAdd, index);
    storage().nodes[result.node_].kind = DbNodeKind::kSubscript;
    return result;
}
DbQuery::Expr DbQuery::collate(Expr value, std::string_view collation) {
    auto result = alias(value, collation);
    storage().nodes[result.node_].kind = DbNodeKind::kCollate;
    return result;
}

DbQuery::Expr DbQuery::importExpression(Expr expression, std::string_view sourceQualifier, std::string_view targetQualifier) {
    if (expression.empty() || expression.node_ >= expression.owner_->nodes.size()) {
        throw std::invalid_argument("cannot import an empty database expression");
    }
    auto& target = storage();
    if (sourceQualifier.empty() != targetQualifier.empty()) {
        throw std::invalid_argument("expression qualifier mapping requires both source and target");
    }
    if (!sourceQualifier.empty()) {
        detail::requireName(sourceQualifier, true);
        detail::requireName(targetQualifier, true);
    }
    if (expression.owner_ == &target && sourceQualifier.empty()) {
        return expression;
    }
    const auto& source = *expression.owner_;
    std::pmr::vector<std::size_t> mapped(source.nodes.size(), noDbNode, target.resource);
    auto import = [&](auto&& self, std::size_t id, std::size_t depth) -> std::size_t {
        if (depth > 256) {
            throw std::length_error("database expression nesting exceeds 256 levels");
        }
        if (mapped[id] != noDbNode) {
            return mapped[id];
        }
        detail::DbQueryNode node(source.nodes[id], target.resource);
        if (!sourceQualifier.empty() && (node.kind == DbNodeKind::kColumn || node.kind == DbNodeKind::kStar) && node.qualifier == sourceQualifier) {
            node.qualifier = targetQualifier;
        }
        if (node.left != noDbNode) {
            node.left = self(self, node.left, depth + 1);
        }
        if (node.right != noDbNode) {
            node.right = self(self, node.right, depth + 1);
        }
        for (auto& arg : node.args) {
            arg = self(self, arg, depth + 1);
        }
        for (auto& arg : node.named) {
            arg.expression = self(self, arg.expression, depth + 1);
        }
        for (auto& order : node.orders) {
            order.expression = self(self, order.expression, depth + 1);
        }
        if (node.query != noDbNode) {
            auto snapshot = source.queries[node.query].clone(target.resource);
            node.query = target.queries.size();
            target.queries.push_back(std::move(snapshot));
        }
        const auto index = target.nodes.size();
        target.nodes.push_back(std::move(node));
        mapped[id] = index;
        return index;
    };
    return Expr(&target, import(import, expression.node_, 0));
}

DbQuery& DbQuery::select(std::span<const Expr> projections) {
    auto& s = storage();
    std::pmr::vector<std::size_t> owned(s.resource);
    for (const auto expression : projections) {
        owned.push_back(requireExpression(expression));
    }
    s.projections = std::move(owned);
    return *this;
}
DbQuery& DbQuery::addSelect(Expr projection) {
    storage().projections.push_back(requireExpression(projection));
    return *this;
}
DbQuery& DbQuery::from(std::string_view table, std::string_view alias) {
    detail::requireName(table, true);
    if (!alias.empty()) {
        detail::requireName(alias);
    }
    auto& s = storage();
    detail::DbQuerySource source(s.resource);
    source.name = table;
    source.alias = alias;
    s.source = std::move(source);
    return *this;
}
DbQuery& DbQuery::from(const DbQuery& query, std::string_view alias, const DbSourceOptions& options) {
    detail::requireName(alias);
    query.requireSelectQuery();
    auto snapshot = query.clone(resource());
    auto& s = storage();
    detail::DbQuerySource source(s.resource);
    source.kind = DbSourceKind::kQuery;
    source.alias = alias;
    source.query = s.queries.size();
    detail::setSourceOptions(source, options, s.resource);
    s.queries.push_back(std::move(snapshot));
    s.source = std::move(source);
    return *this;
}
DbQuery& DbQuery::fromFunction(Expr function, std::string_view alias, const DbSourceOptions& options) {
    detail::requireName(alias);
    const auto index = requireExpression(function);
    auto& s = storage();
    detail::DbQuerySource source(s.resource);
    source.kind = DbSourceKind::kFunction;
    source.expression = index;
    source.alias = alias;
    detail::setSourceOptions(source, options, s.resource);
    s.source = std::move(source);
    return *this;
}
DbQuery& DbQuery::join(DbJoinType type, std::string_view table, Expr on, std::string_view alias, std::span<const std::string_view> usingColumns) {
    detail::requireName(table, true);
    if (!alias.empty()) {
        detail::requireName(alias);
    }
    auto& s = storage();
    detail::DbQuerySource source(s.resource);
    source.name = table;
    source.alias = alias;
    detail::DbStoredJoin join(type, std::move(source), on.empty() ? noDbNode : requireExpression(on), s.resource);
    for (const auto name : usingColumns) {
        detail::requireName(name);
        join.usingColumns.emplace_back(name);
    }
    s.joins.push_back(std::move(join));
    return *this;
}
DbQuery& DbQuery::join(DbJoinType type, const DbQuery& query, Expr on, std::string_view alias, const DbSourceOptions& options) {
    detail::requireName(alias);
    const auto predicate = on.empty() ? noDbNode : requireExpression(on);
    query.requireSelectQuery();
    auto snapshot = query.clone(resource());
    auto& s = storage();
    detail::DbQuerySource source(s.resource);
    source.kind = DbSourceKind::kQuery;
    source.query = s.queries.size();
    source.alias = alias;
    detail::setSourceOptions(source, options, s.resource);
    s.queries.push_back(std::move(snapshot));
    s.joins.emplace_back(type, std::move(source), predicate, s.resource);
    return *this;
}
DbQuery& DbQuery::joinFunction(DbJoinType type, Expr function, Expr on, std::string_view alias, const DbSourceOptions& options) {
    detail::requireName(alias);
    const auto functionId = requireExpression(function);
    const auto predicate = on.empty() ? noDbNode : requireExpression(on);
    auto& s = storage();
    detail::DbQuerySource source(s.resource);
    source.kind = DbSourceKind::kFunction;
    source.expression = functionId;
    source.alias = alias;
    detail::setSourceOptions(source, options, s.resource);
    s.joins.emplace_back(type, std::move(source), predicate, s.resource);
    return *this;
}
DbQuery& DbQuery::where(Expr predicate) {
    storage().predicate = requireExpression(predicate);
    return *this;
}
DbQuery& DbQuery::andWhere(Expr predicate) {
    const auto previous = storage().predicate;
    return where(previous == noDbNode ? predicate : binary(Expr(storage_.get(), previous), DbBinaryOperator::kAnd, predicate));
}
DbQuery& DbQuery::orWhere(Expr predicate) {
    const auto previous = storage().predicate;
    return where(previous == noDbNode ? predicate : binary(Expr(storage_.get(), previous), DbBinaryOperator::kOr, predicate));
}
DbQuery& DbQuery::groupBy(std::span<const Expr> expressions) {
    std::pmr::vector<std::size_t> groups(resource());
    for (const auto expression : expressions) {
        groups.push_back(requireExpression(expression));
    }
    storage().groups = std::move(groups);
    return *this;
}
DbQuery& DbQuery::addGroupBy(Expr expression) {
    storage().groups.push_back(requireExpression(expression));
    return *this;
}
DbQuery& DbQuery::having(Expr predicate) {
    storage().having = requireExpression(predicate);
    return *this;
}
DbQuery& DbQuery::andHaving(Expr predicate) {
    const auto previous = storage().having;
    return having(previous == noDbNode ? predicate : binary(Expr(storage_.get(), previous), DbBinaryOperator::kAnd, predicate));
}
DbQuery& DbQuery::orderBy(Expr expression, DbOrderDirection direction, DbNullsOrder nulls) {
    const auto index = requireExpression(expression);
    auto& s = storage();
    s.orders.clear();
    s.orders.push_back({index, direction, nulls});
    return *this;
}
DbQuery& DbQuery::addOrderBy(Expr expression, DbOrderDirection direction, DbNullsOrder nulls) {
    storage().orders.push_back({requireExpression(expression), direction, nulls});
    return *this;
}
DbQuery& DbQuery::clearOrder() {
    storage().orders.clear();
    return *this;
}
DbQuery& DbQuery::limit(std::optional<std::uint64_t> count) {
    if (count && *count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("database limit exceeds signed 64-bit range");
    }
    storage().limit = count;
    return *this;
}
DbQuery& DbQuery::offset(std::optional<std::uint64_t> count) {
    if (count && *count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("database offset exceeds signed 64-bit range");
    }
    storage().offset = count;
    return *this;
}
DbQuery& DbQuery::distinct(bool enabled) {
    auto& s = storage();
    s.distinct = enabled;
    s.distinctOn.clear();
    return *this;
}
DbQuery& DbQuery::distinctOn(std::span<const Expr> expressions) {
    if (expressions.empty()) {
        throw std::invalid_argument("DISTINCT ON requires an expression");
    }
    std::pmr::vector<std::size_t> columns(resource());
    for (const auto expression : expressions) {
        columns.push_back(requireExpression(expression));
    }
    auto& s = storage();
    s.distinctOn = std::move(columns);
    s.distinct = false;
    return *this;
}
DbQuery& DbQuery::returning(std::span<const Expr> expressions) {
    std::pmr::vector<std::size_t> columns(resource());
    for (const auto expression : expressions) {
        columns.push_back(requireExpression(expression));
    }
    storage().returning = std::move(columns);
    return *this;
}
DbQuery& DbQuery::insertInto(std::string_view table, std::span<const std::string_view> columns, std::string_view alias) {
    detail::requireName(table, true);
    if (!alias.empty()) {
        detail::requireName(alias);
    }
    auto& s = storage();
    std::pmr::vector<std::pmr::string> names(s.resource);
    for (const auto column : columns) {
        detail::requireName(column);
        if (std::ranges::any_of(names, [&](const auto& name) { return name == column; })) {
            throw std::invalid_argument("duplicate INSERT column");
        }
        names.emplace_back(column);
    }
    s.kind = DbQueryKind::kInsert;
    s.target = table;
    s.targetAlias = alias;
    s.columns = std::move(names);
    return *this;
}
DbQuery& DbQuery::values(std::span<const Expr> row) {
    auto& s = storage();
    if (s.kind != DbQueryKind::kSelect && s.kind != DbQueryKind::kValues && s.kind != DbQueryKind::kInsert) {
        throw std::invalid_argument("VALUES requires an INSERT or a values query");
    }
    std::pmr::vector<std::size_t> values(s.resource);
    for (const auto expression : row) {
        values.push_back(requireExpression(expression));
    }
    if (s.kind != DbQueryKind::kInsert) {
        s.kind = DbQueryKind::kValues;
    }
    s.rows.push_back(std::move(values));
    return *this;
}
DbQuery& DbQuery::insertFrom(const DbQuery& query) {
    if (storage().kind != DbQueryKind::kInsert) {
        throw std::invalid_argument("INSERT SELECT requires an INSERT target");
    }
    query.requireSelectQuery();
    auto snapshot = query.clone(resource());
    auto& s = storage();
    const auto index = s.queries.size();
    s.queries.push_back(std::move(snapshot));
    s.insertQuery = index;
    return *this;
}
DbQuery& DbQuery::onConflict(const DbConflictOptions& conflict) {
    auto& s = storage();
    if (s.kind != DbQueryKind::kInsert) {
        throw std::invalid_argument("ON CONFLICT requires an INSERT");
    }
    detail::DbStoredConflict owned(s.resource);
    for (const auto& name : conflict.columns) {
        detail::requireName(name);
        owned.columns.emplace_back(name);
    }
    if (!conflict.constraint.empty()) {
        detail::requireName(conflict.constraint);
        owned.constraint = conflict.constraint;
    }
    owned.doNothing = conflict.doNothing;
    owned.anyUniqueKey = conflict.anyUniqueKey;
    if (!conflict.targetWhere.empty()) {
        owned.targetWhere = requireExpression(conflict.targetWhere);
    }
    if (!conflict.updateWhere.empty()) {
        owned.updateWhere = requireExpression(conflict.updateWhere);
    }
    for (const auto& assignment : conflict.update) {
        detail::requireName(assignment.column);
        if (std::ranges::any_of(owned.assignments, [&](const auto& existing) { return std::string_view(existing.column) == assignment.column; })) {
            throw std::invalid_argument("duplicate conflict update column");
        }
        owned.assignments.emplace_back(assignment.column, requireExpression(assignment.value), s.resource);
    }
    s.conflict = std::move(owned);
    return *this;
}
DbQuery& DbQuery::update(std::string_view table, std::string_view alias) {
    detail::requireName(table, true);
    if (!alias.empty()) {
        detail::requireName(alias);
    }
    auto& s = storage();
    s.kind = DbQueryKind::kUpdate;
    s.target = table;
    s.targetAlias = alias;
    return *this;
}
DbQuery& DbQuery::set(std::string_view column, Expr value) {
    detail::requireName(column);
    const auto expression = requireExpression(value);
    auto& s = storage();
    if (s.kind != DbQueryKind::kUpdate) {
        throw std::invalid_argument("SET requires an UPDATE");
    }
    for (auto& existing : s.assignments) {
        if (existing.column == column) {
            existing.expression = expression;
            return *this;
        }
    }
    s.assignments.emplace_back(column, expression, s.resource);
    return *this;
}
DbQuery& DbQuery::updateFrom(std::string_view table, std::string_view alias) {
    if (storage().kind != DbQueryKind::kUpdate) {
        throw std::invalid_argument("UPDATE FROM requires an UPDATE");
    }
    return from(table, alias);
}
DbQuery& DbQuery::updateFrom(const DbQuery& query, std::string_view alias) {
    if (storage().kind != DbQueryKind::kUpdate) {
        throw std::invalid_argument("UPDATE FROM requires an UPDATE");
    }
    return from(query, alias);
}
DbQuery& DbQuery::deleteFrom(std::string_view table, std::string_view alias) {
    detail::requireName(table, true);
    if (!alias.empty()) {
        detail::requireName(alias);
    }
    auto& s = storage();
    s.kind = DbQueryKind::kDelete;
    s.target = table;
    s.targetAlias = alias;
    return *this;
}
DbQuery& DbQuery::deleteUsing(std::string_view table, std::string_view alias) {
    if (storage().kind != DbQueryKind::kDelete) {
        throw std::invalid_argument("DELETE USING requires a DELETE");
    }
    return from(table, alias);
}
DbQuery& DbQuery::deleteUsing(const DbQuery& query, std::string_view alias) {
    if (storage().kind != DbQueryKind::kDelete) {
        throw std::invalid_argument("DELETE USING requires a DELETE");
    }
    return from(query, alias);
}
DbQuery& DbQuery::lock(const DbLockOptions& options) {
    if (options.nowait && options.skipLocked) {
        throw std::invalid_argument("NOWAIT and SKIP LOCKED are mutually exclusive");
    }
    for (const auto& table : options.tables) {
        detail::requireName(table);
    }
    storage().lock.emplace(options, resource());
    return *this;
}
DbQuery& DbQuery::clearLock() {
    storage().lock.reset();
    return *this;
}
DbQuery& DbQuery::with(std::string_view name, const DbQuery& query, const DbCteOptions& options) {
    detail::requireName(name);
    auto& s = storage();
    if (std::ranges::any_of(s.ctes, [&](const auto& existing) { return existing.name == name; })) {
        throw std::invalid_argument("duplicate CTE name");
    }
    for (const auto& column : options.columns) {
        detail::requireName(column);
    }
    auto snapshot = query.clone(resource());
    const auto index = s.queries.size();
    s.queries.push_back(std::move(snapshot));
    s.ctes.emplace_back(name, index, options, s.resource);
    return *this;
}
DbQuery& DbQuery::combine(DbSetOperation operation, const DbQuery& query) {
    auto& s = storage();
    if ((s.kind != DbQueryKind::kSelect && s.kind != DbQueryKind::kValues) ||
        (query.storage().kind != DbQueryKind::kSelect && query.storage().kind != DbQueryKind::kValues)) {
        throw std::invalid_argument("set operations require SELECT or VALUES queries");
    }
    auto snapshot = query.clone(resource());
    const auto index = s.queries.size();
    s.queries.push_back(std::move(snapshot));
    s.setOperations.push_back({operation, index});
    return *this;
}

}  // namespace ruvia
