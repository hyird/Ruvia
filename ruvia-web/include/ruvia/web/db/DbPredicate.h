#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/web/db/DbQuery.h"
#include "ruvia/web/detail/db/DbParameterPack.h"

namespace ruvia {

// An owning condition. Literal arguments are copied during construction, so a
// condition may outlive the strings used to build it.
class DbPredicate final {
public:
    DbPredicate() = default;
    explicit DbPredicate(DbExpression expression) {
        if (!expression.empty()) {
            query_.emplace();
            expression_ = query_->importExpression(expression);
        }
    }
    DbPredicate(const DbPredicate&) = delete;
    DbPredicate& operator=(const DbPredicate&) = delete;
    DbPredicate(DbPredicate&&) noexcept = default;
    DbPredicate& operator=(DbPredicate&&) noexcept = default;
    [[nodiscard]] bool empty() const noexcept {
        return expression_.empty();
    }
    [[nodiscard]] DbExpression expression(DbQuery& query, std::string_view table = {}, std::string_view alias = {}) const {
        return empty() ? DbExpression{} : query.importExpression(expression_, alias.empty() ? std::string_view{} : table, alias);
    }
    friend DbPredicate operator&&(DbPredicate left, const DbPredicate& right) {
        return combine(std::move(left), DbBinaryOperator::kAnd, right);
    }
    friend DbPredicate operator||(DbPredicate left, const DbPredicate& right) {
        return combine(std::move(left), DbBinaryOperator::kOr, right);
    }
    friend DbPredicate operator!(DbPredicate value) {
        if (value.empty()) {
            throw std::invalid_argument("cannot negate an empty database condition");
        }
        value.expression_ = value.query_->unary(DbUnaryOperator::kNot, value.expression_);
        return value;
    }

private:
    template <typename, FixedString>
    friend class DbFieldReference;
    static DbPredicate combine(DbPredicate left, DbBinaryOperator op, const DbPredicate& right) {
        if (left.empty() || right.empty()) {
            throw std::invalid_argument("cannot combine empty database conditions");
        }
        left.expression_ = left.query_->binary(left.expression_, op, right.expression(*left.query_));
        return left;
    }
    std::optional<DbQuery> query_{};
    DbExpression expression_{};
};

template <typename E, FixedString Name>
class DbFieldReference final {
    static_assert(E::template columnIndex<Name>() < std::tuple_size_v<typename E::Columns>);

public:
    [[nodiscard]] static constexpr std::string_view name() noexcept {
        return Name.view();
    }
    [[nodiscard]] DbExpression expression(DbQuery& query, std::string_view alias = {}) const {
        return query.column(Name.view(), alias.empty() ? E::tableName() : alias);
    }
    template <detail::DbParameter V>
    DbPredicate operator==(V&& value) const {
        return compare(DbBinaryOperator::kEqual, std::forward<V>(value));
    }
    template <detail::DbParameter V>
    DbPredicate operator!=(V&& value) const {
        return compare(DbBinaryOperator::kNotEqual, std::forward<V>(value));
    }
    template <detail::DbParameter V>
    DbPredicate operator<(V&& value) const {
        return compare(DbBinaryOperator::kLess, std::forward<V>(value));
    }
    template <detail::DbParameter V>
    DbPredicate operator<=(V&& value) const {
        return compare(DbBinaryOperator::kLessEqual, std::forward<V>(value));
    }
    template <detail::DbParameter V>
    DbPredicate operator>(V&& value) const {
        return compare(DbBinaryOperator::kGreater, std::forward<V>(value));
    }
    template <detail::DbParameter V>
    DbPredicate operator>=(V&& value) const {
        return compare(DbBinaryOperator::kGreaterEqual, std::forward<V>(value));
    }
    [[nodiscard]] DbPredicate like(std::string_view pattern) const {
        return compare(DbBinaryOperator::kLike, pattern);
    }
    [[nodiscard]] DbPredicate ilike(std::string_view pattern) const {
        return compare(DbBinaryOperator::kILike, pattern);
    }
    template <detail::DbParameter Lower, detail::DbParameter Upper>
    [[nodiscard]] DbPredicate between(Lower&& lower, Upper&& upper) const {
        DbPredicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        result.expression_ = query.between(expression(query),
            query.value(detail::makeImmediateDbParameter(std::forward<Lower>(lower))),
            query.value(detail::makeImmediateDbParameter(std::forward<Upper>(upper))));
        return result;
    }
    [[nodiscard]] DbPredicate isNull() const {
        return compare(DbBinaryOperator::kEqual, nullptr);
    }
    [[nodiscard]] DbPredicate isNotNull() const {
        return compare(DbBinaryOperator::kNotEqual, nullptr);
    }
    template <typename T>
    [[nodiscard]] DbPredicate in(std::span<const T> values) const {
        DbPredicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        std::pmr::vector<DbExpression> args(query.resource());
        for (const auto& value : values) {
            args.push_back(query.value(detail::makeImmediateDbParameter(value)));
        }
        result.expression_ = query.binary(expression(query), DbBinaryOperator::kIn, query.list(args));
        return result;
    }
    template <typename T>
    [[nodiscard]] DbPredicate in(std::initializer_list<T> values) const {
        return in(std::span<const T>(values.begin(), values.size()));
    }

    template <typename T>
    [[nodiscard]] DbPredicate arrayContains(std::span<const T> values) const {
        return compareArray(DbBinaryOperator::kArrayContains, values);
    }
    template <typename T>
    [[nodiscard]] DbPredicate arrayContains(std::initializer_list<T> values) const {
        return arrayContains(std::span<const T>(values.begin(), values.size()));
    }
    template <typename T>
    [[nodiscard]] DbPredicate arrayContainedBy(std::span<const T> values) const {
        return compareArray(DbBinaryOperator::kArrayContainedBy, values);
    }
    template <typename T>
    [[nodiscard]] DbPredicate arrayContainedBy(std::initializer_list<T> values) const {
        return arrayContainedBy(std::span<const T>(values.begin(), values.size()));
    }
    template <typename T>
    [[nodiscard]] DbPredicate arrayOverlap(std::span<const T> values) const {
        return compareArray(DbBinaryOperator::kArrayOverlap, values);
    }
    template <typename T>
    [[nodiscard]] DbPredicate arrayOverlap(std::initializer_list<T> values) const {
        return arrayOverlap(std::span<const T>(values.begin(), values.size()));
    }

private:
    template <typename T>
    DbPredicate compareArray(DbBinaryOperator op, std::span<const T> values) const {
        using Column = std::tuple_element_t<E::template columnIndex<Name>(), typename E::Columns>;
        static_assert(detail::IsPmrVector<typename Column::value_type>::value, "array predicates require an array column");
        using Item = typename detail::IsPmrVector<typename Column::value_type>::value_type;
        constexpr auto type = Column::options.dataType == DbDataType::kInferred || Column::options.dataType == DbDataType::kArray
                                  ? detail::DbEntityTypeTraits<Item>::dataType
                                  : Column::options.dataType;
        DbPredicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        std::pmr::vector<DbExpression> args(query.resource());
        args.reserve(values.size());
        for (const auto& value : values) {
            args.push_back(query.value(detail::makeImmediateDbParameter(value)));
        }
        const auto array = query.cast(query.array(args), {.dataType = type,
                                                             .length = Column::options.length,
                                                             .precision = Column::options.precision,
                                                             .scale = Column::options.scale,
                                                             .array = true});
        result.expression_ = query.binary(expression(query), op, array);
        return result;
    }
    template <detail::DbParameter V>
    DbPredicate compare(DbBinaryOperator op, V&& value) const {
        DbPredicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        result.expression_ = query.binary(expression(query), op, query.value(detail::makeImmediateDbParameter(std::forward<V>(value))));
        return result;
    }
};

template <FixedString Table, typename... Columns>
template <FixedString Name>
DbFieldReference<DbEntity<Table, Columns...>, Name> DbEntity<Table, Columns...>::column() {
    return {};
}

}  // namespace ruvia
