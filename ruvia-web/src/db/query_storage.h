#pragma once

#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ruvia/web/db/DbQuery.h"

#include "db/DbSqlFormat.h"

namespace ruvia::detail {

constexpr std::size_t noDbNode = std::numeric_limits<std::size_t>::max();

enum class DbNodeKind : std::uint8_t {
    kSql,
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

void validate_query_shape(const DbQueryStorage& storage);

}  // namespace ruvia::detail
