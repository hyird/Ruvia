#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/db/db_expressions.h"
#include "ruvia/web/db/db_find_options.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_projection.h"
#include "ruvia/web/detail/db/db_entity_codec.h"
#include "ruvia/web/detail/db/db_relation_query.h"
#include "ruvia/web/detail/db/db_result_access.h"

namespace ruvia {

template <typename entity_type, typename executor_type>
class db_write_query_builder;

struct db_selection final {
    std::string column_{};
    db_expression expression_{};
};

struct db_upsert_options final {
    std::vector<std::string> conflict_paths_{};
    std::vector<std::string> update_columns_{};
    bool do_nothing_{false};
    bool any_unique_key_{false};
    bool skip_update_if_no_values_changed_{false};
    db_predicate index_predicate_{};
    std::vector<db_assignment> update_expressions_{};
    db_expression update_where_{};
};

namespace detail {

template <typename t_type>
concept db_write_condition = std::same_as<t_type, db_predicate> || std::same_as<t_type, db_expression>;

inline db_expression write_condition(db_query& query, const db_predicate& predicate, std::string_view table = {}, std::string_view alias = {}) {
    return predicate.expression(query, table, alias);
}
inline db_expression write_condition(db_query& query, db_expression predicate, std::string_view = {}, std::string_view = {}) {
    return query.import_expression(predicate);
}

template <typename e_type, typename fn_type>
void for_each_entity_column(fn_type&& fn) {
    [&]<std::size_t... i>(std::index_sequence<i...>) {
        (fn.template operator()<std::tuple_element_t<i, typename e_type::columns_type>>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<typename e_type::columns_type>>{});
}
template <typename e_type>
constexpr bool has_entity_column(std::string_view name) noexcept {
    return [&]<typename... columns_type>(std::tuple<columns_type...>*) {
        return ((name == columns_type::name.view()) || ...);
    }(static_cast<typename e_type::columns_type*>(nullptr));
}
template <typename e_type>
void require_entity_column(std::string_view name) {
    if (!has_entity_column<e_type>(name)) {
        throw std::invalid_argument("unknown database entity column");
    }
}
template <typename e_type>
bool is_generated_entity_column(std::string_view name) {
    bool generated = false;
    for_each_entity_column<e_type>([&]<typename c_type> {
        if constexpr (c_type::options.generated_type_ != db_generated_type::none) {
            generated |= name == c_type::name.view();
        }
    });
    return generated;
}
template <typename e_type>
void select_entity(db_query& query, std::string_view alias) {
    const auto qualifier = alias.empty() ? e_type::table_name() : alias;
    for_each_entity_column<e_type>([&]<typename c_type> { query.add_select(query.column(c_type::name.view(), qualifier)); });
    query.from(e_type::table_name(), alias);
}
template <typename e_type>
constexpr std::string_view entity_query_alias() noexcept {
    const auto table_value = e_type::table_name();
    const auto dot = table_value.rfind('.');
    return table_value.substr(dot == std::string_view::npos ? 0 : dot + 1);
}
template <typename e_type>
void apply_find_options(db_query& query, const db_find_options& options, std::string_view alias = {}) {
    query.cache(options.cache_);
    if (!options.where_.empty()) {
        query.where(options.where_.expression(query, e_type::table_name(), alias));
    }
    for (const auto& order : options.order_) {
        require_entity_column<e_type>(order.column_);
        query.add_order_by(query.column(order.column_, alias.empty() ? e_type::table_name() : alias), order.direction_, order.nulls_);
    }
    query.offset(options.skip_).limit(options.take_);
    if (options.lock_) {
        query.lock(*options.lock_);
    }
}
template <typename e_type>
struct db_map_entity_rows final {
    entity_rows<e_type> operator()(db_rows&& rows, std::pmr::memory_resource* resource) const {
        return map_entity_rows<e_type>(std::move(rows), resource);
    }
};
template <typename e_type>
struct db_map_one_entity final {
    std::optional<e_type> operator()(db_rows&& rows, std::pmr::memory_resource* resource) const {
        if (rows.empty()) {
            return std::nullopt;
        }
        e_type entity(resource);
        decode_entity(entity, rows.front(), resource, static_cast<typename e_type::columns_type*>(nullptr),
            std::make_index_sequence<std::tuple_size_v<typename e_type::columns_type>>{});
        return std::optional<e_type>(std::in_place, std::move(entity));
    }
};
struct db_map_count final {
    std::uint64_t operator()(db_rows&& rows, std::pmr::memory_resource*) const {
        return db_count_value(rows);
    }
};
struct db_map_exists final {
    bool operator()(db_rows&& rows, std::pmr::memory_resource*) const {
        if (rows.size() != 1) {
            throw std::runtime_error("database exists did not return exactly one row");
        }
        return rows.front()["exists"].template as<bool>().value();
    }
};
template <typename e_type, typename c_type>
db_expression entity_column_value(db_query& query, const e_type& entity) {
    if (!entity.template is_set<c_type::name>()) {
        return query.default_value();
    }
    if (entity.template is_null<c_type::name>()) {
        return query.null_value();
    }
    return query.value(entity_db_value(entity.template get<c_type::name>(), query.resource()));
}

template <typename e_type>
struct db_map_projection final {
    static_assert(sql_entity<e_type> || sql_projection<e_type>, "SQL results require a SQL entity or a db_projection");
    explicit db_map_projection(std::span<const std::pmr::string> selected) {
        for (const auto& name : selected) {
            require_entity_column<e_type>(name);
        }
        std::size_t index = 0;
        for_each_entity_column<e_type>([&]<typename c_type> {
            selected_columns_[index++] = selected.empty() || std::ranges::find(selected, c_type::name.view()) != selected.end();
        });
    }
    e_type decode(const db_row& row, std::pmr::memory_resource* resource) const {
        e_type result(resource);
        std::size_t index = 0;
        for_each_entity_column<e_type>([&]<typename c_type> {
            if (selected_columns_[index++]) {
                decode_entity_column<e_type, c_type>(result, row, resource);
            }
        });
        return result;
    }
    entity_rows<e_type> operator()(db_rows&& rows, std::pmr::memory_resource* resource) const {
        auto result_value = db_result_access::make_entity_rows<e_type>(resource, rows.size());
        db_entity_row_decoder<e_type> decoder(selected_columns_);
        for (const auto& row : rows) {
            result_value.push_back(decoder.decode(row, resource));
        }
        return result_value;
    }

private:
    std::array<bool, std::tuple_size_v<typename e_type::columns_type>> selected_columns_{};
};
template <typename e_type>
struct db_map_one_projection final {
    db_map_projection<e_type> mapper_;
    std::optional<e_type> operator()(db_rows&& rows, std::pmr::memory_resource* resource) const {
        if (rows.empty()) {
            return std::nullopt;
        }
        return mapper_.decode(rows.front(), resource);
    }
};
inline std::pmr::vector<std::pmr::string> apply_projection(db_query& query, std::span<const db_selection> selection,
    std::string_view qualifier, bool returning = false) {
    if (selection.empty()) {
        throw std::invalid_argument("projection requires at least one field");
    }
    std::pmr::vector<std::pmr::string> columns(query.resource());
    std::pmr::vector<db_expression> values(query.resource());
    for (const auto& field : selection) {
        if (field.column_.empty() || std::ranges::find(columns, std::string_view(field.column_)) != columns.end()) {
            throw std::invalid_argument("projection requires unique nonempty field names");
        }
        columns.emplace_back(field.column_);
        auto expression = field.expression_.empty() ? query.column(field.column_, qualifier) : query.import_expression(field.expression_);
        values.push_back(query.alias(expression, field.column_));
    }
    if (returning) {
        query.returning(values);
    } else {
        query.select(values);
    }
    return columns;
}

template <typename executor_type>
using db_repository_executor_type = std::conditional_t<std::same_as<executor_type, db_handle>, db_handle, executor_type&>;
template <typename executor_type>
using db_repository_input_type = std::conditional_t<std::same_as<executor_type, db_handle>, const db_handle&, executor_type&>;

}  // namespace detail

template <typename entity_type, typename executor_type>
class db_query_builder final {
public:
    db_query_builder(const db_query_builder&) = delete;
    db_query_builder& operator=(const db_query_builder&) = delete;
    db_query_builder(db_query_builder&&) = default;
    db_query_builder& operator=(db_query_builder&&) = delete;

    db_query_builder& select(std::span<const db_selection> fields_value) {
        require_plain_projection();
        for (const auto& field : fields_value) {
            if (field.expression_.empty()) {
                detail::require_entity_column<entity_type>(field.column_);
            }
        }
        selected_ = detail::apply_projection(query_, fields_value, alias_);
        return *this;
    }
    db_query_builder& select(std::initializer_list<db_selection> fields_value) {
        return select(std::span<const db_selection>(fields_value.begin(), fields_value.size()));
    }
    template <typename joined_type>
    db_query_builder& join(db_join_type type, std::string_view alias, db_expression on = {}) {
        static_assert(sql_entity<joined_type>, "SQL joins require a SQL entity");
        query_.join(type, joined_type::table_name(), (on.empty() ? db_expression{} : query_.import_expression(on)), alias);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_query_builder& join(db_join_type type, const db_query_builder<source, source_executor_type>& source_value,
        std::string_view alias, db_expression on = {}, const db_source_options& options = {}) {
        source_value.require_plain_projection();
        query_.join(type, source_value.query_, (on.empty() ? db_expression{} : query_.import_expression(on)), alias, options);
        return *this;
    }
    db_query_builder& join_cte(db_join_type type, std::string_view name, std::string_view alias, db_expression on = {}) {
        query_.join(type, name, (on.empty() ? db_expression{} : query_.import_expression(on)), alias);
        return *this;
    }
    db_query_builder& join_function(db_join_type type, db_expression function, std::string_view alias,
        db_expression on = {}, const db_source_options& options = {}) {
        query_.join_function(type, query_.import_expression(function), (on.empty() ? db_expression{} : query_.import_expression(on)), alias, options);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_query_builder& with(std::string_view name, const db_query_builder<source, source_executor_type>& source_value, const db_cte_options& options = {}) {
        source_value.require_plain_projection();
        query_.with(name, source_value.query_, options);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_query_builder& with(std::string_view name, const db_write_query_builder<source, source_executor_type>& source_value, const db_cte_options& options = {}) {
        require_plain_projection();
        source_value.validate();
        query_.with(name, source_value.query_, options);
        return *this;
    }
    db_query_builder& from_cte(std::string_view name) {
        require_plain_projection();
        query_.from(name, alias_);
        return *this;
    }
    db_expression subquery(db_expressions& expressions) const {
        require_plain_projection();
        return expressions.query_.subquery(query_);
    }
    db_expression exists(db_expressions& expressions) const {
        require_plain_projection();
        return expressions.query_.exists(query_);
    }
    template <typename source, typename source_executor_type>
    db_query_builder& combine(db_set_operation operation, const db_query_builder<source, source_executor_type>& source_value) {
        require_plain_projection();
        source_value.require_plain_projection();
        query_.combine(operation, source_value.query_);
        return *this;
    }
    db_query_builder& group_by(std::span<const db_expression> expressions) {
        std::pmr::vector<db_expression> imported(query_.resource());
        for (auto expression : expressions) {
            imported.push_back(query_.import_expression(expression));
        }
        query_.group_by(imported);
        return *this;
    }
    db_query_builder& group_by(std::initializer_list<db_expression> expressions) {
        return group_by(std::span<const db_expression>(expressions.begin(), expressions.size()));
    }
    db_query_builder& having(db_expression expression) {
        query_.having(query_.import_expression(expression));
        return *this;
    }
    db_query_builder& and_having(db_expression expression) {
        query_.and_having(query_.import_expression(expression));
        return *this;
    }
    db_query_builder& where(db_expression expression) {
        query_.where(query_.import_expression(expression));
        return *this;
    }
    db_query_builder& and_where(db_expression expression) {
        query_.and_where(query_.import_expression(expression));
        return *this;
    }
    db_query_builder& or_where(db_expression expression) {
        query_.or_where(query_.import_expression(expression));
        return *this;
    }
    db_query_builder& order_by(db_expression expression, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value) {
        query_.order_by(query_.import_expression(expression), direction, nulls);
        return *this;
    }
    db_query_builder& add_order_by(db_expression expression, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value) {
        query_.add_order_by(query_.import_expression(expression), direction, nulls);
        return *this;
    }
    db_query_builder& distinct(bool enabled = true) {
        query_.distinct(enabled);
        return *this;
    }
    db_query_builder& where(const db_predicate& predicate) {
        query_.where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& and_where(const db_predicate& predicate) {
        query_.and_where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& or_where(const db_predicate& predicate) {
        query_.or_where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& order_by(std::string_view column, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value) {
        detail::require_entity_column<entity_type>(column);
        query_.order_by(query_.column(column, alias_), direction, nulls);
        return *this;
    }
    db_query_builder& add_order_by(std::string_view column, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value) {
        detail::require_entity_column<entity_type>(column);
        query_.add_order_by(query_.column(column, alias_), direction, nulls);
        return *this;
    }
    db_query_builder& skip(std::uint64_t count) {
        query_.offset(count);
        return *this;
    }
    db_query_builder& take(std::uint64_t count) {
        query_.limit(count);
        return *this;
    }
    db_query_builder& left_join_and_select(std::string_view relation, std::string_view alias) {
        return join_and_select(relation, alias, db_join_type::left);
    }
    db_query_builder& inner_join_and_select(std::string_view relation, std::string_view alias) {
        return join_and_select(relation, alias, db_join_type::inner);
    }
    db_query_builder& cache(const db_cache_setting_type& setting) {
        query_.cache(setting);
        return *this;
    }
    db_query_builder& cache(std::string_view id, std::optional<std::chrono::milliseconds> milliseconds = {}) {
        query_.cache(id, milliseconds);
        return *this;
    }
    db_query_builder& set_lock(const db_lock_options& options) {
        query_.lock(options);
        return *this;
    }
    template <typename output_type = entity_type>
    [[nodiscard]] scoped_operation<entity_rows<output_type>> get_many() const {
        auto mapper = projection_mapper<output_type>();
        if constexpr (std::same_as<output_type, entity_type>) {
            if (relations_ && !relations_->empty()) {
                auto prepared = relations_->template prepare<entity_type>(query_, alias_, executor_.query_driver());
                return executor_.template query_mapped<entity_rows<entity_type>>(prepared ? *prepared : query_,
                    detail::db_map_related_entities<entity_type>{relations_->clone()});
            }
        }
        return executor_.template query_mapped<entity_rows<output_type>>(query_, std::move(mapper));
    }
    template <typename output_type = entity_type>
    [[nodiscard]] scoped_operation<std::optional<output_type>> get_one() const {
        auto mapper = projection_mapper<output_type>();
        auto query = query_.clone(query_.resource());
        query.limit(1);
        if constexpr (std::same_as<output_type, entity_type>) {
            if (relations_ && !relations_->empty()) {
                auto prepared = relations_->template prepare<entity_type>(query, alias_, executor_.query_driver());
                return executor_.template query_mapped<std::optional<entity_type>>(prepared ? *prepared : query,
                    detail::db_map_one_related_entity<entity_type>{relations_->clone()});
            }
        }
        return executor_.template query_mapped<std::optional<output_type>>(query, detail::db_map_one_projection<output_type>{std::move(mapper)});
    }
    [[nodiscard]] scoped_operation<std::uint64_t> get_count() const {
        const auto count = count_query();
        return executor_.template query_mapped<std::uint64_t>(count, detail::db_map_count{});
    }
    [[nodiscard]] scoped_operation<bool> get_exists() const {
        auto source_value = query_.clone(query_.resource());
        source_value.clear_order().clear_lock().limit(std::nullopt).offset(std::nullopt);
        db_query query(query_.resource());
        query.select(query.alias(query.exists(source_value), "exists"));
        query.copy_cache(query_);
        return executor_.template query_mapped<bool>(query, detail::db_map_exists{});
    }
    template <typename output_type = entity_type>
    [[nodiscard]] scoped_operation<std::pair<entity_rows<output_type>, std::uint64_t>> get_many_and_count() const {
        if (query_.has_writes()) {
            throw std::invalid_argument("get_many_and_count cannot execute a write CTE twice");
        }
        auto mapper = projection_mapper<output_type>();
        auto count = count_query();
        count.copy_cache(query_, "-count");
        if constexpr (std::same_as<output_type, entity_type>) {
            if (relations_ && !relations_->empty()) {
                auto prepared = relations_->template prepare<entity_type>(query_, alias_, executor_.query_driver());
                return executor_.template query_mapped_and_count<entity_rows<entity_type>>(prepared ? *prepared : query_, count,
                    detail::db_map_related_entities<entity_type>{relations_->clone()});
            }
        }
        return executor_.template query_mapped_and_count<entity_rows<output_type>>(query_, count, std::move(mapper));
    }
    [[nodiscard]] db_statement get_query_and_parameters() const {
        if (relations_ && !relations_->empty()) {
            auto prepared = relations_->template prepare<entity_type>(query_, alias_, executor_.query_driver());
            return (prepared ? *prepared : query_).compile(executor_.query_driver(), query_.resource());
        }
        return query_.compile(executor_.query_driver(), query_.resource());
    }

private:
    friend class db_repository<entity_type, executor_type>;
    template <typename, typename>
    friend class db_query_builder;
    template <typename, typename>
    friend class db_write_query_builder;
    void require_plain_projection() const {
        if (relations_ && !relations_->empty()) {
            throw std::invalid_argument("explicit projections and subqueries require joins without relation hydration");
        }
    }
    template <typename output_type>
    detail::db_map_projection<output_type> projection_mapper() const {
        if constexpr (!std::same_as<output_type, entity_type>) {
            require_plain_projection();
            if (selected_.empty()) {
                throw std::invalid_argument("DTO mapping requires an explicit projection");
            }
        }
        return detail::db_map_projection<output_type>(selected_);
    }
    db_query_builder(detail::db_repository_input_type<executor_type> executor, std::string_view alias)
        : executor_(executor),
          query_(executor.query_resource()),
          alias_(alias.empty() ? detail::entity_query_alias<entity_type>() : alias, query_.resource()),
          selected_(query_.resource()) {
        detail::select_entity<entity_type>(query_, alias_);
    }
    [[nodiscard]] db_query count_query() const {
        auto source_value = query_.clone(query_.resource());
        source_value.clear_order().clear_lock().limit(std::nullopt).offset(std::nullopt);
        if (relations_ && !relations_->empty()) {
            db_query roots(query_.resource());
            constexpr auto keys = detail::db_primary_key_columns<entity_type>();
            for (const auto key : keys) {
                roots.add_select(roots.column(key, "__ruvia_count_roots"));
            }
            roots.from(source_value, "__ruvia_count_roots").distinct();
            source_value = std::move(roots);
        }
        db_query count(query_.resource());
        const std::array args{count.star()};
        count.select(count.alias(count.aggregate("count", args), "count")).from(source_value, "count_source");
        count.copy_cache(query_);
        return count;
    }
    db_query_builder& join_and_select(std::string_view relation, std::string_view alias, db_join_type join) {
        if (query_.has_writes()) {
            throw std::invalid_argument("write CTEs require flat result mapping");
        }
        if (!selected_.empty()) {
            throw std::invalid_argument("relation hydration requires a full entity projection");
        }
        if (alias.empty()) {
            throw std::invalid_argument("relation joins require an alias");
        }
        if (!relations_) {
            relations_.emplace(query_.resource());
        }
        relations_->template add<entity_type>(query_, alias_, relation, alias, join);
        return *this;
    }
    detail::db_repository_executor_type<executor_type> executor_;
    db_query query_;
    std::pmr::string alias_;
    std::pmr::vector<std::pmr::string> selected_;
    std::optional<detail::db_relation_plan> relations_{};
};

template <typename entity_type, typename executor_type>
class db_write_query_builder final {
public:
    db_write_query_builder(const db_write_query_builder&) = delete;
    db_write_query_builder& operator=(const db_write_query_builder&) = delete;
    db_write_query_builder(db_write_query_builder&&) = default;
    db_write_query_builder& operator=(db_write_query_builder&&) = delete;

    template <detail::db_write_condition condition_type = db_predicate>
    db_write_query_builder& where(const condition_type& predicate) {
        require_condition(predicate);
        query_.where(detail::write_condition(query_, predicate, entity_type::table_name(), alias_));
        return *this;
    }
    template <detail::db_write_condition condition_type = db_predicate>
    db_write_query_builder& and_where(const condition_type& predicate) {
        require_condition(predicate);
        query_.and_where(detail::write_condition(query_, predicate, entity_type::table_name(), alias_));
        return *this;
    }
    template <detail::db_write_condition condition_type = db_predicate>
    db_write_query_builder& or_where(const condition_type& predicate) {
        require_condition(predicate);
        query_.or_where(detail::write_condition(query_, predicate, entity_type::table_name(), alias_));
        return *this;
    }
    db_write_query_builder& set(std::string_view column, db_expression value) {
        detail::require_entity_column<entity_type>(column);
        if (detail::is_generated_entity_column<entity_type>(column) || value.empty()) {
            throw std::invalid_argument("SET requires a writable entity column and expression");
        }
        query_.set(column, query_.import_expression(value));
        return *this;
    }
    db_write_query_builder& set(const entity_type& changes) {
        detail::for_each_entity_column<entity_type>([&]<typename c_type> {
            if constexpr (c_type::options.generated_type_ == db_generated_type::none) {
                if (changes.template is_set<c_type::name>()) {
                    query_.set(c_type::name.view(), detail::entity_column_value<entity_type, c_type>(query_, changes));
                }
            }
        });
        return *this;
    }
    template <typename source>
    db_write_query_builder& update_from(std::string_view alias) {
        static_assert(sql_entity<source>, "SQL update sources require a SQL entity");
        query_.update_from(source::table_name(), alias);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& update_from(const db_query_builder<source, source_executor_type>& source_value, std::string_view alias) {
        source_value.require_plain_projection();
        query_.update_from(source_value.query_, alias);
        return *this;
    }
    db_write_query_builder& update_from_cte(std::string_view name, std::string_view alias) {
        query_.update_from(name, alias);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& insert_from(std::span<const std::string_view> columns, const db_query_builder<source, source_executor_type>& source_value) {
        if (!inserting_) {
            throw std::invalid_argument("insert_from requires an insert builder");
        }
        source_value.require_plain_projection();
        source_value.query_.require_select_query();
        if (columns.empty()) {
            throw std::invalid_argument("insert_from requires target columns");
        }
        for (auto column : columns) {
            detail::require_entity_column<entity_type>(column);
            if (detail::is_generated_entity_column<entity_type>(column)) {
                throw std::invalid_argument("insert_from cannot write a generated column");
            }
        }
        const auto count = source_value.selected_.empty() ? std::tuple_size_v<typename source::columns_type> : source_value.selected_.size();
        if (count != columns.size()) {
            throw std::invalid_argument("insert_from projection and target column counts differ");
        }
        query_.insert_into(entity_type::table_name(), columns, alias_).insert_from(source_value.query_);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& insert_from(std::initializer_list<std::string_view> columns, const db_query_builder<source, source_executor_type>& source_value) {
        return insert_from(std::span<const std::string_view>(columns.begin(), columns.size()), source_value);
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& with(std::string_view name, const db_query_builder<source, source_executor_type>& source_value, const db_cte_options& options = {}) {
        source_value.require_plain_projection();
        query_.with(name, source_value.query_, options);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& with(std::string_view name, const db_write_query_builder<source, source_executor_type>& source_value, const db_cte_options& options = {}) {
        source_value.validate();
        query_.with(name, source_value.query_, options);
        return *this;
    }
    db_write_query_builder& returning(std::span<const db_selection> fields = {}) {
        if (fields.empty()) {
            std::pmr::vector<db_expression> expressions(query_.resource());
            detail::for_each_entity_column<entity_type>([&]<typename c_type> { expressions.push_back(query_.column(c_type::name.view(), qualifier())); });
            query_.returning(expressions);
            selected_.clear();
        } else {
            for (const auto& field : fields) {
                if (field.expression_.empty()) {
                    detail::require_entity_column<entity_type>(field.column_);
                }
            }
            selected_ = detail::apply_projection(query_, fields, qualifier(), true);
        }
        return *this;
    }
    db_write_query_builder& returning(std::initializer_list<db_selection> fields_value) {
        return returning(std::span<const db_selection>(fields_value.begin(), fields_value.size()));
    }
    [[nodiscard]] scoped_operation<db_exec_result> execute() const {
        validate();
        return executor_.execute(query_);
    }
    template <typename output_type = entity_type>
    [[nodiscard]] scoped_operation<entity_rows<output_type>> get_many() const {
        validate();
        if (!query_.returns_rows()) {
            throw std::invalid_argument("get_many requires RETURNING");
        }
        if constexpr (!std::same_as<output_type, entity_type>) {
            if (selected_.empty()) {
                throw std::invalid_argument("DTO returning requires an explicit projection");
            }
        }
        return executor_.template query_mapped<entity_rows<output_type>>(query_, detail::db_map_projection<output_type>(selected_));
    }
    [[nodiscard]] db_statement get_query_and_parameters() const {
        validate();
        return query_.compile(executor_.query_driver(), query_.resource());
    }

private:
    friend class db_repository<entity_type, executor_type>;
    template <typename, typename>
    friend class db_query_builder;
    template <typename, typename>
    friend class db_write_query_builder;
    db_write_query_builder(detail::db_repository_input_type<executor_type> executor, db_query query, bool inserting, std::string_view alias = {})
        : executor_(executor),
          query_(std::move(query)),
          inserting_(inserting),
          alias_(alias, query_.resource()),
          selected_(query_.resource()) {}
    std::string_view qualifier() const {
        return alias_.empty() ? entity_type::table_name() : std::string_view(alias_);
    }
    template <detail::db_write_condition condition_type = db_predicate>
    void require_condition(const condition_type& condition) const {
        if (inserting_ || condition.empty()) {
            throw std::invalid_argument("write WHERE requires an update/delete builder and a nonempty condition");
        }
    }
    void validate() const {
        if (!inserting_ && !query_.has_where()) {
            throw std::invalid_argument("repository update/delete requires a condition");
        }
    }
    detail::db_repository_executor_type<executor_type> executor_;
    db_query query_;
    bool inserting_;
    std::pmr::string alias_;
    std::pmr::vector<std::pmr::string> selected_;
};

template <typename entity_type, typename executor_type>
class db_repository final {
    static_assert(sql_entity<entity_type>, "SQL repositories require a RUVIA_DB_ENTITY declaration");

public:
    [[nodiscard]] db_query_builder<entity_type, executor_type> create_query_builder(std::string_view alias = {}) const {
        return db_query_builder<entity_type, executor_type>(executor_, alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> create_update_builder(std::string_view alias = {}) const {
        db_query query(executor_.query_resource());
        query.update(entity_type::table_name(), alias);
        return db_write_query_builder<entity_type, executor_type>(executor_, std::move(query), false, alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> create_delete_builder(std::string_view alias = {}) const {
        db_query query(executor_.query_resource());
        query.delete_from(entity_type::table_name(), alias);
        return db_write_query_builder<entity_type, executor_type>(executor_, std::move(query), false, alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> create_insert_builder() const {
        db_query query(executor_.query_resource());
        query.insert_into(entity_type::table_name());
        return db_write_query_builder<entity_type, executor_type>(executor_, std::move(query), true);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> create_insert_builder(const entity_type& entity) const {
        return create_insert_builder(std::span<const entity_type>(&entity, 1));
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> create_insert_builder(std::span<const entity_type> entities) const {
        return db_write_query_builder<entity_type, executor_type>(executor_, insert_query(entities), true);
    }
    [[nodiscard]] scoped_operation<entity_rows<entity_type>> find(const db_find_options& options = {}) const {
        return find_builder(options).get_many();
    }
    [[nodiscard]] scoped_operation<std::optional<entity_type>> find_one(const db_find_options& options) const {
        return find_builder(options).get_one();
    }
    [[nodiscard]] scoped_operation<std::pair<entity_rows<entity_type>, std::uint64_t>> find_and_count(const db_find_options& options = {}) const {
        return find_builder(options).get_many_and_count();
    }
    [[nodiscard]] scoped_operation<std::uint64_t> count(const db_find_options& options = {}) const {
        return find_builder(options).get_count();
    }
    [[nodiscard]] scoped_operation<bool> exists(const db_find_options& options = {}) const {
        return find_builder(options).get_exists();
    }
    [[nodiscard]] scoped_operation<db_exec_result> insert(const entity_type& entity) const {
        return insert(std::span<const entity_type>(&entity, 1));
    }
    [[nodiscard]] scoped_operation<db_exec_result> insert(std::span<const entity_type> entities) const {
        auto query = insert_query(entities);
        return executor_.execute(query);
    }
    template <detail::db_write_condition condition_type = db_predicate>
    [[nodiscard]] scoped_operation<db_exec_result> update(const condition_type& predicate, const entity_type& changes) const {
        auto query = update_query(predicate, changes);
        return executor_.execute(query);
    }
    [[nodiscard]] scoped_operation<db_exec_result> delete_by(const db_predicate& predicate) const {
        if (predicate.empty()) {
            throw std::invalid_argument("repository delete_by requires a condition");
        }
        db_query query(executor_.query_resource());
        query.delete_from(entity_type::table_name()).where(predicate.expression(query));
        return executor_.execute(query);
    }
    template <typename number_type>
        requires((std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] scoped_operation<db_exec_result> increment(const db_predicate& predicate, std::string_view property_path, number_type value) const {
        return adjust_number(predicate, property_path, value, db_binary_operator::add);
    }
    template <typename number_type>
        requires((std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] scoped_operation<db_exec_result> decrement(const db_predicate& predicate, std::string_view property_path, number_type value) const {
        return adjust_number(predicate, property_path, value, db_binary_operator::subtract);
    }
    [[nodiscard]] scoped_operation<db_exec_result> remove(const entity_type& entity) const {
        db_query query(executor_.query_resource());
        query.delete_from(entity_type::table_name());
        detail::for_each_entity_column<entity_type>([&]<typename c_type> {
            if constexpr (c_type::options.primary_key_) {
                if (!entity.template is_set<c_type::name>() || entity.template is_null<c_type::name>()) {
                    throw std::invalid_argument("remove requires all primary key values");
                }
                query.and_where(query.binary(query.column(c_type::name.view()), db_binary_operator::equal, detail::entity_column_value<entity_type, c_type>(query, entity)));
            }
        });
        if (!query.has_where()) {
            throw std::invalid_argument("remove requires an entity primary key");
        }
        return executor_.execute(query);
    }
    [[nodiscard]] scoped_operation<db_exec_result> upsert(const entity_type& entity, const db_upsert_options& options) const {
        return upsert(std::span<const entity_type>(&entity, 1), options);
    }
    [[nodiscard]] scoped_operation<db_exec_result> upsert(std::span<const entity_type> entities, const db_upsert_options& options) const {
        auto query = upsert_query(entities, options);
        return executor_.execute(query);
    }

    template <detail::db_write_condition condition_type = db_predicate>
    [[nodiscard]] scoped_operation<db_exec_result> update(const condition_type& predicate, std::span<const db_assignment> changes) const {
        auto query = update_query(predicate, changes);
        return executor_.execute(query);
    }
    template <detail::db_write_condition condition_type = db_predicate>
    [[nodiscard]] scoped_operation<db_exec_result> update(const condition_type& predicate, std::initializer_list<db_assignment> changes) const {
        return update(predicate, std::span<const db_assignment>(changes.begin(), changes.size()));
    }
    template <typename output_type = entity_type>
    [[nodiscard]] scoped_operation<entity_rows<output_type>> insert_returning(const entity_type& entity, std::span<const db_selection> fields = {}) const {
        return insert_returning<output_type>(std::span<const entity_type>(&entity, 1), fields);
    }
    template <typename output_type = entity_type>
    [[nodiscard]] scoped_operation<entity_rows<output_type>> insert_returning(std::span<const entity_type> entities, std::span<const db_selection> fields = {}) const {
        auto query = insert_query(entities);
        return returning<output_type>(query, fields);
    }
    template <typename output_type = entity_type, detail::db_write_condition condition_type = db_predicate>
    [[nodiscard]] scoped_operation<entity_rows<output_type>> update_returning(const condition_type& predicate, const entity_type& changes, std::span<const db_selection> fields = {}) const {
        auto query = update_query(predicate, changes);
        return returning<output_type>(query, fields);
    }
    template <typename output_type = entity_type, detail::db_write_condition condition_type = db_predicate>
    [[nodiscard]] scoped_operation<entity_rows<output_type>> update_returning(const condition_type& predicate, std::span<const db_assignment> changes, std::span<const db_selection> fields = {}) const {
        auto query = update_query(predicate, changes);
        return returning<output_type>(query, fields);
    }
    template <typename output_type = entity_type>
    [[nodiscard]] scoped_operation<entity_rows<output_type>> delete_returning(const db_predicate& predicate, std::span<const db_selection> fields = {}) const {
        if (predicate.empty()) {
            throw std::invalid_argument("repository delete_returning requires a condition");
        }
        db_query query(executor_.query_resource());
        query.delete_from(entity_type::table_name()).where(predicate.expression(query));
        return returning<output_type>(query, fields);
    }
    template <typename output_type = entity_type>
    [[nodiscard]] scoped_operation<entity_rows<output_type>> upsert_returning(const entity_type& entity, const db_upsert_options& options, std::span<const db_selection> fields = {}) const {
        return upsert_returning<output_type>(std::span<const entity_type>(&entity, 1), options, fields);
    }
    template <typename output_type = entity_type>
    [[nodiscard]] scoped_operation<entity_rows<output_type>> upsert_returning(std::span<const entity_type> entities, const db_upsert_options& options, std::span<const db_selection> fields = {}) const {
        auto query = upsert_query(entities, options);
        return returning<output_type>(query, fields);
    }

private:
    friend class db_handle;
    friend class db_transaction;
    explicit db_repository(detail::db_repository_input_type<executor_type> executor)
        : executor_(executor) {}
    template <typename number_type>
    [[nodiscard]] scoped_operation<db_exec_result> adjust_number(const db_predicate& predicate, std::string_view property_path, number_type value, db_binary_operator op) const {
        if (predicate.empty()) {
            throw std::invalid_argument("repository increment/decrement requires a condition");
        }
        bool numeric = false;
        detail::for_each_entity_column<entity_type>([&]<typename c_type> {
            if (c_type::name.view() == property_path && c_type::options.generated_type_ == db_generated_type::none && !detail::is_pmr_vector<typename c_type::value_type>::value) {
                numeric = c_type::data_type == db_data_type::small_int || c_type::data_type == db_data_type::integer || c_type::data_type == db_data_type::big_int || c_type::data_type == db_data_type::numeric || c_type::data_type == db_data_type::real || c_type::data_type == db_data_type::double_value;
            }
        });
        if (!numeric) {
            throw std::invalid_argument("repository increment/decrement requires a writable numeric column");
        }
        db_query query(executor_.query_resource());
        query.update(entity_type::table_name()).where(detail::write_condition(query, predicate));
        query.set(property_path, query.binary(query.column(property_path), op, query.value(value)));
        return executor_.execute(query);
    }
    [[nodiscard]] db_query_builder<entity_type, executor_type> find_builder(const db_find_options& options) const {
        auto builder = create_query_builder();
        detail::apply_find_options<entity_type>(builder.query_, options, builder.alias_);
        if (!options.relations_.empty()) {
            builder.relations_.emplace(builder.query_.resource());
            for (const auto& path : options.relations_) {
                builder.relations_->template add<entity_type>(builder.query_, builder.alias_, path);
            }
        }
        return builder;
    }

    template <typename output_type>
    scoped_operation<entity_rows<output_type>> returning(db_query& query, std::span<const db_selection> fields_value) const {
        std::pmr::vector<std::pmr::string> columns(query.resource());
        if (fields_value.empty()) {
            static_assert(requires { typename output_type::columns_type; });
            constexpr bool entity_columns_only = []<typename... columns_type>(std::tuple<columns_type...>*) {
                return (detail::has_entity_column<entity_type>(columns_type::name.view()) && ...);
            }(static_cast<typename output_type::columns_type*>(nullptr));
            if constexpr (entity_columns_only) {
                std::pmr::vector<db_expression> values(query.resource());
                detail::for_each_entity_column<output_type>([&]<typename c_type> {
                    values.push_back(query.column(c_type::name.view()));
                });
                query.returning(values);
            } else {
                throw std::invalid_argument("unknown database entity column");
            }
        } else {
            for (const auto& field : fields_value) {
                if (field.expression_.empty()) {
                    detail::require_entity_column<entity_type>(field.column_);
                }
            }
            columns = detail::apply_projection(query, fields_value, {}, true);
        }
        return executor_.template query_mapped<entity_rows<output_type>>(query, detail::db_map_projection<output_type>(columns));
    }
    template <detail::db_write_condition condition_type = db_predicate>
    db_query update_query(const condition_type& predicate, const entity_type& changes) const {
        if (predicate.empty()) {
            throw std::invalid_argument("repository update requires a condition");
        }
        db_query query(executor_.query_resource());
        query.update(entity_type::table_name()).where(detail::write_condition(query, predicate));
        bool selected = false;
        detail::for_each_entity_column<entity_type>([&]<typename c_type> {
            if constexpr (c_type::options.generated_type_ == db_generated_type::none) {
                if (changes.template is_set<c_type::name>()) {
                    query.set(c_type::name.view(), detail::entity_column_value<entity_type, c_type>(query, changes));
                    selected = true;
                }
            }
        });
        if (!selected) {
            throw std::invalid_argument("repository update requires a writable column");
        }
        return query;
    }
    template <detail::db_write_condition condition_type = db_predicate>
    db_query update_query(const condition_type& predicate, std::span<const db_assignment> changes) const {
        if (predicate.empty() || changes.empty()) {
            throw std::invalid_argument("repository update requires a condition and writable columns");
        }
        db_query query(executor_.query_resource());
        query.update(entity_type::table_name()).where(detail::write_condition(query, predicate));
        for (std::size_t i = 0; i < changes.size(); ++i) {
            const auto& item = changes[i];
            detail::require_entity_column<entity_type>(item.column_);
            if (detail::is_generated_entity_column<entity_type>(item.column_) || item.value_.empty()) {
                throw std::invalid_argument("update expression requires a writable column");
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (changes[j].column_ == item.column_) {
                    throw std::invalid_argument("duplicate update column");
                }
            }
            query.set(item.column_, query.import_expression(item.value_));
        }
        return query;
    }
    db_query upsert_query(std::span<const entity_type> entities, const db_upsert_options& options) const {
        auto query = insert_query(entities);
        if ((options.skip_update_if_no_values_changed_ || !options.index_predicate_.empty() || !options.update_where_.empty()) && executor_.query_driver() != db_driver::postgresql) {
            throw std::invalid_argument("conditional repository upsert requires PostgreSQL");
        }
        db_conflict_options conflict{.columns_ = options.conflict_paths_, .do_nothing_ = options.do_nothing_, .any_unique_key_ = options.any_unique_key_};
        conflict.target_where_ = options.index_predicate_.expression(query);
        for (const auto& name : options.conflict_paths_) {
            detail::require_entity_column<entity_type>(name);
        }
        for (const auto& name : options.update_columns_) {
            detail::require_entity_column<entity_type>(name);
            if (detail::is_generated_entity_column<entity_type>(name)) {
                throw std::invalid_argument("repository upsert cannot update a generated column");
            }
        }
        for (std::size_t i = 0; i < options.update_expressions_.size(); ++i) {
            const auto& item = options.update_expressions_[i];
            detail::require_entity_column<entity_type>(item.column_);
            if (item.value_.empty() || detail::is_generated_entity_column<entity_type>(item.column_) || options.do_nothing_) {
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
        if (!options.do_nothing_) {
            detail::for_each_entity_column<entity_type>([&]<typename c_type> {
                bool selected = false;
                const auto custom_value = std::ranges::find_if(options.update_expressions_, [](const auto& item) { return item.column_ == c_type::name.view(); });
                if (custom_value != options.update_expressions_.end()) {
                    selected = true;
                } else if (options.update_columns_.empty()) {
                    if constexpr (!c_type::options.primary_key_ && !c_type::options.generated_ && c_type::options.generated_type_ == db_generated_type::none) {
                        selected = std::ranges::all_of(entities, [](const entity_type& entity) { return entity.template is_set<c_type::name>(); });
                        if (!selected && std::ranges::any_of(entities, [](const entity_type& entity) { return entity.template is_set<c_type::name>(); })) {
                            throw std::invalid_argument("bulk upsert requires identical set columns or explicit update_columns");
                        }
                        selected &= std::ranges::none_of(options.conflict_paths_, [](const auto& name) { return name == c_type::name.view(); });
                    }
                } else {
                    if constexpr (c_type::options.generated_type_ == db_generated_type::none) {
                        selected = std::ranges::any_of(options.update_columns_, [](const auto& name) { return name == c_type::name.view(); });
                    }
                }
                if (selected || custom_value != options.update_expressions_.end()) {
                    auto value = custom_value == options.update_expressions_.end() ? query.excluded(c_type::name.view()) : query.import_expression(custom_value->value_);
                    conflict.update_.push_back({std::string(c_type::name.view()), value});
                    if (options.skip_update_if_no_values_changed_) {
                        auto changed = query.binary(query.column(c_type::name.view(), entity_type::table_name()), db_binary_operator::is_distinct_from, value);
                        conflict.update_where_ = conflict.update_where_.empty() ? changed : query.binary(conflict.update_where_, db_binary_operator::or_value, changed);
                    }
                }
            });
            if (conflict.update_.empty() && executor_.query_driver() == db_driver::postgresql) {
                conflict.do_nothing_ = true;
            }
        }
        if (!options.update_where_.empty()) {
            auto condition = query.import_expression(options.update_where_);
            conflict.update_where_ = conflict.update_where_.empty() ? condition : query.binary(conflict.update_where_, db_binary_operator::and_value, condition);
        }
        query.on_conflict(conflict);
        return query;
    }
    db_query insert_query(std::span<const entity_type> entities) const {
        if (entities.empty()) {
            throw std::invalid_argument("repository insert requires an entity");
        }
        db_query query(executor_.query_resource());
        std::pmr::vector<std::string_view> columns(query.resource());
        detail::for_each_entity_column<entity_type>([&]<typename c_type> {
            if constexpr (c_type::options.generated_type_ == db_generated_type::none) {
                if (std::ranges::any_of(entities, [](const entity_type& entity) { return entity.template is_set<c_type::name>(); })) {
                    columns.push_back(c_type::name.view());
                }
            }
        });
        query.insert_into(entity_type::table_name(), columns);
        if (columns.empty()) {
            if (entities.size() != 1) {
                throw std::invalid_argument("bulk default-only inserts require at least one explicit column");
            }
            return query;
        }
        for (const auto& entity : entities) {
            std::pmr::vector<db_expression> row(query.resource());
            detail::for_each_entity_column<entity_type>([&]<typename c_type> {
                if constexpr (c_type::options.generated_type_ == db_generated_type::none) {
                    if (std::ranges::find(columns, c_type::name.view()) != columns.end()) {
                        row.push_back(detail::entity_column_value<entity_type, c_type>(query, entity));
                    }
                }
            });
            query.values(row);
        }
        return query;
    }
    detail::db_repository_executor_type<executor_type> executor_;
};

template <typename entity_type>
db_repository<entity_type, db_handle> db_handle::get_repository() const {
    registration_.require_active();
    return db_repository<entity_type, db_handle>(*this);
}
template <typename entity_type>
db_repository<entity_type, db_transaction> db_transaction::get_repository() & {
    (void)query_resource();
    return db_repository<entity_type, db_transaction>(*this);
}

}  // namespace ruvia
