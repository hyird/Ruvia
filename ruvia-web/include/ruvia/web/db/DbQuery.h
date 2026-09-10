#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/db/DbEntity.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbParameterPack.h"

namespace ruvia {

namespace detail {
class DbQueryStorage;
class DbQueryCompiler;
class DbRelationPlan;
}  // namespace detail

enum class DbParameterMode : std::uint8_t { kBound,
    kLiteral };
enum class DbBinaryOperator : std::uint8_t {
    kEqual,
    kNotEqual,
    kLess,
    kLessEqual,
    kGreater,
    kGreaterEqual,
    kAnd,
    kOr,
    kAdd,
    kSubtract,
    kMultiply,
    kDivide,
    kModulo,
    kConcat,
    kLike,
    kNotLike,
    kILike,
    kNotILike,
    kIn,
    kNotIn,
    kIsDistinctFrom,
    kIsNotDistinctFrom,
    kBitAnd,
    kBitOr,
    kBitXor,
    kJsonGet,
    kJsonGetText,
    kJsonPath,
    kJsonPathText,
    kJsonContains,
    kJsonContainedBy,
    kJsonHasKey,
    kJsonHasAnyKey,
    kJsonHasAllKeys,
    kJsonConcat,
    kJsonDelete,
    kJsonDeletePath,
    kArrayContains,
    kArrayContainedBy,
    kArrayOverlap,
    kRegex,
    kRegexInsensitive,
    kInetContains,
    kInetContainsOrEqual,
    kInetContainedBy,
    kInetContainedByOrEqual,
    kInetOverlap,
};
enum class DbUnaryOperator : std::uint8_t { kNot,
    kNegate,
    kBitNot,
    kIsNull,
    kIsNotNull,
    kIsTrue,
    kIsFalse,
    kIsNotTrue,
    kIsNotFalse };
enum class DbJoinType : std::uint8_t { kInner,
    kLeft,
    kRight,
    kFull,
    kCross };
enum class DbOrderDirection : std::uint8_t { kAsc,
    kDesc };
enum class DbNullsOrder : std::uint8_t { kDefault,
    kFirst,
    kLast };
enum class DbRowLock : std::uint8_t { kUpdate,
    kNoKeyUpdate,
    kShare,
    kKeyShare };
enum class DbMaterialization : std::uint8_t { kDefault,
    kMaterialized,
    kNotMaterialized };
enum class DbSetOperation : std::uint8_t { kUnion,
    kUnionAll,
    kIntersect,
    kIntersectAll,
    kExcept,
    kExceptAll };
enum class DbWindowFrame : std::uint8_t { kRows,
    kRange,
    kGroups };
enum class DbFrameBoundary : std::uint8_t { kUnboundedPreceding,
    kPreceding,
    kCurrentRow,
    kFollowing,
    kUnboundedFollowing };
enum class DbDatePart : std::uint8_t { kEpoch,
    kYear,
    kMonth,
    kDay,
    kHour,
    kMinute,
    kSecond,
    kDow,
    kDoy,
    kWeek,
    kQuarter };

struct DbTypeDefinition final {
    DbDataType dataType{DbDataType::kInferred};
    std::string customName{};
    std::size_t length{0};
    unsigned precision{0};
    unsigned scale{0};
    bool array{false};
};

// Borrows the address-stable AST owned by its query. Import an expression or
// pass it to an ORM operation while that query is alive; no operation retains
// this view after returning its cold asynchronous operation.
class DbExpression final {
public:
    DbExpression() noexcept = default;
    [[nodiscard]] bool empty() const noexcept {
        return owner_ == nullptr;
    }

private:
    friend class DbQuery;
    friend class detail::DbQueryCompiler;
    DbExpression(const detail::DbQueryStorage* owner, std::size_t node) noexcept
        : owner_(owner),
          node_(node) {}
    const detail::DbQueryStorage* owner_{nullptr};
    std::size_t node_{0};
};

struct DbOrderTerm final {
    DbExpression expression{};
    DbOrderDirection direction{DbOrderDirection::kAsc};
    DbNullsOrder nulls{DbNullsOrder::kDefault};
};
struct DbWindowBoundary final {
    DbFrameBoundary kind{DbFrameBoundary::kCurrentRow};
    std::uint64_t offset{0};
};
struct DbWindowFrameOptions final {
    DbWindowFrame kind{DbWindowFrame::kRows};
    DbWindowBoundary start{DbFrameBoundary::kUnboundedPreceding};
    DbWindowBoundary end{DbFrameBoundary::kCurrentRow};
};
struct DbWindowOptions final {
    std::vector<DbExpression> partitionBy{};
    std::vector<DbOrderTerm> orderBy{};
    std::optional<DbWindowFrameOptions> frame{};
};
struct DbCaseBranch final {
    DbExpression when{};
    DbExpression then{};
};
struct DbNamedArgument final {
    std::string name{};
    DbExpression value{};
};
struct DbSourceColumn final {
    std::string name{};
    DbTypeDefinition type{};
};
struct DbSourceOptions final {
    bool lateral{false};
    bool withOrdinality{false};
    std::vector<DbSourceColumn> columns{};
};
struct DbCteOptions final {
    bool recursive{false};
    DbMaterialization materialization{DbMaterialization::kDefault};
    std::vector<std::string> columns{};
};
struct DbLockOptions final {
    DbRowLock mode{DbRowLock::kUpdate};
    bool nowait{false};
    bool skipLocked{false};
    std::vector<std::string> tables{};
};
struct DbAssignment final {
    std::string column{};
    DbExpression value{};
};
struct DbConflictOptions final {
    std::vector<std::string> columns{};
    std::string constraint{};
    DbExpression targetWhere{};
    std::vector<DbAssignment> update{};
    DbExpression updateWhere{};
    bool doNothing{false};
    // MariaDB matches any unique key. This explicit mode does not pretend to
    // implement PostgreSQL's conflict target semantics.
    bool anyUniqueKey{false};
};

class DbStatement final {
public:
    DbStatement(const DbStatement&) = delete;
    DbStatement& operator=(const DbStatement&) = delete;
    DbStatement(DbStatement&&) noexcept = default;
    DbStatement& operator=(DbStatement&&) = delete;
    [[nodiscard]] std::string_view sql() const& noexcept {
        return sql_;
    }
    std::string_view sql() const&& = delete;
    [[nodiscard]] std::span<const DbValue> params() const& noexcept {
        return params_;
    }
    std::span<const DbValue> params() const&& = delete;
    [[nodiscard]] bool returnsRows() const noexcept {
        return returnsRows_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return sql_.get_allocator().resource();
    }

private:
    friend class DbQuery;
    friend class DbHandle;
    friend class DbTransaction;
    DbStatement(std::pmr::string sql, std::pmr::vector<DbValue> params, bool rows)
        : sql_(std::move(sql)),
          params_(std::move(params)),
          returnsRows_(rows) {}
    std::pmr::string sql_;
    std::pmr::vector<DbValue> params_;
    bool returnsRows_{false};
};

// A synchronous relational query AST. Values, identifiers and nested queries
// become owned data at each construction call. Compilation is the sole place
// that emits SQL and assigns parameter positions.
class DbQuery final {
public:
    using Expr = DbExpression;

    explicit DbQuery(std::pmr::memory_resource* resource = nullptr);
    DbQuery(const DbQuery&) = delete;
    DbQuery& operator=(const DbQuery&) = delete;
    DbQuery(DbQuery&&) noexcept;
    DbQuery& operator=(DbQuery&&) noexcept;
    ~DbQuery();

    [[nodiscard]] std::pmr::memory_resource* resource() const;
    [[nodiscard]] DbQuery clone(std::pmr::memory_resource* resource) const;
    [[nodiscard]] bool returnsRows() const;
    [[nodiscard]] bool hasWhere() const;
    [[nodiscard]] bool hasGrouping() const;

    Expr column(std::string_view name, std::string_view table = {});
    Expr star(std::string_view table = {});
    Expr value(DbValue value);
    template <detail::DbParameter Value>
        requires(!std::same_as<std::remove_cvref_t<Value>, DbValue>)
    Expr value(Value&& value) {
        return this->value(detail::makeImmediateDbParameter(std::forward<Value>(value)));
    }
    Expr excluded(std::string_view column);
    Expr nullValue();
    Expr defaultValue();
    Expr call(std::string_view function, std::span<const Expr> args = {}, std::span<const DbNamedArgument> named = {});
    Expr call(std::string_view function, std::initializer_list<Expr> args) {
        return call(function, std::span<const Expr>(args.begin(), args.size()));
    }
    Expr coalesce(std::span<const Expr> args);
    Expr coalesce(std::initializer_list<Expr> args) {
        return coalesce(std::span<const Expr>(args.begin(), args.size()));
    }
    Expr nullIf(Expr value, Expr other);
    Expr greatest(std::span<const Expr> args);
    Expr greatest(std::initializer_list<Expr> args) {
        return greatest(std::span<const Expr>(args.begin(), args.size()));
    }
    Expr least(std::span<const Expr> args);
    Expr least(std::initializer_list<Expr> args) {
        return least(std::span<const Expr>(args.begin(), args.size()));
    }
    Expr binary(Expr lhs, DbBinaryOperator op, Expr rhs);
    Expr unary(DbUnaryOperator op, Expr value);
    Expr between(Expr value, Expr lower, Expr upper, bool negate = false);
    Expr tuple(std::span<const Expr> values);
    Expr tuple(std::initializer_list<Expr> values) {
        return tuple(std::span<const Expr>(values.begin(), values.size()));
    }
    Expr list(std::span<const Expr> values);
    Expr list(std::initializer_list<Expr> values) {
        return list(std::span<const Expr>(values.begin(), values.size()));
    }
    Expr array(std::span<const Expr> values);
    Expr array(std::initializer_list<Expr> values) {
        return array(std::span<const Expr>(values.begin(), values.size()));
    }
    Expr any(Expr array);
    Expr all(Expr array);
    Expr cast(Expr value, const DbTypeDefinition& type);
    Expr cast(Expr value, DbDataType type) {
        return cast(value, DbTypeDefinition{.dataType = type});
    }
    Expr alias(Expr value, std::string_view name);
    Expr caseWhen(std::span<const DbCaseBranch> branches, Expr otherwise = {});
    Expr caseWhen(std::initializer_list<DbCaseBranch> branches, Expr otherwise = {}) {
        return caseWhen(std::span<const DbCaseBranch>(branches.begin(), branches.size()), otherwise);
    }
    Expr exists(const DbQuery& query);
    Expr subquery(const DbQuery& query);
    Expr aggregate(std::string_view function, std::span<const Expr> args, bool distinct = false, std::span<const DbOrderTerm> order = {});
    Expr aggregate(std::string_view function, std::initializer_list<Expr> args, bool distinct = false, std::span<const DbOrderTerm> order = {}) {
        return aggregate(function, std::span<const Expr>(args.begin(), args.size()), distinct, order);
    }
    Expr filter(Expr aggregate, Expr predicate);
    Expr over(Expr function, const DbWindowOptions& window);
    Expr withinGroup(Expr function, std::span<const DbOrderTerm> order);
    Expr extract(DbDatePart part, Expr value);
    Expr subscript(Expr array, Expr index);
    Expr collate(Expr value, std::string_view collation);
    Expr importExpression(Expr expression, std::string_view sourceQualifier = {}, std::string_view targetQualifier = {});
    [[nodiscard]] static std::pmr::string renderExpression(Expr expression, DbDriver driver,
        std::pmr::memory_resource* resource, DbParameterMode mode = DbParameterMode::kLiteral);

    DbQuery& select(std::span<const Expr> projections);
    DbQuery& select(std::initializer_list<Expr> projections) {
        return select(std::span<const Expr>(projections.begin(), projections.size()));
    }
    DbQuery& select(Expr projection) {
        return select(std::span<const Expr>(&projection, 1));
    }
    DbQuery& addSelect(Expr projection);
    DbQuery& from(std::string_view table, std::string_view alias = {});
    DbQuery& from(const DbQuery& query, std::string_view alias, const DbSourceOptions& options = {});
    DbQuery& fromFunction(Expr function, std::string_view alias, const DbSourceOptions& options = {});
    DbQuery& join(DbJoinType type, std::string_view table, Expr on = {}, std::string_view alias = {}, std::span<const std::string_view> usingColumns = {});
    DbQuery& join(DbJoinType type, const DbQuery& query, Expr on, std::string_view alias, const DbSourceOptions& options = {});
    DbQuery& joinFunction(DbJoinType type, Expr function, Expr on, std::string_view alias, const DbSourceOptions& options = {});
    DbQuery& where(Expr predicate);
    DbQuery& andWhere(Expr predicate);
    DbQuery& orWhere(Expr predicate);
    DbQuery& groupBy(std::span<const Expr> expressions);
    DbQuery& groupBy(std::initializer_list<Expr> expressions) {
        return groupBy(std::span<const Expr>(expressions.begin(), expressions.size()));
    }
    DbQuery& addGroupBy(Expr expression);
    DbQuery& having(Expr predicate);
    DbQuery& andHaving(Expr predicate);
    DbQuery& orderBy(Expr expression, DbOrderDirection direction = DbOrderDirection::kAsc, DbNullsOrder nulls = DbNullsOrder::kDefault);
    DbQuery& addOrderBy(Expr expression, DbOrderDirection direction = DbOrderDirection::kAsc, DbNullsOrder nulls = DbNullsOrder::kDefault);
    DbQuery& clearOrder();
    DbQuery& limit(std::optional<std::uint64_t> count);
    DbQuery& offset(std::optional<std::uint64_t> count);
    DbQuery& distinct(bool enabled = true);
    DbQuery& distinctOn(std::span<const Expr> expressions);
    DbQuery& distinctOn(std::initializer_list<Expr> expressions) {
        return distinctOn(std::span<const Expr>(expressions.begin(), expressions.size()));
    }
    DbQuery& returning(std::span<const Expr> expressions);
    DbQuery& returning(std::initializer_list<Expr> expressions) {
        return returning(std::span<const Expr>(expressions.begin(), expressions.size()));
    }
    DbQuery& insertInto(std::string_view table, std::span<const std::string_view> columns = {}, std::string_view alias = {});
    DbQuery& insertInto(std::string_view table, std::initializer_list<std::string_view> columns, std::string_view alias = {}) {
        return insertInto(table, std::span<const std::string_view>(columns.begin(), columns.size()), alias);
    }
    DbQuery& values(std::span<const Expr> row);
    DbQuery& values(std::initializer_list<Expr> row) {
        return values(std::span<const Expr>(row.begin(), row.size()));
    }
    DbQuery& insertFrom(const DbQuery& query);
    DbQuery& onConflict(const DbConflictOptions& conflict);
    DbQuery& update(std::string_view table, std::string_view alias = {});
    DbQuery& set(std::string_view column, Expr value);
    DbQuery& updateFrom(std::string_view table, std::string_view alias = {});
    DbQuery& updateFrom(const DbQuery& query, std::string_view alias);
    DbQuery& deleteFrom(std::string_view table, std::string_view alias = {});
    DbQuery& deleteUsing(std::string_view table, std::string_view alias = {});
    DbQuery& deleteUsing(const DbQuery& query, std::string_view alias);
    DbQuery& lock(const DbLockOptions& options);
    DbQuery& clearLock();
    DbQuery& with(std::string_view name, const DbQuery& query, const DbCteOptions& options = {});
    DbQuery& combine(DbSetOperation operation, const DbQuery& query);

    [[nodiscard]] DbStatement compile(DbDriver driver, std::pmr::memory_resource* resource,
        DbParameterMode mode = DbParameterMode::kBound) const;

private:
    friend class detail::DbQueryCompiler;
    friend class detail::DbRelationPlan;
    friend class DbSchema;
    struct StorageDeleter final {
        std::pmr::memory_resource* resource{nullptr};
        void operator()(detail::DbQueryStorage* storage) const noexcept;
    };
    using StorageOwner = std::unique_ptr<detail::DbQueryStorage, StorageDeleter>;
    explicit DbQuery(StorageOwner storage) noexcept;
    [[nodiscard]] static StorageOwner copyStorage(const detail::DbQueryStorage& source, std::pmr::memory_resource* resource);
    [[nodiscard]] detail::DbQueryStorage& storage();
    [[nodiscard]] const detail::DbQueryStorage& storage() const;
    [[nodiscard]] std::size_t requireExpression(Expr expression) const;
    void requireSelectQuery() const;
    [[nodiscard]] bool usesSourceName(std::string_view name) const;
    [[nodiscard]] bool usesProjectionName(std::string_view name) const;
    [[nodiscard]] std::optional<DbQuery> prepareEntityRead(
        std::span<const std::string_view> primaryKey, std::string_view rootAlias,
        DbDriver driver) const;
    StorageOwner storage_;
};

}  // namespace ruvia
