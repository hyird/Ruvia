#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_map>

#include "ruvia/core/memory/PmrResource.h"

#include "query_storage.h"

namespace ruvia::detail {
namespace {

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

class query_sql_writer final {
public:
    query_sql_writer(DbDriver driver, std::pmr::memory_resource* resource, DbParameterMode mode)
        : sql(resource),
          params(resource),
          driver_(driver),
          mode_(mode),
          parameterPositions_(resource) {
        requireDbDialect(driver);
    }

    std::pmr::string sql;
    std::pmr::vector<DbValue> params;
    [[nodiscard]] DbDriver driver() const noexcept {
        return driver_;
    }

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

    void type(const DbStoredType& value) {
        appendDbTypeName(sql, value.dataType, value.customName, value.length, value.precision,
            value.scale, value.array, driver_, DbSqlTypeUsage::kCast);
    }

private:
    DbDriver driver_;
    DbParameterMode mode_;
    std::pmr::unordered_map<const DbQueryNode*, std::size_t> parameterPositions_;
};

}  // namespace

class DbQueryCompiler final {
public:
    DbQueryCompiler(DbDriver driver, std::pmr::memory_resource* resource, DbParameterMode mode)
        : output(driver, resource, mode) {}

    query_sql_writer output;

    void expression(DbExpression value) {
        if (value.empty()) {
            throw std::invalid_argument("empty database expression");
        }
        expr(*value.owner_, value.node_, 0);
    }
    void query(const DbQueryStorage& s, std::size_t depth = 0) {
        requireDepth(depth);
        validate_query_shape(s);
        if (!s.ctes.empty()) {
            if (!output.pg() && s.kind != DbQueryKind::kSelect) {
                output.unsupported("data-changing WITH");
            }
            output.sql += "WITH ";
            if (std::ranges::any_of(s.ctes, [](const auto& cte) { return cte.recursive; })) {
                output.sql += "RECURSIVE ";
            }
            separated(s.ctes, [&](const auto& cte) {
                output.identifier(cte.name);
                if (!cte.columns.empty()) {
                    output.sql += " (";
                    identifiers(cte.columns);
                    output.sql += ')';
                }
                output.sql += " AS ";
                switch (cte.materialization) {
                    case DbMaterialization::kDefault:
                        break;
                    case DbMaterialization::kMaterialized:
                        output.requirePg("materialized CTE");
                        output.sql += "MATERIALIZED ";
                        break;
                    case DbMaterialization::kNotMaterialized:
                        output.requirePg("not-materialized CTE");
                        output.sql += "NOT MATERIALIZED ";
                        break;
                }
                const auto& child = nested(s, cte.query);
                if (child.kind != DbQueryKind::kSelect && child.kind != DbQueryKind::kValues) {
                    output.requirePg("data-changing CTE");
                    if (depth != 0) {
                        throw std::invalid_argument("data-changing CTE requires a top-level WITH statement");
                    }
                }
                output.sql += '(';
                query(child, depth + 1);
                output.sql += ')';
            });
            output.sql += ' ';
        }
        for (std::size_t i = 0; i < s.setOperations.size(); ++i) {
            output.sql += '(';
        }
        body(s, depth + 1);
        for (const auto& operation : s.setOperations) {
            output.sql += ')';
            switch (operation.operation) {
                case DbSetOperation::kUnion:
                    output.sql += " UNION ";
                    break;
                case DbSetOperation::kUnionAll:
                    output.sql += " UNION ALL ";
                    break;
                case DbSetOperation::kIntersect:
                    output.sql += " INTERSECT ";
                    break;
                case DbSetOperation::kIntersectAll:
                    output.requirePg("INTERSECT ALL");
                    output.sql += " INTERSECT ALL ";
                    break;
                case DbSetOperation::kExcept:
                    output.sql += " EXCEPT ";
                    break;
                case DbSetOperation::kExceptAll:
                    output.requirePg("EXCEPT ALL");
                    output.sql += " EXCEPT ALL ";
                    break;
            }
            output.sql += '(';
            query(nested(s, operation.query), depth + 1);
            output.sql += ')';
        }
        if (!s.orders.empty()) {
            output.sql += " ORDER BY ";
            orders(s, s.orders, depth + 1);
        }
        if (s.limit) {
            output.sql += " LIMIT ";
            output.number(*s.limit);
        } else if (s.offset && !output.pg()) {
            output.sql += " LIMIT 18446744073709551615";
        }
        if (s.offset) {
            output.sql += " OFFSET ";
            output.number(*s.offset);
        }
        if (s.lock) {
            const auto& lock = *s.lock;
            switch (lock.mode) {
                case DbRowLock::kUpdate:
                    output.sql += " FOR UPDATE";
                    break;
                case DbRowLock::kNoKeyUpdate:
                    output.requirePg("FOR NO KEY UPDATE");
                    output.sql += " FOR NO KEY UPDATE";
                    break;
                case DbRowLock::kShare:
                    output.sql += output.pg() ? " FOR SHARE" : " LOCK IN SHARE MODE";
                    break;
                case DbRowLock::kKeyShare:
                    output.requirePg("FOR KEY SHARE");
                    output.sql += " FOR KEY SHARE";
                    break;
            }
            if (!lock.tables.empty()) {
                output.requirePg("row lock OF");
                output.sql += " OF ";
                identifiers(lock.tables);
            }
            if (lock.nowait) {
                output.sql += " NOWAIT";
            }
            if (lock.skipLocked) {
                if (!output.pg() && lock.mode != DbRowLock::kUpdate) {
                    output.unsupported("SKIP LOCKED for a shared lock");
                }
                output.sql += " SKIP LOCKED";
            }
        }
        if (!s.returning.empty()) {
            output.requirePg("RETURNING");
            output.sql += " RETURNING ";
            expressions(s, s.returning, depth + 1, true);
        }
    }

private:
    static void requireDepth(std::size_t depth) {
        if (depth > 256) {
            throw std::length_error("database query nesting exceeds 256 levels");
        }
    }
    static const DbQueryStorage& nested(const DbQueryStorage& s, std::size_t index) {
        return s.queries.at(index).storage();
    }
    template <class Range, class Fn>
    void separated(const Range& range, Fn&& fn) {
        bool first = true;
        for (const auto& item : range) {
            if (!first) {
                output.sql += ", ";
            }
            first = false;
            fn(item);
        }
    }
    template <class Range>
    void identifiers(const Range& range) {
        separated(range, [&](const auto& name) { output.identifier(name); });
    }
    template <class Range>
    void expressions(const DbQueryStorage& s, const Range& range, std::size_t depth, bool aliases = false) {
        separated(range, [&](std::size_t index) { expr(s, index, depth, aliases); });
    }
    void orders(const DbQueryStorage& s, const std::pmr::vector<DbStoredOrder>& terms, std::size_t depth) {
        separated(terms, [&](const auto& term) {
            if (!output.pg() && term.nulls != DbNullsOrder::kDefault) {
                output.sql += '(';
                expr(s, term.expression, depth);
                output.sql += " IS NULL)";
                output.sql += term.nulls == DbNullsOrder::kFirst ? " DESC, " : " ASC, ";
            }
            expr(s, term.expression, depth);
            output.sql += term.direction == DbOrderDirection::kAsc ? " ASC" : " DESC";
            if (output.pg()) {
                if (term.nulls == DbNullsOrder::kFirst) {
                    output.sql += " NULLS FIRST";
                }
                if (term.nulls == DbNullsOrder::kLast) {
                    output.sql += " NULLS LAST";
                }
            }
        });
    }
    void boundary(const DbWindowBoundary& boundary) {
        if (boundary.kind != DbFrameBoundary::kPreceding && boundary.kind != DbFrameBoundary::kFollowing && boundary.offset != 0) {
            throw std::invalid_argument("a non-offset window frame boundary has an offset");
        }
        switch (boundary.kind) {
            case DbFrameBoundary::kUnboundedPreceding:
                output.sql += "UNBOUNDED PRECEDING";
                break;
            case DbFrameBoundary::kPreceding:
                appendDbNumber(output.sql, boundary.offset);
                output.sql += " PRECEDING";
                break;
            case DbFrameBoundary::kCurrentRow:
                output.sql += "CURRENT ROW";
                break;
            case DbFrameBoundary::kFollowing:
                appendDbNumber(output.sql, boundary.offset);
                output.sql += " FOLLOWING";
                break;
            case DbFrameBoundary::kUnboundedFollowing:
                output.sql += "UNBOUNDED FOLLOWING";
                break;
        }
    }
    // Imports own their nodes. Equal grouped expressions must nevertheless use
    // the same PostgreSQL parameter nodes in SELECT, GROUP BY and HAVING.
    static bool sameExpression(const DbQueryStorage& s, std::size_t lhs, std::size_t rhs, std::size_t depth = 0) {
        if (lhs == rhs) {
            return true;
        }
        if (lhs == noDbNode || rhs == noDbNode || depth > 256) {
            return false;
        }
        const auto& a = s.nodes.at(lhs);
        const auto& b = s.nodes.at(rhs);
        if (a.kind != b.kind || a.text != b.text || a.qualifier != b.qualifier || a.binary != b.binary ||
            a.unary != b.unary || a.datePart != b.datePart || a.flag != b.flag ||
            a.query != noDbNode || b.query != noDbNode || a.args.size() != b.args.size() ||
            a.named.size() != b.named.size() || a.orders.size() != b.orders.size() ||
            a.type.dataType != b.type.dataType || a.type.customName != b.type.customName ||
            a.type.length != b.type.length || a.type.precision != b.type.precision || a.type.scale != b.type.scale ||
            a.type.array != b.type.array || a.frame.has_value() != b.frame.has_value()) {
            return false;
        }
        if (a.frame && (a.frame->kind != b.frame->kind || a.frame->start.kind != b.frame->start.kind ||
                           a.frame->start.offset != b.frame->start.offset || a.frame->end.kind != b.frame->end.kind ||
                           a.frame->end.offset != b.frame->end.offset)) {
            return false;
        }
        if (a.kind == DbNodeKind::kValue) {
            if (DbValueAccess::type(a.value) != DbValueAccess::type(b.value)) {
                return false;
            }
            switch (DbValueAccess::type(a.value)) {
                case DbValueType::kNull:
                    break;
                case DbValueType::kString:
                    if (DbValueAccess::text(a.value) != DbValueAccess::text(b.value)) {
                        return false;
                    }
                    break;
                case DbValueType::kSigned:
                    if (DbValueAccess::signedValue(a.value) != DbValueAccess::signedValue(b.value)) {
                        return false;
                    }
                    break;
                case DbValueType::kUnsigned:
                    if (DbValueAccess::unsignedValue(a.value) != DbValueAccess::unsignedValue(b.value)) {
                        return false;
                    }
                    break;
                case DbValueType::kDouble:
                    if (DbValueAccess::doubleValue(a.value) != DbValueAccess::doubleValue(b.value)) {
                        return false;
                    }
                    break;
                case DbValueType::kBool:
                    if (DbValueAccess::boolValue(a.value) != DbValueAccess::boolValue(b.value)) {
                        return false;
                    }
                    break;
            }
        }
        const auto same = [&](std::size_t x, std::size_t y) { return sameExpression(s, x, y, depth + 1); };
        if (!same(a.left, b.left) || !same(a.right, b.right)) {
            return false;
        }
        for (std::size_t i = 0; i < a.args.size(); ++i) {
            if (!same(a.args[i], b.args[i])) {
                return false;
            }
        }
        for (std::size_t i = 0; i < a.named.size(); ++i) {
            if (a.named[i].name != b.named[i].name || !same(a.named[i].expression, b.named[i].expression)) {
                return false;
            }
        }
        for (std::size_t i = 0; i < a.orders.size(); ++i) {
            if (a.orders[i].direction != b.orders[i].direction || a.orders[i].nulls != b.orders[i].nulls ||
                !same(a.orders[i].expression, b.orders[i].expression)) {
                return false;
            }
        }
        return true;
    }
    static std::size_t groupExpression(const DbQueryStorage& s, std::size_t index) {
        for (const auto group : s.groups) {
            if (!sameExpression(s, group, index)) {
                continue;
            }
            for (auto projection : s.projections) {
                if (s.nodes[projection].kind == DbNodeKind::kAlias) {
                    projection = s.nodes[projection].left;
                }
                if (sameExpression(s, group, projection)) {
                    return projection;
                }
            }
            return group;
        }
        return index;
    }
    void expr(const DbQueryStorage& s, std::size_t index, std::size_t depth, bool aliases = false) {
        requireDepth(depth);
        if (output.pg() && !s.groups.empty()) {
            index = groupExpression(s, index);
        }
        const auto& n = s.nodes.at(index);
        const auto child = [&](std::size_t id) { expr(s, id, depth + 1); };
        switch (n.kind) {
            case DbNodeKind::kSql:
                output.sql += '(';
                for (const auto& part : n.named) {
                    output.sql += part.name;
                    child(part.expression);
                }
                output.sql += n.text;
                output.sql += ')';
                break;
            case DbNodeKind::kColumn:
            case DbNodeKind::kStar:
                if (!n.qualifier.empty()) {
                    output.qualified(n.qualifier);
                    output.sql += '.';
                }
                if (n.kind == DbNodeKind::kStar) {
                    output.sql += '*';
                } else {
                    output.identifier(n.text);
                }
                break;
            case DbNodeKind::kValue:
                output.bound(n.value, &n);
                break;
            case DbNodeKind::kDefault:
                output.sql += "DEFAULT";
                break;
            case DbNodeKind::kExcluded:
                if (output.pg()) {
                    output.sql += "excluded.";
                    output.identifier(n.text);
                } else {
                    output.sql += "VALUES(";
                    output.identifier(n.text);
                    output.sql += ')';
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
                        output.sql += "COALESCE";
                        break;
                    case DbNodeKind::kNullIf:
                        output.sql += "NULLIF";
                        break;
                    case DbNodeKind::kGreatest:
                        output.sql += "GREATEST";
                        break;
                    case DbNodeKind::kLeast:
                        output.sql += "LEAST";
                        break;
                    default:
                        output.functionName(n.text);
                        break;
                }
                output.sql += '(';
                if (n.kind == DbNodeKind::kAggregate && n.flag) {
                    output.sql += "DISTINCT ";
                }
                expressions(s, n.args, depth + 1);
                if (!n.named.empty()) {
                    output.requirePg("named function arguments");
                    if (!n.args.empty()) {
                        output.sql += ", ";
                    }
                    separated(n.named, [&](const auto& arg) { output.identifier(arg.name); output.sql += " => "; child(arg.expression); });
                }
                if (!n.orders.empty()) {
                    output.requirePg("ordered aggregate");
                    output.sql += " ORDER BY ";
                    orders(s, n.orders, depth + 1);
                }
                output.sql += ')';
                break;
            }
            case DbNodeKind::kBinary:
                binary(s, n, depth + 1);
                break;
            case DbNodeKind::kUnary: {
                output.sql += '(';
                switch (n.unary) {
                    case DbUnaryOperator::kNot:
                        output.sql += "NOT ";
                        child(n.left);
                        break;
                    case DbUnaryOperator::kNegate:
                        output.sql += '-';
                        child(n.left);
                        break;
                    case DbUnaryOperator::kBitNot:
                        output.sql += '~';
                        child(n.left);
                        break;
                    default:
                        child(n.left);
                        switch (n.unary) {
                            case DbUnaryOperator::kIsNull:
                                output.sql += " IS NULL";
                                break;
                            case DbUnaryOperator::kIsNotNull:
                                output.sql += " IS NOT NULL";
                                break;
                            case DbUnaryOperator::kIsTrue:
                                output.sql += " IS TRUE";
                                break;
                            case DbUnaryOperator::kIsFalse:
                                output.sql += " IS FALSE";
                                break;
                            case DbUnaryOperator::kIsNotTrue:
                                output.sql += " IS NOT TRUE";
                                break;
                            case DbUnaryOperator::kIsNotFalse:
                                output.sql += " IS NOT FALSE";
                                break;
                            default:
                                std::terminate();
                        }
                }
                output.sql += ')';
                break;
            }
            case DbNodeKind::kBetween:
                output.sql += '(';
                child(n.args[0]);
                output.sql += n.flag ? " NOT BETWEEN " : " BETWEEN ";
                child(n.args[1]);
                output.sql += " AND ";
                child(n.args[2]);
                output.sql += ')';
                break;
            case DbNodeKind::kTuple:
            case DbNodeKind::kList:
            case DbNodeKind::kArray:
                if (n.kind == DbNodeKind::kArray) {
                    output.requirePg("SQL arrays");
                    output.sql += "ARRAY[";
                } else {
                    if (n.args.empty()) {
                        throw std::invalid_argument("empty SQL tuple or list");
                    }
                    output.sql += n.kind == DbNodeKind::kTuple ? "ROW(" : "(";
                }
                expressions(s, n.args, depth + 1);
                output.sql += n.kind == DbNodeKind::kArray ? ']' : ')';
                break;
            case DbNodeKind::kAny:
            case DbNodeKind::kAll:
                output.requirePg("array ANY/ALL");
                output.sql += n.kind == DbNodeKind::kAny ? "ANY(" : "ALL(";
                child(n.left);
                output.sql += ')';
                break;
            case DbNodeKind::kCast:
                output.sql += "CAST(";
                child(n.left);
                output.sql += " AS ";
                output.type(n.type);
                output.sql += ')';
                break;
            case DbNodeKind::kAlias:
                if (!aliases) {
                    throw std::invalid_argument("an alias is only valid in a projection or RETURNING");
                }
                child(n.left);
                output.sql += " AS ";
                output.identifier(n.text);
                break;
            case DbNodeKind::kCase:
                output.sql += "CASE";
                for (std::size_t i = 0; i < n.args.size(); i += 2) {
                    output.sql += " WHEN ";
                    child(n.args[i]);
                    output.sql += " THEN ";
                    child(n.args[i + 1]);
                }
                if (n.left != noDbNode) {
                    output.sql += " ELSE ";
                    child(n.left);
                }
                output.sql += " END";
                break;
            case DbNodeKind::kExists:
            case DbNodeKind::kSubquery:
                if (n.kind == DbNodeKind::kExists) {
                    output.sql += "EXISTS ";
                }
                output.sql += '(';
                query(nested(s, n.query), depth + 1);
                output.sql += ')';
                break;
            case DbNodeKind::kFilter:
                output.requirePg("aggregate FILTER");
                child(n.left);
                output.sql += " FILTER (WHERE ";
                child(n.right);
                output.sql += ')';
                break;
            case DbNodeKind::kWindow: {
                child(n.left);
                output.sql += " OVER (";
                bool space = false;
                if (!n.args.empty()) {
                    output.sql += "PARTITION BY ";
                    expressions(s, n.args, depth + 1);
                    space = true;
                }
                if (!n.orders.empty()) {
                    if (space) {
                        output.sql += ' ';
                    }
                    output.sql += "ORDER BY ";
                    orders(s, n.orders, depth + 1);
                    space = true;
                }
                if (n.frame) {
                    if (space) {
                        output.sql += ' ';
                    }
                    const auto& frame = *n.frame;
                    if (frame.start.kind == DbFrameBoundary::kUnboundedFollowing || frame.end.kind == DbFrameBoundary::kUnboundedPreceding || frame.start.kind > frame.end.kind) {
                        throw std::invalid_argument("invalid window frame boundaries");
                    }
                    switch (frame.kind) {
                        case DbWindowFrame::kRows:
                            output.sql += "ROWS";
                            break;
                        case DbWindowFrame::kRange:
                            output.sql += "RANGE";
                            break;
                        case DbWindowFrame::kGroups:
                            output.requirePg("GROUPS window frame");
                            output.sql += "GROUPS";
                            break;
                    }
                    output.sql += " BETWEEN ";
                    boundary(frame.start);
                    output.sql += " AND ";
                    boundary(frame.end);
                }
                output.sql += ')';
                break;
            }
            case DbNodeKind::kWithinGroup:
                output.requirePg("ordered-set aggregate");
                child(n.left);
                output.sql += " WITHIN GROUP (ORDER BY ";
                orders(s, n.orders, depth + 1);
                output.sql += ')';
                break;
            case DbNodeKind::kExtract: {
                output.sql += "EXTRACT(";
                switch (n.datePart) {
                    case DbDatePart::kEpoch:
                        output.requirePg("EXTRACT EPOCH");
                        output.sql += "EPOCH";
                        break;
                    case DbDatePart::kYear:
                        output.sql += "YEAR";
                        break;
                    case DbDatePart::kMonth:
                        output.sql += "MONTH";
                        break;
                    case DbDatePart::kDay:
                        output.sql += "DAY";
                        break;
                    case DbDatePart::kHour:
                        output.sql += "HOUR";
                        break;
                    case DbDatePart::kMinute:
                        output.sql += "MINUTE";
                        break;
                    case DbDatePart::kSecond:
                        output.sql += "SECOND";
                        break;
                    case DbDatePart::kDow:
                        output.requirePg("EXTRACT DOW");
                        output.sql += "DOW";
                        break;
                    case DbDatePart::kDoy:
                        output.requirePg("EXTRACT DOY");
                        output.sql += "DOY";
                        break;
                    case DbDatePart::kWeek:
                        output.sql += "WEEK";
                        break;
                    case DbDatePart::kQuarter:
                        output.sql += "QUARTER";
                        break;
                }
                output.sql += " FROM ";
                child(n.left);
                output.sql += ')';
                break;
            }
            case DbNodeKind::kSubscript:
                output.requirePg("array subscript");
                output.sql += '(';
                child(n.left);
                output.sql += ")[";
                child(n.right);
                output.sql += ']';
                break;
            case DbNodeKind::kCollate:
                output.sql += '(';
                child(n.left);
                output.sql += " COLLATE ";
                output.qualified(n.text);
                output.sql += ')';
                break;
        }
    }
    void binary(const DbQueryStorage& s, const DbQueryNode& n, std::size_t depth) {
        const auto& left = s.nodes.at(n.left);
        const auto& right = s.nodes.at(n.right);
        const bool nullLeft = left.kind == DbNodeKind::kValue && DbValueAccess::type(left.value) == DbValueType::kNull;
        const bool nullRight = right.kind == DbNodeKind::kValue && DbValueAccess::type(right.value) == DbValueType::kNull;
        if ((n.binary == DbBinaryOperator::kEqual || n.binary == DbBinaryOperator::kNotEqual) && (nullLeft || nullRight)) {
            output.sql += '(';
            expr(s, nullLeft ? n.right : n.left, depth);
            output.sql += n.binary == DbBinaryOperator::kEqual ? " IS NULL)" : " IS NOT NULL)";
            return;
        }
        if ((n.binary == DbBinaryOperator::kIn || n.binary == DbBinaryOperator::kNotIn) && right.kind == DbNodeKind::kList && right.args.empty()) {
            output.sql += n.binary == DbBinaryOperator::kIn ? "FALSE" : "TRUE";
            return;
        }
        if (n.binary == DbBinaryOperator::kConcat && !output.pg()) {
            output.sql += "CONCAT(";
            expr(s, n.left, depth);
            output.sql += ", ";
            expr(s, n.right, depth);
            output.sql += ')';
            return;
        }
        const bool mariaDistinct = !output.pg() && n.binary == DbBinaryOperator::kIsDistinctFrom;
        output.sql += '(';
        if (mariaDistinct) {
            output.sql += "NOT (";
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
                output.requirePg("ILIKE");
                token = "ILIKE";
                break;
            case DbBinaryOperator::kNotILike:
                output.requirePg("NOT ILIKE");
                token = "NOT ILIKE";
                break;
            case DbBinaryOperator::kIn:
                token = "IN";
                break;
            case DbBinaryOperator::kNotIn:
                token = "NOT IN";
                break;
            case DbBinaryOperator::kIsDistinctFrom:
                token = output.pg() ? "IS DISTINCT FROM" : "<=>";
                break;
            case DbBinaryOperator::kIsNotDistinctFrom:
                token = output.pg() ? "IS NOT DISTINCT FROM" : "<=>";
                break;
            case DbBinaryOperator::kBitAnd:
                token = "&";
                break;
            case DbBinaryOperator::kBitOr:
                token = "|";
                break;
            case DbBinaryOperator::kBitXor:
                token = output.pg() ? "#" : "^";
                break;
            default:
                output.requirePg("PostgreSQL JSON, array, regex or network operator");
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
        output.sql += ' ';
        output.sql += token;
        output.sql += ' ';
        expr(s, n.right, depth);
        if (mariaDistinct) {
            output.sql += ')';
        }
        output.sql += ')';
    }
    void source(const DbQueryStorage& s, const DbQuerySource& value, std::size_t depth) {
        if (value.lateral) {
            output.requirePg("LATERAL");
            output.sql += "LATERAL ";
        }
        switch (value.kind) {
            case DbSourceKind::kTable:
                output.qualified(value.name);
                break;
            case DbSourceKind::kQuery:
                output.sql += '(';
                query(nested(s, value.query), depth + 1);
                output.sql += ')';
                break;
            case DbSourceKind::kFunction:
                output.requirePg("table function");
                expr(s, value.expression, depth + 1);
                break;
        }
        if (value.ordinality) {
            output.requirePg("WITH ORDINALITY");
            if (value.kind != DbSourceKind::kFunction) {
                throw std::invalid_argument("WITH ORDINALITY requires a table function");
            }
            output.sql += " WITH ORDINALITY";
        }
        if (!value.alias.empty()) {
            output.sql += " AS ";
            output.identifier(value.alias);
        }
        if (!value.columns.empty()) {
            if (value.alias.empty()) {
                throw std::invalid_argument("source column names require an alias");
            }
            output.sql += " (";
            const bool typed = value.columns.front().type.dataType != DbDataType::kInferred || !value.columns.front().type.customName.empty();
            if (typed && (value.kind != DbSourceKind::kFunction || value.ordinality)) {
                throw std::invalid_argument("record definitions require a function without ordinality");
            }
            separated(value.columns, [&](const auto& column) {
                const bool hasType = column.type.dataType != DbDataType::kInferred || !column.type.customName.empty();
                if (hasType != typed) {
                    throw std::invalid_argument("source column definitions must all specify a type or all omit it");
                }
                output.identifier(column.name);
                if (typed) {
                    output.sql += ' ';
                    const auto& t = column.type;
                    appendDbTypeName(output.sql, t.dataType, t.customName, t.length, t.precision, t.scale, t.array, output.driver());
                }
            });
            output.sql += ')';
        }
    }
    void sources(const DbQueryStorage& s, std::size_t depth) {
        source(s, *s.source, depth);
        for (const auto& join : s.joins) {
            switch (join.type) {
                case DbJoinType::kInner:
                    output.sql += " INNER JOIN ";
                    break;
                case DbJoinType::kLeft:
                    output.sql += " LEFT JOIN ";
                    break;
                case DbJoinType::kRight:
                    output.sql += " RIGHT JOIN ";
                    break;
                case DbJoinType::kFull:
                    output.requirePg("FULL JOIN");
                    output.sql += " FULL JOIN ";
                    break;
                case DbJoinType::kCross:
                    output.sql += " CROSS JOIN ";
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
                output.sql += " ON ";
                expr(s, join.on, depth + 1);
            }
            if (!join.usingColumns.empty()) {
                output.sql += " USING (";
                identifiers(join.usingColumns);
                output.sql += ')';
            }
        }
    }
    void assignments(const DbQueryStorage& s, const std::pmr::vector<DbStoredAssignment>& values, std::size_t depth) {
        if (values.empty()) {
            throw std::invalid_argument("UPDATE requires assignments");
        }
        separated(values, [&](const auto& assignment) { output.identifier(assignment.column); output.sql += " = "; expr(s, assignment.expression, depth + 1); });
    }
    void conflict(const DbQueryStorage& s, std::size_t depth) {
        const auto& value = *s.conflict;
        if (value.doNothing && (!value.assignments.empty() || value.updateWhere != noDbNode)) {
            throw std::invalid_argument("DO NOTHING cannot specify updates");
        }
        if (output.pg()) {
            if (value.anyUniqueKey) {
                throw std::invalid_argument("anyUniqueKey is specific to MariaDB");
            }
            if (!value.columns.empty() && !value.constraint.empty()) {
                throw std::invalid_argument("conflict target cannot have both columns and a constraint");
            }
            if (value.targetWhere != noDbNode && value.columns.empty()) {
                throw std::invalid_argument("a conflict predicate requires target columns");
            }
            output.sql += " ON CONFLICT";
            if (!value.columns.empty()) {
                output.sql += " (";
                identifiers(value.columns);
                output.sql += ')';
            }
            if (!value.constraint.empty()) {
                output.sql += " ON CONSTRAINT ";
                output.identifier(value.constraint);
            }
            if (value.targetWhere != noDbNode) {
                output.sql += " WHERE ";
                expr(s, value.targetWhere, depth + 1);
            }
            if (value.doNothing) {
                output.sql += " DO NOTHING";
                return;
            }
            if (value.columns.empty() && value.constraint.empty()) {
                throw std::invalid_argument("DO UPDATE requires a conflict target");
            }
            output.sql += " DO UPDATE SET ";
            assignments(s, value.assignments, depth);
            if (value.updateWhere != noDbNode) {
                output.sql += " WHERE ";
                expr(s, value.updateWhere, depth + 1);
            }
        } else {
            if (!value.anyUniqueKey || !value.columns.empty() || !value.constraint.empty() || value.targetWhere != noDbNode || value.updateWhere != noDbNode || value.doNothing) {
                output.unsupported("targeted or conditional upsert / DO NOTHING");
            }
            output.sql += " ON DUPLICATE KEY UPDATE ";
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
        output.sql += "VALUES ";
        separated(s.rows, [&](const auto& row) {
            if (row.size() != width) {
                throw std::invalid_argument("VALUES rows have different widths");
            }
            output.sql += '(';
            expressions(s, row, depth + 1);
            output.sql += ')';
        });
    }
    void body(const DbQueryStorage& s, std::size_t depth) {
        switch (s.kind) {
            case DbQueryKind::kSelect:
                output.sql += "SELECT ";
                if (s.distinct) {
                    output.sql += "DISTINCT ";
                }
                if (!s.distinctOn.empty()) {
                    output.requirePg("DISTINCT ON");
                    output.sql += "DISTINCT ON (";
                    expressions(s, s.distinctOn, depth + 1);
                    output.sql += ") ";
                }
                if (s.projections.empty()) {
                    output.sql += '*';
                } else {
                    expressions(s, s.projections, depth + 1, true);
                }
                if (s.source) {
                    output.sql += " FROM ";
                    sources(s, depth + 1);
                }
                break;
            case DbQueryKind::kValues:
                rows(s, depth);
                break;
            case DbQueryKind::kInsert:
                output.sql += "INSERT INTO ";
                output.qualified(s.target);
                if (!s.targetAlias.empty()) {
                    output.requirePg("INSERT target alias");
                    output.sql += " AS ";
                    output.identifier(s.targetAlias);
                }
                if (!s.columns.empty()) {
                    output.sql += " (";
                    identifiers(s.columns);
                    output.sql += ')';
                }
                output.sql += ' ';
                if (s.insertQuery != noDbNode) {
                    if (!s.rows.empty()) {
                        throw std::invalid_argument("INSERT cannot have both VALUES and SELECT");
                    }
                    query(nested(s, s.insertQuery), depth + 1);
                } else if (s.rows.empty()) {
                    if (!s.columns.empty()) {
                        throw std::invalid_argument("INSERT column list requires input rows");
                    }
                    output.sql += output.pg() ? "DEFAULT VALUES" : "() VALUES ()";
                } else {
                    rows(s, depth);
                }
                if (s.conflict) {
                    conflict(s, depth + 1);
                }
                break;
            case DbQueryKind::kUpdate:
            case DbQueryKind::kDelete:
                output.sql += s.kind == DbQueryKind::kUpdate ? "UPDATE " : "DELETE FROM ";
                output.qualified(s.target);
                if (!s.targetAlias.empty()) {
                    if (!output.pg() && s.kind == DbQueryKind::kDelete) {
                        output.unsupported("DELETE target alias");
                    }
                    output.sql += " AS ";
                    output.identifier(s.targetAlias);
                }
                if (s.kind == DbQueryKind::kUpdate) {
                    output.sql += " SET ";
                    assignments(s, s.assignments, depth + 1);
                }
                if (s.source) {
                    output.requirePg("UPDATE FROM / DELETE USING");
                    output.sql += s.kind == DbQueryKind::kUpdate ? " FROM " : " USING ";
                    sources(s, depth + 1);
                }
                break;
        }
        if (s.predicate != noDbNode) {
            output.sql += " WHERE ";
            expr(s, s.predicate, depth + 1);
        }
        if (!s.groups.empty()) {
            output.sql += " GROUP BY ";
            expressions(s, s.groups, depth + 1);
        }
        if (s.having != noDbNode) {
            output.sql += " HAVING ";
            expr(s, s.having, depth + 1);
        }
    }
};

}  // namespace ruvia::detail

namespace ruvia {

DbStatement DbQuery::compile(DbDriver driver, std::pmr::memory_resource* resource, DbParameterMode mode) const {
    detail::DbQueryCompiler compiler(driver, detail::pmrResourceOrDefault(resource), mode);
    compiler.query(storage());
    return DbStatement(std::move(compiler.output.sql), std::move(compiler.output.params), returnsRows());
}

std::pmr::string DbQuery::renderExpression(Expr expression, DbDriver driver,
    std::pmr::memory_resource* resource, DbParameterMode mode) {
    if (mode != DbParameterMode::kLiteral) {
        throw std::invalid_argument("standalone expression rendering requires literal mode");
    }
    detail::DbQueryCompiler compiler(driver, detail::pmrResourceOrDefault(resource), mode);
    compiler.expression(expression);
    return std::move(compiler.output.sql);
}

}  // namespace ruvia
