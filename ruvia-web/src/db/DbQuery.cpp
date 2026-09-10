#include "ruvia/web/db/DbQuery.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/detail/db/DbSqlFormat.h"

namespace ruvia::detail {

constexpr std::size_t noDbNode = std::numeric_limits<std::size_t>::max();

enum class DbNodeKind : std::uint8_t {
    kColumn,
    kStar,
    kValue,
    kDefault,
    kExcluded,
    kFunction,
    kCoalesce,
    kNullIf,
    kGreatest,
    kLeast,
    kBinary,
    kUnary,
    kBetween,
    kTuple,
    kList,
    kArray,
    kAny,
    kAll,
    kCast,
    kAlias,
    kCase,
    kExists,
    kSubquery,
    kAggregate,
    kFilter,
    kWindow,
    kWithinGroup,
    kExtract,
    kSubscript,
    kCollate,
};
enum class DbQueryKind : std::uint8_t { kSelect,
    kValues,
    kInsert,
    kUpdate,
    kDelete };
enum class DbSourceKind : std::uint8_t { kTable,
    kQuery,
    kFunction };

struct DbStoredType final {
    explicit DbStoredType(std::pmr::memory_resource* resource)
        : customName(resource) {}
    DbStoredType(const DbStoredType& source, std::pmr::memory_resource* resource)
        : dataType(source.dataType),
          customName(source.customName, resource),
          length(source.length),
          precision(source.precision),
          scale(source.scale),
          array(source.array) {}
    DbStoredType(const DbTypeDefinition& source, std::pmr::memory_resource* resource)
        : dataType(source.dataType),
          customName(source.customName, resource),
          length(source.length),
          precision(source.precision),
          scale(source.scale),
          array(source.array) {}
    DbDataType dataType{DbDataType::kInferred};
    std::pmr::string customName;
    std::size_t length{0};
    unsigned precision{0};
    unsigned scale{0};
    bool array{false};
};
struct DbStoredOrder final {
    std::size_t expression{noDbNode};
    DbOrderDirection direction{DbOrderDirection::kAsc};
    DbNullsOrder nulls{DbNullsOrder::kDefault};
};
struct DbStoredNamedArgument final {
    DbStoredNamedArgument(std::string_view name, std::size_t expression, std::pmr::memory_resource* resource)
        : name(name, resource),
          expression(expression) {}
    std::pmr::string name;
    std::size_t expression;
};

struct DbQueryNode final {
    DbQueryNode(DbNodeKind kind, std::pmr::memory_resource* resource)
        : kind(kind),
          value(nullptr),
          text(resource),
          qualifier(resource),
          args(resource),
          named(resource),
          orders(resource),
          type(resource) {}
    DbQueryNode(const DbQueryNode& source, std::pmr::memory_resource* resource)
        : kind(source.kind),
          value(cloneDbValueForResource(source.value, resource)),
          text(source.text, resource),
          qualifier(source.qualifier, resource),
          args(source.args, resource),
          named(resource),
          orders(source.orders, resource),
          type(source.type, resource),
          binary(source.binary),
          unary(source.unary),
          datePart(source.datePart),
          left(source.left),
          right(source.right),
          query(source.query),
          flag(source.flag),
          frame(source.frame) {
        named.reserve(source.named.size());
        for (const auto& argument : source.named) {
            named.emplace_back(argument.name, argument.expression, resource);
        }
    }
    DbQueryNode(DbQueryNode&&) noexcept = default;
    DbQueryNode& operator=(DbQueryNode&&) = delete;
    DbNodeKind kind;
    DbValue value;
    std::pmr::string text;
    std::pmr::string qualifier;
    std::pmr::vector<std::size_t> args;
    std::pmr::vector<DbStoredNamedArgument> named;
    std::pmr::vector<DbStoredOrder> orders;
    DbStoredType type;
    DbBinaryOperator binary{DbBinaryOperator::kEqual};
    DbUnaryOperator unary{DbUnaryOperator::kNot};
    DbDatePart datePart{DbDatePart::kEpoch};
    std::size_t left{noDbNode};
    std::size_t right{noDbNode};
    std::size_t query{noDbNode};
    bool flag{false};
    std::optional<DbWindowFrameOptions> frame{};
};
struct DbStoredSourceColumn final {
    DbStoredSourceColumn(const DbSourceColumn& source, std::pmr::memory_resource* resource)
        : name(source.name, resource),
          type(source.type, resource) {}
    DbStoredSourceColumn(const DbStoredSourceColumn& source, std::pmr::memory_resource* resource)
        : name(source.name, resource),
          type(source.type, resource) {}
    std::pmr::string name;
    DbStoredType type;
};
struct DbQuerySource final {
    explicit DbQuerySource(std::pmr::memory_resource* resource)
        : name(resource),
          alias(resource),
          columns(resource) {}
    DbQuerySource(const DbQuerySource& source, std::pmr::memory_resource* resource)
        : kind(source.kind),
          name(source.name, resource),
          alias(source.alias, resource),
          expression(source.expression),
          query(source.query),
          lateral(source.lateral),
          ordinality(source.ordinality),
          columns(resource) {
        for (const auto& column : source.columns) {
            columns.emplace_back(column, resource);
        }
    }
    DbSourceKind kind{DbSourceKind::kTable};
    std::pmr::string name;
    std::pmr::string alias;
    std::size_t expression{noDbNode};
    std::size_t query{noDbNode};
    bool lateral{false};
    bool ordinality{false};
    std::pmr::vector<DbStoredSourceColumn> columns;
};
struct DbStoredJoin final {
    DbStoredJoin(DbJoinType type, DbQuerySource source, std::size_t on, std::pmr::memory_resource* resource)
        : type(type),
          source(std::move(source)),
          on(on),
          usingColumns(resource) {}
    DbStoredJoin(const DbStoredJoin& source, std::pmr::memory_resource* resource)
        : type(source.type),
          source(source.source, resource),
          on(source.on),
          usingColumns(source.usingColumns, resource) {}
    DbJoinType type;
    DbQuerySource source;
    std::size_t on{noDbNode};
    std::pmr::vector<std::pmr::string> usingColumns;
};
struct DbStoredAssignment final {
    DbStoredAssignment(std::string_view column, std::size_t expression, std::pmr::memory_resource* resource)
        : column(column, resource),
          expression(expression) {}
    std::pmr::string column;
    std::size_t expression;
};
struct DbStoredCte final {
    DbStoredCte(std::string_view name, std::size_t query, const DbCteOptions& options, std::pmr::memory_resource* resource)
        : name(name, resource),
          query(query),
          recursive(options.recursive),
          materialization(options.materialization),
          columns(resource) {
        for (const auto& column : options.columns) {
            columns.emplace_back(column);
        }
    }
    DbStoredCte(const DbStoredCte& source, std::pmr::memory_resource* resource)
        : name(source.name, resource),
          query(source.query),
          recursive(source.recursive),
          materialization(source.materialization),
          columns(source.columns, resource) {}
    std::pmr::string name;
    std::size_t query;
    bool recursive{false};
    DbMaterialization materialization{DbMaterialization::kDefault};
    std::pmr::vector<std::pmr::string> columns;
};
struct DbStoredSetOperation final {
    DbSetOperation operation;
    std::size_t query;
};
struct DbStoredConflict final {
    explicit DbStoredConflict(std::pmr::memory_resource* resource)
        : columns(resource),
          constraint(resource),
          assignments(resource) {}
    DbStoredConflict(const DbStoredConflict& source, std::pmr::memory_resource* resource)
        : columns(source.columns, resource),
          constraint(source.constraint, resource),
          targetWhere(source.targetWhere),
          assignments(resource),
          updateWhere(source.updateWhere),
          doNothing(source.doNothing),
          anyUniqueKey(source.anyUniqueKey) {
        for (const auto& assignment : source.assignments) {
            assignments.emplace_back(assignment.column, assignment.expression, resource);
        }
    }
    std::pmr::vector<std::pmr::string> columns;
    std::pmr::string constraint;
    std::size_t targetWhere{noDbNode};
    std::pmr::vector<DbStoredAssignment> assignments;
    std::size_t updateWhere{noDbNode};
    bool doNothing{false};
    bool anyUniqueKey{false};
};
struct DbStoredLock final {
    DbStoredLock(const DbLockOptions& source, std::pmr::memory_resource* resource)
        : mode(source.mode),
          nowait(source.nowait),
          skipLocked(source.skipLocked),
          tables(resource) {
        for (const auto& table : source.tables) {
            tables.emplace_back(table);
        }
    }
    DbStoredLock(const DbStoredLock& source, std::pmr::memory_resource* resource)
        : mode(source.mode),
          nowait(source.nowait),
          skipLocked(source.skipLocked),
          tables(source.tables, resource) {}
    DbRowLock mode;
    bool nowait{false};
    bool skipLocked{false};
    std::pmr::vector<std::pmr::string> tables;
};

class DbQueryStorage final {
public:
    explicit DbQueryStorage(std::pmr::memory_resource* resource)
        : resource(resource),
          nodes(resource),
          queries(resource),
          target(resource),
          targetAlias(resource),
          columns(resource),
          projections(resource),
          groups(resource),
          orders(resource),
          distinctOn(resource),
          returning(resource),
          joins(resource),
          rows(resource),
          assignments(resource),
          ctes(resource),
          setOperations(resource),
          cacheId(resource) {}

    std::pmr::memory_resource* resource;
    std::pmr::vector<DbQueryNode> nodes;
    std::pmr::vector<DbQuery> queries;
    DbQueryKind kind{DbQueryKind::kSelect};
    std::pmr::string target;
    std::pmr::string targetAlias;
    std::pmr::vector<std::pmr::string> columns;
    std::pmr::vector<std::size_t> projections;
    std::pmr::vector<std::size_t> groups;
    std::pmr::vector<DbStoredOrder> orders;
    bool distinct{false};
    std::pmr::vector<std::size_t> distinctOn;
    std::pmr::vector<std::size_t> returning;
    std::optional<DbQuerySource> source{};
    std::pmr::vector<DbStoredJoin> joins;
    std::pmr::vector<std::pmr::vector<std::size_t>> rows;
    std::pmr::vector<DbStoredAssignment> assignments;
    std::size_t insertQuery{noDbNode};
    std::size_t predicate{noDbNode};
    std::size_t having{noDbNode};
    std::optional<std::uint64_t> limit{};
    std::optional<std::uint64_t> offset{};
    std::optional<DbStoredConflict> conflict{};
    std::optional<DbStoredLock> lock{};
    std::pmr::vector<DbStoredCte> ctes;
    std::pmr::vector<DbStoredSetOperation> setOperations;

    std::optional<bool> cacheEnabled{};
    std::optional<std::chrono::milliseconds> cacheDuration{};
    std::pmr::string cacheId;

    [[nodiscard]] bool returnsRows() const noexcept {
        return kind == DbQueryKind::kSelect || kind == DbQueryKind::kValues || !returning.empty();
    }
};

namespace {
void requireName(std::string_view name, bool qualified = false) {
    if (name.empty() || name.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("database identifier must be nonempty and contain no NUL");
    }
    if (qualified && (name.front() == '.' || name.back() == '.' || name.find("..") != std::string_view::npos)) {
        throw std::invalid_argument("database qualified identifier has an empty component");
    }
}
bool isMariaDbBareFunctionName(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    const auto isAsciiLetter = [](char value) noexcept {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
    };
    if (!isAsciiLetter(name.front()) && name.front() != '_') {
        return false;
    }
    return std::ranges::all_of(name.substr(1), [&](char value) noexcept {
        return isAsciiLetter(value) || (value >= '0' && value <= '9') || value == '_' || value == '$';
    });
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

namespace ruvia::detail {

class DbQueryCompiler final {
public:
    DbQueryCompiler(DbDriver driver, std::pmr::memory_resource* resource, DbParameterMode mode)
        : sql(resource),
          params(resource),
          driver_(driver),
          mode_(mode),
          parameterPositions_(resource) {
        requireDbDialect(driver);
    }

    std::pmr::string sql;
    std::pmr::vector<DbValue> params;

    void expression(DbExpression value) {
        if (value.empty()) {
            throw std::invalid_argument("empty database expression");
        }
        expr(*value.owner_, value.node_, 0);
    }
    void query(const DbQueryStorage& s, std::size_t depth = 0) {
        requireDepth(depth);
        validate(s);
        if (!s.ctes.empty()) {
            if (!pg() && s.kind != DbQueryKind::kSelect) {
                unsupported("data-changing WITH");
            }
            sql += "WITH ";
            if (std::ranges::any_of(s.ctes, [](const auto& cte) { return cte.recursive; })) {
                sql += "RECURSIVE ";
            }
            separated(s.ctes, [&](const auto& cte) {
                identifier(cte.name);
                if (!cte.columns.empty()) {
                    sql += " (";
                    identifiers(cte.columns);
                    sql += ')';
                }
                sql += " AS ";
                switch (cte.materialization) {
                    case DbMaterialization::kDefault:
                        break;
                    case DbMaterialization::kMaterialized:
                        requirePg("materialized CTE");
                        sql += "MATERIALIZED ";
                        break;
                    case DbMaterialization::kNotMaterialized:
                        requirePg("not-materialized CTE");
                        sql += "NOT MATERIALIZED ";
                        break;
                }
                const auto& child = nested(s, cte.query);
                if (child.kind != DbQueryKind::kSelect && child.kind != DbQueryKind::kValues) {
                    requirePg("data-changing CTE");
                    if (depth != 0) {
                        throw std::invalid_argument("data-changing CTE requires a top-level WITH statement");
                    }
                }
                sql += '(';
                query(child, depth + 1);
                sql += ')';
            });
            sql += ' ';
        }
        for (std::size_t i = 0; i < s.setOperations.size(); ++i) {
            sql += '(';
        }
        body(s, depth + 1);
        for (const auto& operation : s.setOperations) {
            sql += ')';
            switch (operation.operation) {
                case DbSetOperation::kUnion:
                    sql += " UNION ";
                    break;
                case DbSetOperation::kUnionAll:
                    sql += " UNION ALL ";
                    break;
                case DbSetOperation::kIntersect:
                    sql += " INTERSECT ";
                    break;
                case DbSetOperation::kIntersectAll:
                    requirePg("INTERSECT ALL");
                    sql += " INTERSECT ALL ";
                    break;
                case DbSetOperation::kExcept:
                    sql += " EXCEPT ";
                    break;
                case DbSetOperation::kExceptAll:
                    requirePg("EXCEPT ALL");
                    sql += " EXCEPT ALL ";
                    break;
            }
            sql += '(';
            query(nested(s, operation.query), depth + 1);
            sql += ')';
        }
        if (!s.orders.empty()) {
            sql += " ORDER BY ";
            orders(s, s.orders, depth + 1);
        }
        if (s.limit) {
            sql += " LIMIT ";
            number(*s.limit);
        } else if (s.offset && !pg()) {
            sql += " LIMIT 18446744073709551615";
        }
        if (s.offset) {
            sql += " OFFSET ";
            number(*s.offset);
        }
        if (s.lock) {
            const auto& lock = *s.lock;
            switch (lock.mode) {
                case DbRowLock::kUpdate:
                    sql += " FOR UPDATE";
                    break;
                case DbRowLock::kNoKeyUpdate:
                    requirePg("FOR NO KEY UPDATE");
                    sql += " FOR NO KEY UPDATE";
                    break;
                case DbRowLock::kShare:
                    sql += pg() ? " FOR SHARE" : " LOCK IN SHARE MODE";
                    break;
                case DbRowLock::kKeyShare:
                    requirePg("FOR KEY SHARE");
                    sql += " FOR KEY SHARE";
                    break;
            }
            if (!lock.tables.empty()) {
                requirePg("row lock OF");
                sql += " OF ";
                identifiers(lock.tables);
            }
            if (lock.nowait) {
                sql += " NOWAIT";
            }
            if (lock.skipLocked) {
                if (!pg() && lock.mode != DbRowLock::kUpdate) {
                    unsupported("SKIP LOCKED for a shared lock");
                }
                sql += " SKIP LOCKED";
            }
        }
        if (!s.returning.empty()) {
            requirePg("RETURNING");
            sql += " RETURNING ";
            expressions(s, s.returning, depth + 1, true);
        }
    }

private:
    DbDriver driver_;
    DbParameterMode mode_;
    std::pmr::unordered_map<const DbQueryNode*, std::size_t> parameterPositions_;
    bool pg() const noexcept {
        return driver_ == DbDriver::kPostgreSql;
    }
    [[noreturn]] static void unsupported(std::string_view feature) {
        throw std::invalid_argument(std::string(feature) + " is not supported by this database dialect");
    }
    void requirePg(std::string_view feature) const {
        if (!pg()) {
            unsupported(feature);
        }
    }
    static void requireDepth(std::size_t depth) {
        if (depth > 256) {
            throw std::length_error("database query nesting exceeds 256 levels");
        }
    }
    static const DbQueryStorage& nested(const DbQueryStorage& s, std::size_t index) {
        return s.queries.at(index).storage();
    }
    void identifier(std::string_view name) {
        appendDbIdentifier(sql, name, driver_);
    }
    void qualified(std::string_view name) {
        appendDbQualifiedIdentifier(sql, name, driver_);
    }
    void functionName(std::string_view name) {
        if (driver_ == DbDriver::kMariaDb && isMariaDbBareFunctionName(name)) {
            sql += name;
        } else {
            qualified(name);
        }
    }
    template <class Range, class Fn>
    void separated(const Range& range, Fn&& fn) {
        bool first = true;
        for (const auto& item : range) {
            if (!first) {
                sql += ", ";
            }
            first = false;
            fn(item);
        }
    }
    template <class Range>
    void identifiers(const Range& range) {
        separated(range, [&](const auto& name) { identifier(name); });
    }
    void bound(const DbValue& value, const DbQueryNode* node = nullptr) {
        if (mode_ == DbParameterMode::kLiteral || DbValueAccess::type(value) == DbValueType::kNull) {
            appendDbLiteral(sql, value, driver_);
            return;
        }
        if (pg() && node != nullptr) {
            if (const auto found = parameterPositions_.find(node); found != parameterPositions_.end()) {
                sql += '$';
                appendDbNumber(sql, static_cast<std::uint64_t>(found->second));
                return;
            }
        }
        params.push_back(cloneDbValueForResource(value, params.get_allocator().resource()));
        if (pg() && node != nullptr) {
            parameterPositions_.emplace(node, params.size());
        }
        if (pg()) {
            sql += '$';
            appendDbNumber(sql, static_cast<std::uint64_t>(params.size()));
        } else {
            sql += '?';
        }
    }
    void number(std::uint64_t value) {
        bound(DbValue(static_cast<std::int64_t>(value)));
    }
    template <class Range>
    void expressions(const DbQueryStorage& s, const Range& range, std::size_t depth, bool aliases = false) {
        separated(range, [&](std::size_t index) { expr(s, index, depth, aliases); });
    }
    void orders(const DbQueryStorage& s, const std::pmr::vector<DbStoredOrder>& terms, std::size_t depth) {
        separated(terms, [&](const auto& term) {
            if (!pg() && term.nulls != DbNullsOrder::kDefault) {
                sql += '(';
                expr(s, term.expression, depth);
                sql += " IS NULL)";
                sql += term.nulls == DbNullsOrder::kFirst ? " DESC, " : " ASC, ";
            }
            expr(s, term.expression, depth);
            sql += term.direction == DbOrderDirection::kAsc ? " ASC" : " DESC";
            if (pg()) {
                if (term.nulls == DbNullsOrder::kFirst) {
                    sql += " NULLS FIRST";
                }
                if (term.nulls == DbNullsOrder::kLast) {
                    sql += " NULLS LAST";
                }
            }
        });
    }
    void type(const DbStoredType& value) {
        appendDbTypeName(sql, value.dataType, value.customName, value.length, value.precision,
            value.scale, value.array, driver_, DbSqlTypeUsage::kCast);
    }
    void boundary(const DbWindowBoundary& boundary) {
        if (boundary.kind != DbFrameBoundary::kPreceding && boundary.kind != DbFrameBoundary::kFollowing && boundary.offset != 0) {
            throw std::invalid_argument("a non-offset window frame boundary has an offset");
        }
        switch (boundary.kind) {
            case DbFrameBoundary::kUnboundedPreceding:
                sql += "UNBOUNDED PRECEDING";
                break;
            case DbFrameBoundary::kPreceding:
                appendDbNumber(sql, boundary.offset);
                sql += " PRECEDING";
                break;
            case DbFrameBoundary::kCurrentRow:
                sql += "CURRENT ROW";
                break;
            case DbFrameBoundary::kFollowing:
                appendDbNumber(sql, boundary.offset);
                sql += " FOLLOWING";
                break;
            case DbFrameBoundary::kUnboundedFollowing:
                sql += "UNBOUNDED FOLLOWING";
                break;
        }
    }
    void expr(const DbQueryStorage& s, std::size_t index, std::size_t depth, bool aliases = false) {
        requireDepth(depth);
        const auto& n = s.nodes.at(index);
        const auto child = [&](std::size_t id) { expr(s, id, depth + 1); };
        switch (n.kind) {
            case DbNodeKind::kColumn:
            case DbNodeKind::kStar:
                if (!n.qualifier.empty()) {
                    qualified(n.qualifier);
                    sql += '.';
                }
                if (n.kind == DbNodeKind::kStar) {
                    sql += '*';
                } else {
                    identifier(n.text);
                }
                break;
            case DbNodeKind::kValue:
                bound(n.value, &n);
                break;
            case DbNodeKind::kDefault:
                sql += "DEFAULT";
                break;
            case DbNodeKind::kExcluded:
                if (pg()) {
                    sql += "excluded.";
                    identifier(n.text);
                } else {
                    sql += "VALUES(";
                    identifier(n.text);
                    sql += ')';
                }
                break;
            case DbNodeKind::kFunction:
            case DbNodeKind::kAggregate:
            case DbNodeKind::kCoalesce:
            case DbNodeKind::kNullIf:
            case DbNodeKind::kGreatest:
            case DbNodeKind::kLeast: {
                switch (n.kind) {
                    case DbNodeKind::kCoalesce:
                        sql += "COALESCE";
                        break;
                    case DbNodeKind::kNullIf:
                        sql += "NULLIF";
                        break;
                    case DbNodeKind::kGreatest:
                        sql += "GREATEST";
                        break;
                    case DbNodeKind::kLeast:
                        sql += "LEAST";
                        break;
                    default:
                        functionName(n.text);
                        break;
                }
                sql += '(';
                if (n.kind == DbNodeKind::kAggregate && n.flag) {
                    sql += "DISTINCT ";
                }
                expressions(s, n.args, depth + 1);
                if (!n.named.empty()) {
                    requirePg("named function arguments");
                    if (!n.args.empty()) {
                        sql += ", ";
                    }
                    separated(n.named, [&](const auto& arg) { identifier(arg.name); sql += " => "; child(arg.expression); });
                }
                if (!n.orders.empty()) {
                    requirePg("ordered aggregate");
                    sql += " ORDER BY ";
                    orders(s, n.orders, depth + 1);
                }
                sql += ')';
                break;
            }
            case DbNodeKind::kBinary:
                binary(s, n, depth + 1);
                break;
            case DbNodeKind::kUnary: {
                sql += '(';
                switch (n.unary) {
                    case DbUnaryOperator::kNot:
                        sql += "NOT ";
                        child(n.left);
                        break;
                    case DbUnaryOperator::kNegate:
                        sql += '-';
                        child(n.left);
                        break;
                    case DbUnaryOperator::kBitNot:
                        sql += '~';
                        child(n.left);
                        break;
                    default:
                        child(n.left);
                        switch (n.unary) {
                            case DbUnaryOperator::kIsNull:
                                sql += " IS NULL";
                                break;
                            case DbUnaryOperator::kIsNotNull:
                                sql += " IS NOT NULL";
                                break;
                            case DbUnaryOperator::kIsTrue:
                                sql += " IS TRUE";
                                break;
                            case DbUnaryOperator::kIsFalse:
                                sql += " IS FALSE";
                                break;
                            case DbUnaryOperator::kIsNotTrue:
                                sql += " IS NOT TRUE";
                                break;
                            case DbUnaryOperator::kIsNotFalse:
                                sql += " IS NOT FALSE";
                                break;
                            default:
                                std::terminate();
                        }
                }
                sql += ')';
                break;
            }
            case DbNodeKind::kBetween:
                sql += '(';
                child(n.args[0]);
                sql += n.flag ? " NOT BETWEEN " : " BETWEEN ";
                child(n.args[1]);
                sql += " AND ";
                child(n.args[2]);
                sql += ')';
                break;
            case DbNodeKind::kTuple:
            case DbNodeKind::kList:
            case DbNodeKind::kArray:
                if (n.kind == DbNodeKind::kArray) {
                    requirePg("SQL arrays");
                    sql += "ARRAY[";
                } else {
                    if (n.args.empty()) {
                        throw std::invalid_argument("empty SQL tuple or list");
                    }
                    sql += n.kind == DbNodeKind::kTuple ? "ROW(" : "(";
                }
                expressions(s, n.args, depth + 1);
                sql += n.kind == DbNodeKind::kArray ? ']' : ')';
                break;
            case DbNodeKind::kAny:
            case DbNodeKind::kAll:
                requirePg("array ANY/ALL");
                sql += n.kind == DbNodeKind::kAny ? "ANY(" : "ALL(";
                child(n.left);
                sql += ')';
                break;
            case DbNodeKind::kCast:
                sql += "CAST(";
                child(n.left);
                sql += " AS ";
                type(n.type);
                sql += ')';
                break;
            case DbNodeKind::kAlias:
                if (!aliases) {
                    throw std::invalid_argument("an alias is only valid in a projection or RETURNING");
                }
                child(n.left);
                sql += " AS ";
                identifier(n.text);
                break;
            case DbNodeKind::kCase:
                sql += "CASE";
                for (std::size_t i = 0; i < n.args.size(); i += 2) {
                    sql += " WHEN ";
                    child(n.args[i]);
                    sql += " THEN ";
                    child(n.args[i + 1]);
                }
                if (n.left != noDbNode) {
                    sql += " ELSE ";
                    child(n.left);
                }
                sql += " END";
                break;
            case DbNodeKind::kExists:
            case DbNodeKind::kSubquery:
                if (n.kind == DbNodeKind::kExists) {
                    sql += "EXISTS ";
                }
                sql += '(';
                query(nested(s, n.query), depth + 1);
                sql += ')';
                break;
            case DbNodeKind::kFilter:
                requirePg("aggregate FILTER");
                child(n.left);
                sql += " FILTER (WHERE ";
                child(n.right);
                sql += ')';
                break;
            case DbNodeKind::kWindow: {
                child(n.left);
                sql += " OVER (";
                bool space = false;
                if (!n.args.empty()) {
                    sql += "PARTITION BY ";
                    expressions(s, n.args, depth + 1);
                    space = true;
                }
                if (!n.orders.empty()) {
                    if (space) {
                        sql += ' ';
                    }
                    sql += "ORDER BY ";
                    orders(s, n.orders, depth + 1);
                    space = true;
                }
                if (n.frame) {
                    if (space) {
                        sql += ' ';
                    }
                    const auto& frame = *n.frame;
                    if (frame.start.kind == DbFrameBoundary::kUnboundedFollowing || frame.end.kind == DbFrameBoundary::kUnboundedPreceding || frame.start.kind > frame.end.kind) {
                        throw std::invalid_argument("invalid window frame boundaries");
                    }
                    switch (frame.kind) {
                        case DbWindowFrame::kRows:
                            sql += "ROWS";
                            break;
                        case DbWindowFrame::kRange:
                            sql += "RANGE";
                            break;
                        case DbWindowFrame::kGroups:
                            requirePg("GROUPS window frame");
                            sql += "GROUPS";
                            break;
                    }
                    sql += " BETWEEN ";
                    boundary(frame.start);
                    sql += " AND ";
                    boundary(frame.end);
                }
                sql += ')';
                break;
            }
            case DbNodeKind::kWithinGroup:
                requirePg("ordered-set aggregate");
                child(n.left);
                sql += " WITHIN GROUP (ORDER BY ";
                orders(s, n.orders, depth + 1);
                sql += ')';
                break;
            case DbNodeKind::kExtract: {
                sql += "EXTRACT(";
                switch (n.datePart) {
                    case DbDatePart::kEpoch:
                        requirePg("EXTRACT EPOCH");
                        sql += "EPOCH";
                        break;
                    case DbDatePart::kYear:
                        sql += "YEAR";
                        break;
                    case DbDatePart::kMonth:
                        sql += "MONTH";
                        break;
                    case DbDatePart::kDay:
                        sql += "DAY";
                        break;
                    case DbDatePart::kHour:
                        sql += "HOUR";
                        break;
                    case DbDatePart::kMinute:
                        sql += "MINUTE";
                        break;
                    case DbDatePart::kSecond:
                        sql += "SECOND";
                        break;
                    case DbDatePart::kDow:
                        requirePg("EXTRACT DOW");
                        sql += "DOW";
                        break;
                    case DbDatePart::kDoy:
                        requirePg("EXTRACT DOY");
                        sql += "DOY";
                        break;
                    case DbDatePart::kWeek:
                        sql += "WEEK";
                        break;
                    case DbDatePart::kQuarter:
                        sql += "QUARTER";
                        break;
                }
                sql += " FROM ";
                child(n.left);
                sql += ')';
                break;
            }
            case DbNodeKind::kSubscript:
                requirePg("array subscript");
                sql += '(';
                child(n.left);
                sql += ")[";
                child(n.right);
                sql += ']';
                break;
            case DbNodeKind::kCollate:
                sql += '(';
                child(n.left);
                sql += " COLLATE ";
                qualified(n.text);
                sql += ')';
                break;
        }
    }
    void binary(const DbQueryStorage& s, const DbQueryNode& n, std::size_t depth) {
        const auto& left = s.nodes.at(n.left);
        const auto& right = s.nodes.at(n.right);
        const bool nullLeft = left.kind == DbNodeKind::kValue && DbValueAccess::type(left.value) == DbValueType::kNull;
        const bool nullRight = right.kind == DbNodeKind::kValue && DbValueAccess::type(right.value) == DbValueType::kNull;
        if ((n.binary == DbBinaryOperator::kEqual || n.binary == DbBinaryOperator::kNotEqual) && (nullLeft || nullRight)) {
            sql += '(';
            expr(s, nullLeft ? n.right : n.left, depth);
            sql += n.binary == DbBinaryOperator::kEqual ? " IS NULL)" : " IS NOT NULL)";
            return;
        }
        if ((n.binary == DbBinaryOperator::kIn || n.binary == DbBinaryOperator::kNotIn) && right.kind == DbNodeKind::kList && right.args.empty()) {
            sql += n.binary == DbBinaryOperator::kIn ? "FALSE" : "TRUE";
            return;
        }
        if (n.binary == DbBinaryOperator::kConcat && !pg()) {
            sql += "CONCAT(";
            expr(s, n.left, depth);
            sql += ", ";
            expr(s, n.right, depth);
            sql += ')';
            return;
        }
        const bool mariaDistinct = !pg() && n.binary == DbBinaryOperator::kIsDistinctFrom;
        sql += '(';
        if (mariaDistinct) {
            sql += "NOT (";
        }
        expr(s, n.left, depth);
        std::string_view token;
        switch (n.binary) {
            case DbBinaryOperator::kEqual:
                token = "=";
                break;
            case DbBinaryOperator::kNotEqual:
                token = "<>";
                break;
            case DbBinaryOperator::kLess:
                token = "<";
                break;
            case DbBinaryOperator::kLessEqual:
                token = "<=";
                break;
            case DbBinaryOperator::kGreater:
                token = ">";
                break;
            case DbBinaryOperator::kGreaterEqual:
                token = ">=";
                break;
            case DbBinaryOperator::kAnd:
                token = "AND";
                break;
            case DbBinaryOperator::kOr:
                token = "OR";
                break;
            case DbBinaryOperator::kAdd:
                token = "+";
                break;
            case DbBinaryOperator::kSubtract:
                token = "-";
                break;
            case DbBinaryOperator::kMultiply:
                token = "*";
                break;
            case DbBinaryOperator::kDivide:
                token = "/";
                break;
            case DbBinaryOperator::kModulo:
                token = "%";
                break;
            case DbBinaryOperator::kConcat:
                token = "||";
                break;
            case DbBinaryOperator::kLike:
                token = "LIKE";
                break;
            case DbBinaryOperator::kNotLike:
                token = "NOT LIKE";
                break;
            case DbBinaryOperator::kILike:
                requirePg("ILIKE");
                token = "ILIKE";
                break;
            case DbBinaryOperator::kNotILike:
                requirePg("NOT ILIKE");
                token = "NOT ILIKE";
                break;
            case DbBinaryOperator::kIn:
                token = "IN";
                break;
            case DbBinaryOperator::kNotIn:
                token = "NOT IN";
                break;
            case DbBinaryOperator::kIsDistinctFrom:
                token = pg() ? "IS DISTINCT FROM" : "<=>";
                break;
            case DbBinaryOperator::kIsNotDistinctFrom:
                token = pg() ? "IS NOT DISTINCT FROM" : "<=>";
                break;
            case DbBinaryOperator::kBitAnd:
                token = "&";
                break;
            case DbBinaryOperator::kBitOr:
                token = "|";
                break;
            case DbBinaryOperator::kBitXor:
                token = pg() ? "#" : "^";
                break;
            default:
                requirePg("PostgreSQL JSON, array, regex or network operator");
                switch (n.binary) {
                    case DbBinaryOperator::kJsonGet:
                        token = "->";
                        break;
                    case DbBinaryOperator::kJsonGetText:
                        token = "->>";
                        break;
                    case DbBinaryOperator::kJsonPath:
                        token = "#>";
                        break;
                    case DbBinaryOperator::kJsonPathText:
                        token = "#>>";
                        break;
                    case DbBinaryOperator::kJsonContains:
                    case DbBinaryOperator::kArrayContains:
                        token = "@>";
                        break;
                    case DbBinaryOperator::kJsonContainedBy:
                    case DbBinaryOperator::kArrayContainedBy:
                        token = "<@";
                        break;
                    case DbBinaryOperator::kJsonHasKey:
                        token = "?";
                        break;
                    case DbBinaryOperator::kJsonHasAnyKey:
                        token = "?|";
                        break;
                    case DbBinaryOperator::kJsonHasAllKeys:
                        token = "?&";
                        break;
                    case DbBinaryOperator::kJsonConcat:
                        token = "||";
                        break;
                    case DbBinaryOperator::kJsonDelete:
                        token = "-";
                        break;
                    case DbBinaryOperator::kJsonDeletePath:
                        token = "#-";
                        break;
                    case DbBinaryOperator::kArrayOverlap:
                    case DbBinaryOperator::kInetOverlap:
                        token = "&&";
                        break;
                    case DbBinaryOperator::kRegex:
                        token = "~";
                        break;
                    case DbBinaryOperator::kRegexInsensitive:
                        token = "~*";
                        break;
                    case DbBinaryOperator::kInetContains:
                        token = ">>";
                        break;
                    case DbBinaryOperator::kInetContainsOrEqual:
                        token = ">>=";
                        break;
                    case DbBinaryOperator::kInetContainedBy:
                        token = "<<";
                        break;
                    case DbBinaryOperator::kInetContainedByOrEqual:
                        token = "<<=";
                        break;
                    default:
                        std::terminate();
                }
        }
        if ((n.binary == DbBinaryOperator::kIn || n.binary == DbBinaryOperator::kNotIn) && right.kind != DbNodeKind::kList && right.kind != DbNodeKind::kSubquery) {
            throw std::invalid_argument("IN requires a list or a subquery");
        }
        sql += ' ';
        sql += token;
        sql += ' ';
        expr(s, n.right, depth);
        if (mariaDistinct) {
            sql += ')';
        }
        sql += ')';
    }
    void source(const DbQueryStorage& s, const DbQuerySource& value, std::size_t depth) {
        if (value.lateral) {
            requirePg("LATERAL");
            sql += "LATERAL ";
        }
        switch (value.kind) {
            case DbSourceKind::kTable:
                qualified(value.name);
                break;
            case DbSourceKind::kQuery:
                sql += '(';
                query(nested(s, value.query), depth + 1);
                sql += ')';
                break;
            case DbSourceKind::kFunction:
                requirePg("table function");
                expr(s, value.expression, depth + 1);
                break;
        }
        if (value.ordinality) {
            requirePg("WITH ORDINALITY");
            if (value.kind != DbSourceKind::kFunction) {
                throw std::invalid_argument("WITH ORDINALITY requires a table function");
            }
            sql += " WITH ORDINALITY";
        }
        if (!value.alias.empty()) {
            sql += " AS ";
            identifier(value.alias);
        }
        if (!value.columns.empty()) {
            if (value.alias.empty()) {
                throw std::invalid_argument("source column names require an alias");
            }
            sql += " (";
            const bool typed = value.columns.front().type.dataType != DbDataType::kInferred || !value.columns.front().type.customName.empty();
            if (typed && (value.kind != DbSourceKind::kFunction || value.ordinality)) {
                throw std::invalid_argument("record definitions require a function without ordinality");
            }
            separated(value.columns, [&](const auto& column) {
                const bool hasType = column.type.dataType != DbDataType::kInferred || !column.type.customName.empty();
                if (hasType != typed) {
                    throw std::invalid_argument("source column definitions must all specify a type or all omit it");
                }
                identifier(column.name);
                if (typed) {
                    sql += ' ';
                    const auto& t = column.type;
                    appendDbTypeName(sql, t.dataType, t.customName, t.length, t.precision, t.scale, t.array, driver_);
                }
            });
            sql += ')';
        }
    }
    void sources(const DbQueryStorage& s, std::size_t depth) {
        source(s, *s.source, depth);
        for (const auto& join : s.joins) {
            switch (join.type) {
                case DbJoinType::kInner:
                    sql += " INNER JOIN ";
                    break;
                case DbJoinType::kLeft:
                    sql += " LEFT JOIN ";
                    break;
                case DbJoinType::kRight:
                    sql += " RIGHT JOIN ";
                    break;
                case DbJoinType::kFull:
                    requirePg("FULL JOIN");
                    sql += " FULL JOIN ";
                    break;
                case DbJoinType::kCross:
                    sql += " CROSS JOIN ";
                    break;
            }
            if (join.type == DbJoinType::kCross) {
                if (join.on != noDbNode || !join.usingColumns.empty()) {
                    throw std::invalid_argument("CROSS JOIN has no ON or USING");
                }
            } else if ((join.on != noDbNode) == !join.usingColumns.empty()) {
                throw std::invalid_argument("JOIN requires exactly one of ON or USING");
            }
            source(s, join.source, depth + 1);
            if (join.on != noDbNode) {
                sql += " ON ";
                expr(s, join.on, depth + 1);
            }
            if (!join.usingColumns.empty()) {
                sql += " USING (";
                identifiers(join.usingColumns);
                sql += ')';
            }
        }
    }
    void assignments(const DbQueryStorage& s, const std::pmr::vector<DbStoredAssignment>& values, std::size_t depth) {
        if (values.empty()) {
            throw std::invalid_argument("UPDATE requires assignments");
        }
        separated(values, [&](const auto& assignment) { identifier(assignment.column); sql += " = "; expr(s, assignment.expression, depth + 1); });
    }
    void conflict(const DbQueryStorage& s, std::size_t depth) {
        const auto& value = *s.conflict;
        if (value.doNothing && (!value.assignments.empty() || value.updateWhere != noDbNode)) {
            throw std::invalid_argument("DO NOTHING cannot specify updates");
        }
        if (pg()) {
            if (value.anyUniqueKey) {
                throw std::invalid_argument("anyUniqueKey is specific to MariaDB");
            }
            if (!value.columns.empty() && !value.constraint.empty()) {
                throw std::invalid_argument("conflict target cannot have both columns and a constraint");
            }
            if (value.targetWhere != noDbNode && value.columns.empty()) {
                throw std::invalid_argument("a conflict predicate requires target columns");
            }
            sql += " ON CONFLICT";
            if (!value.columns.empty()) {
                sql += " (";
                identifiers(value.columns);
                sql += ')';
            }
            if (!value.constraint.empty()) {
                sql += " ON CONSTRAINT ";
                identifier(value.constraint);
            }
            if (value.targetWhere != noDbNode) {
                sql += " WHERE ";
                expr(s, value.targetWhere, depth + 1);
            }
            if (value.doNothing) {
                sql += " DO NOTHING";
                return;
            }
            if (value.columns.empty() && value.constraint.empty()) {
                throw std::invalid_argument("DO UPDATE requires a conflict target");
            }
            sql += " DO UPDATE SET ";
            assignments(s, value.assignments, depth);
            if (value.updateWhere != noDbNode) {
                sql += " WHERE ";
                expr(s, value.updateWhere, depth + 1);
            }
        } else {
            if (!value.anyUniqueKey || !value.columns.empty() || !value.constraint.empty() || value.targetWhere != noDbNode || value.updateWhere != noDbNode || value.doNothing) {
                unsupported("targeted or conditional upsert / DO NOTHING");
            }
            sql += " ON DUPLICATE KEY UPDATE ";
            assignments(s, value.assignments, depth);
        }
    }
    void rows(const DbQueryStorage& s, std::size_t depth) {
        if (s.rows.empty() || s.rows.front().empty()) {
            throw std::invalid_argument("VALUES requires nonempty rows");
        }
        const auto width = s.rows.front().size();
        if (!s.columns.empty() && width != s.columns.size()) {
            throw std::invalid_argument("INSERT column and row widths differ");
        }
        sql += "VALUES ";
        separated(s.rows, [&](const auto& row) {
            if (row.size() != width) {
                throw std::invalid_argument("VALUES rows have different widths");
            }
            sql += '(';
            expressions(s, row, depth + 1);
            sql += ')';
        });
    }
    void body(const DbQueryStorage& s, std::size_t depth) {
        switch (s.kind) {
            case DbQueryKind::kSelect:
                sql += "SELECT ";
                if (s.distinct) {
                    sql += "DISTINCT ";
                }
                if (!s.distinctOn.empty()) {
                    requirePg("DISTINCT ON");
                    sql += "DISTINCT ON (";
                    expressions(s, s.distinctOn, depth + 1);
                    sql += ") ";
                }
                if (s.projections.empty()) {
                    sql += '*';
                } else {
                    expressions(s, s.projections, depth + 1, true);
                }
                if (s.source) {
                    sql += " FROM ";
                    sources(s, depth + 1);
                }
                break;
            case DbQueryKind::kValues:
                rows(s, depth);
                break;
            case DbQueryKind::kInsert:
                sql += "INSERT INTO ";
                qualified(s.target);
                if (!s.targetAlias.empty()) {
                    requirePg("INSERT target alias");
                    sql += " AS ";
                    identifier(s.targetAlias);
                }
                if (!s.columns.empty()) {
                    sql += " (";
                    identifiers(s.columns);
                    sql += ')';
                }
                sql += ' ';
                if (s.insertQuery != noDbNode) {
                    if (!s.rows.empty()) {
                        throw std::invalid_argument("INSERT cannot have both VALUES and SELECT");
                    }
                    query(nested(s, s.insertQuery), depth + 1);
                } else if (s.rows.empty()) {
                    if (!s.columns.empty()) {
                        throw std::invalid_argument("INSERT column list requires input rows");
                    }
                    sql += pg() ? "DEFAULT VALUES" : "() VALUES ()";
                } else {
                    rows(s, depth);
                }
                if (s.conflict) {
                    conflict(s, depth + 1);
                }
                break;
            case DbQueryKind::kUpdate:
            case DbQueryKind::kDelete:
                sql += s.kind == DbQueryKind::kUpdate ? "UPDATE " : "DELETE FROM ";
                qualified(s.target);
                if (!s.targetAlias.empty()) {
                    if (!pg() && s.kind == DbQueryKind::kDelete) {
                        unsupported("DELETE target alias");
                    }
                    sql += " AS ";
                    identifier(s.targetAlias);
                }
                if (s.kind == DbQueryKind::kUpdate) {
                    sql += " SET ";
                    assignments(s, s.assignments, depth + 1);
                }
                if (s.source) {
                    requirePg("UPDATE FROM / DELETE USING");
                    sql += s.kind == DbQueryKind::kUpdate ? " FROM " : " USING ";
                    sources(s, depth + 1);
                }
                break;
        }
        if (s.predicate != noDbNode) {
            sql += " WHERE ";
            expr(s, s.predicate, depth + 1);
        }
        if (!s.groups.empty()) {
            sql += " GROUP BY ";
            expressions(s, s.groups, depth + 1);
        }
        if (s.having != noDbNode) {
            sql += " HAVING ";
            expr(s, s.having, depth + 1);
        }
    }
    static void validate(const DbQueryStorage& s) {
        const bool select = s.kind == DbQueryKind::kSelect;
        const bool values = s.kind == DbQueryKind::kValues;
        const bool insert = s.kind == DbQueryKind::kInsert;
        const bool update = s.kind == DbQueryKind::kUpdate;
        if (!select && (!s.projections.empty() || s.distinct || !s.distinctOn.empty() || !s.groups.empty() || s.having != noDbNode)) {
            throw std::invalid_argument("SELECT clauses cannot be attached to a non-SELECT query");
        }
        if ((select || values) && (!s.returning.empty() || !s.target.empty())) {
            throw std::invalid_argument("a row query cannot have a DML target or RETURNING");
        }
        if ((insert || values) && (s.source || !s.joins.empty() || s.predicate != noDbNode)) {
            throw std::invalid_argument("INSERT/VALUES cannot have FROM/JOIN/WHERE clauses");
        }
        if (!s.source && !s.joins.empty()) {
            throw std::invalid_argument("JOIN requires a FROM source");
        }
        if (!insert && (s.conflict || s.insertQuery != noDbNode || !s.columns.empty())) {
            throw std::invalid_argument("INSERT clauses on a non-INSERT query");
        }
        if (!insert && !values && !s.rows.empty()) {
            throw std::invalid_argument("VALUES on an incompatible query");
        }
        if (!update && !s.assignments.empty()) {
            throw std::invalid_argument("SET on a non-UPDATE query");
        }
        if (!select && !values && (!s.orders.empty() || s.limit || s.offset || s.lock || !s.setOperations.empty())) {
            throw std::invalid_argument("ordering, pagination, locks and set operations require a row query");
        }
        if (s.lock && (!select || !s.setOperations.empty() || s.distinct || !s.distinctOn.empty() || !s.groups.empty() || s.having != noDbNode)) {
            throw std::invalid_argument("row locks require an ungrouped SELECT without DISTINCT or set operations");
        }
    }
};

}  // namespace ruvia::detail

namespace ruvia {

DbStatement DbQuery::compile(DbDriver driver, std::pmr::memory_resource* resource, DbParameterMode mode) const {
    detail::DbQueryCompiler compiler(driver, detail::pmrResourceOrDefault(resource), mode);
    compiler.query(storage());
    return DbStatement(std::move(compiler.sql), std::move(compiler.params), returnsRows());
}

std::pmr::string DbQuery::renderExpression(Expr expression, DbDriver driver,
    std::pmr::memory_resource* resource, DbParameterMode mode) {
    if (mode != DbParameterMode::kLiteral) {
        throw std::invalid_argument("standalone expression rendering requires literal mode");
    }
    detail::DbQueryCompiler compiler(driver, detail::pmrResourceOrDefault(resource), mode);
    compiler.expression(expression);
    return std::move(compiler.sql);
}

}  // namespace ruvia
