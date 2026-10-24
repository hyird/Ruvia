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

#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/detail/db/db_entity_codec.h"
#include "ruvia/web/detail/db/db_relation_query.h"
#include "ruvia/web/detail/db/db_result_access.h"

namespace ruvia {

template <typename entity_type, typename executor_type>
class db_write_query_builder;

struct db_find_order final {
    borrowed_text column_{};
    db_order_direction direction_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
};

// span 及元素中的文本均为借用；repository 调用入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_find_options final {
    std::span<const borrowed_text> select_{};
    db_predicate where_{};
    std::span<const borrowed_text> relations_{};
    std::span<const db_find_order> order_{};
    std::optional<std::uint64_t> skip_{};
    std::optional<std::uint64_t> take_{};
    std::optional<db_lock_options> lock_{};
    db_cache_setting_type cache_{};
    bool with_deleted_{false};
};

// span 及元素中的文本均为借用；upsert 调用入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_upsert_options final {
    std::span<const borrowed_text> conflict_paths_{};
    std::span<const borrowed_text> update_columns_{};
    bool do_nothing_{false};
    bool any_unique_key_{false};
    bool skip_update_if_no_values_changed_{false};
    db_predicate index_predicate_{};
    std::span<const db_assignment> update_expressions_{};
    db_predicate update_where_{};
};

class db_values_builder final {
public:
    explicit db_values_builder(std::span<const std::string_view> columns, std::pmr::memory_resource* resource = nullptr)
        : query_(resource),
          columns_(query_.resource()) {
        if (columns.empty()) {
            throw std::invalid_argument("db_values_builder requires at least one column");
        }
        columns_.reserve(columns.size());
        for (const auto column : columns) {
            columns_.emplace_back(column);
        }
    }
    explicit db_values_builder(std::initializer_list<std::string_view> columns, std::pmr::memory_resource* resource = nullptr)
        : db_values_builder(std::span<const std::string_view>(columns.begin(), columns.size()), resource) {}

    db_values_builder& row(std::span<const db_expr> values) {
        if (values.size() != columns_.size()) {
            throw std::invalid_argument("db_values_builder row width differs from column count");
        }
        std::pmr::vector<db_expression> imported(query_.resource());
        imported.reserve(values.size());
        for (const auto& value : values) {
            imported.push_back(value.expression(query_));
        }
        query_.values(imported);
        return *this;
    }
    db_values_builder& row(std::initializer_list<db_expr> values) {
        return row(std::span<const db_expr>(values.begin(), values.size()));
    }

    [[nodiscard]] const db_query& query() const& noexcept RUVIA_LIFETIMEBOUND {
        return query_;
    }
    const db_query& query() const&& = delete;
    [[nodiscard]] const std::pmr::vector<std::pmr::string>& columns() const noexcept {
        return columns_;
    }

private:
    db_query query_;
    std::pmr::vector<std::pmr::string> columns_;
};

namespace detail {

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
template <typename c_type>
consteval bool is_writable_entity_column_descriptor() noexcept {
    return c_type::options.generated_type_ == db_generated_type::none && !c_type::options.read_only_;
}
template <typename e_type>
bool is_generated_entity_column(std::string_view name) {
    bool generated = false;
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t> {
        if constexpr (!is_writable_entity_column_descriptor<c_type>()) {
            generated |= name == c_type::name.view();
        }
    });
    return generated;
}
template <typename e_type>
bool is_nullable_entity_column(std::string_view name) {
    bool nullable = false;
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t> {
        if constexpr (is_writable_entity_column_descriptor<c_type>() && c_type::options.nullable_) {
            nullable |= name == c_type::name.view();
        }
    });
    return nullable;
}
template <typename e_type>
bool is_numeric_entity_column(std::string_view name) {
    bool numeric = false;
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t> {
        if (c_type::name.view() == name && is_writable_entity_column_descriptor<c_type>() && !is_pmr_vector<typename c_type::value_type>::value) {
            numeric = c_type::data_type == db_data_type::small_int ||
                      c_type::data_type == db_data_type::integer ||
                      c_type::data_type == db_data_type::big_int ||
                      c_type::data_type == db_data_type::numeric ||
                      c_type::data_type == db_data_type::real ||
                      c_type::data_type == db_data_type::double_value;
        }
    });
    return numeric;
}
template <typename e_type>
consteval std::string_view delete_date_column() noexcept {
    std::string_view result{};
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t> {
        if constexpr (is_writable_entity_column_descriptor<c_type>() && c_type::options.delete_date_) {
            result = c_type::name.view();
        }
    });
    return result;
}
template <typename e_type>
consteval std::string_view update_date_column() noexcept {
    std::string_view result{};
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t> {
        if constexpr (is_writable_entity_column_descriptor<c_type>() && c_type::options.update_date_) {
            result = c_type::name.view();
        }
    });
    return result;
}
template <typename e_type>
consteval std::string_view version_column() noexcept {
    std::string_view result{};
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t> {
        if constexpr (is_writable_entity_column_descriptor<c_type>() && c_type::options.version_) {
            result = c_type::name.view();
        }
    });
    return result;
}
template <typename e_type>
void select_entity(db_query& query, std::string_view alias) {
    const auto qualifier = alias.empty() ? e_type::table_name() : alias;
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t> { query.add_select(query.column(c_type::name.view(), qualifier)); });
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
struct db_map_count final {
    std::uint64_t operator()(db_rows&& rows, std::pmr::memory_resource*) const {
        return db_count_value(rows);
    }
};
struct db_map_exists final {
    bool operator()(db_rows&& rows, std::pmr::memory_resource*) const {
        if (rows.size() != 1) {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_row_count, "database exists did not return exactly one row");
        }
        const auto value = rows.front()["exists"].template as<bool>();
        if (!value) {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_null, "database exists column is NULL");
        }
        return *value;
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
    explicit db_map_projection(std::span<const std::pmr::string> selected, bool allow_extra_columns = false, bool wildcard = false)
        : wildcard_(wildcard) {
        if (!allow_extra_columns) {
            for (const auto& name : selected) {
                require_entity_column<e_type>(name);
            }
        }
        for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t index> {
            selected_columns_[index] = (!wildcard && selected.empty()) || std::ranges::find(selected, c_type::name.view()) != selected.end();
        });
    }
    entity_rows<e_type> operator()(const db_rows& rows, std::pmr::memory_resource* resource) const {
        auto result_value = db_result_access::make_entity_rows<e_type>(resource, rows.size());
        db_entity_row_decoder<e_type> decoder(selected_columns_, wildcard_);
        for (const auto& row : rows) {
            result_value.push_back(decoder.decode(row, resource));
        }
        return result_value;
    }

    std::array<bool, std::tuple_size_v<typename e_type::columns_type>> selected_columns_{};
    bool wildcard_{false};
};
template <typename e_type>
struct db_map_one_projection final {
    db_map_projection<e_type> mapper_;
    std::optional<e_type> operator()(db_rows&& rows, std::pmr::memory_resource* resource) const {
        if (rows.empty()) {
            return std::nullopt;
        }
        db_entity_row_decoder<e_type> decoder(mapper_.selected_columns_, mapper_.wildcard_);
        return decoder.decode(rows.front(), resource);
    }
};
struct db_map_raw_rows final {
    db_rows operator()(db_rows&& rows, std::pmr::memory_resource*) const noexcept {
        return std::move(rows);
    }
};
template <typename scalar_type>
struct db_map_scalar final {
    std::optional<scalar_type> operator()(db_rows&& rows, std::pmr::memory_resource* resource) const {
        if (rows.empty() || !rows.front()[std::size_t{0}].value()) {
            return std::nullopt;
        }
        auto out = entity_value_slot<scalar_type>::make_value(resource);
        decode_db_field(rows.front()[std::size_t{0}], out, resource);
        return out;
    }
};
template <typename scalar_type>
struct db_map_scalars final {
    std::pmr::vector<scalar_type> operator()(db_rows&& rows, std::pmr::memory_resource* resource) const {
        std::pmr::vector<scalar_type> result(pmr_resource_or_default(resource));
        result.reserve(rows.size());
        for (const auto& row : rows) {
            if (!row[std::size_t{0}].value()) {
                throw db_conversion_error(
                    db_conversion_error::code_type::unexpected_null, "database scalar column is NULL");
            }
            auto out = entity_value_slot<scalar_type>::make_value(resource);
            decode_db_field(row[std::size_t{0}], out, resource);
            result.push_back(std::move(out));
        }
        return result;
    }
};
template <typename e_type>
struct db_map_raw_and_entities final {
    db_map_projection<e_type> mapper_;
    std::pair<entity_rows<e_type>, db_rows> operator()(db_rows&& rows, std::pmr::memory_resource* resource) const {
        auto entities = mapper_(rows, resource);
        return {std::move(entities), std::move(rows)};
    }
};
template <typename e_type>
struct db_map_required_one final {
    db_map_projection<e_type> mapper_;
    e_type operator()(db_rows&& rows, std::pmr::memory_resource* resource) const {
        if (rows.size() != 1) {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_row_count, "database save did not return the saved row");
        }
        db_entity_row_decoder<e_type> decoder(mapper_.selected_columns_, mapper_.wildcard_);
        return decoder.decode(rows.front(), resource);
    }
};
struct db_projection final {
    explicit db_projection(std::pmr::memory_resource* resource)
        : columns_(resource) {}
    [[nodiscard]] bool empty() const noexcept {
        return columns_.empty() && !wildcard_;
    }
    std::pmr::vector<std::pmr::string> columns_;
    bool wildcard_{false};
};
// Wildcards stay in the AST; only explicit result names are tracked for mapping.
db_projection apply_projection(db_query& query, std::span<const db_selection> selection,
    std::string_view qualifier, bool returning = false, std::string_view root_table = {},
    const db_projection* previous = nullptr);
[[nodiscard]] std::pmr::string format_truncate_table(std::string_view table, db_driver driver, std::pmr::memory_resource* resource);

template <typename entity_type, typename c_type>
void append_upsert_column_from_list(db_query& query,
    std::pmr::vector<db_assignment>& updates,
    db_expression& update_where, std::span<const std::string_view> inserted_columns,
    const db_upsert_options& options, std::string_view qualifier) {
    const auto name = c_type::name.view();
    const auto custom_value = std::ranges::find_if(
        options.update_expressions_, [name](const auto& item) { return item.column_ == name; });
    bool selected = custom_value != options.update_expressions_.end();
    if (!selected) {
        if (options.update_columns_.empty()) {
            if constexpr (!c_type::options.primary_key_ && !c_type::options.generated_ &&
                          is_writable_entity_column_descriptor<c_type>()) {
                selected = std::ranges::find(inserted_columns, name) != inserted_columns.end() &&
                           std::ranges::find(options.conflict_paths_, borrowed_text(name)) == options.conflict_paths_.end();
            }
        } else if constexpr (is_writable_entity_column_descriptor<c_type>()) {
            selected = std::ranges::find(options.update_columns_, borrowed_text(name)) != options.update_columns_.end();
        }
    }
    if (!selected) {
        return;
    }
    auto value = custom_value == options.update_expressions_.end()
                     ? query.excluded(name)
                     : custom_value->expression(query, entity_type::table_name(), qualifier == entity_type::table_name() ? std::string_view{} : qualifier);
    updates.push_back(db_assignment{name, db_expr(value, {}, true)});
    if (options.skip_update_if_no_values_changed_) {
        auto changed = query.binary(
            query.column(name, qualifier), db_binary_operator::is_distinct_from, value);
        update_where = update_where.empty()
                           ? changed
                           : query.binary(update_where, db_binary_operator::or_value, changed);
    }
}

template <typename entity_type>
void apply_entity_upsert(db_query& query, std::span<const std::string_view> inserted_columns,
    const db_upsert_options& options, db_driver driver, std::string_view qualifier) {
    if ((options.skip_update_if_no_values_changed_ || !options.index_predicate_.empty() ||
            !options.update_where_.empty()) &&
        driver != db_driver::postgresql) {
        throw std::invalid_argument("conditional repository upsert requires PostgreSQL");
    }
    const auto alias = qualifier == entity_type::table_name() ? std::string_view{} : qualifier;
    const auto target_where = options.index_predicate_.expression(query, entity_type::table_name(), alias);
    for (const auto& name : options.conflict_paths_) {
        require_entity_column<entity_type>(name);
    }
    for (const auto& name : options.update_columns_) {
        require_entity_column<entity_type>(name);
        if (is_generated_entity_column<entity_type>(name)) {
            throw std::invalid_argument("repository upsert cannot update a generated column");
        }
    }
    for (std::size_t i = 0; i < options.update_expressions_.size(); ++i) {
        const auto& item = options.update_expressions_[i];
        require_entity_column<entity_type>(item.column_);
        if (item.empty() || is_generated_entity_column<entity_type>(item.column_) || options.do_nothing_) {
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
    std::pmr::vector<db_assignment> updates(query.resource());
    db_expression update_where;
    bool do_nothing = options.do_nothing_;
    if (!do_nothing) {
        for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
            append_upsert_column_from_list<entity_type, c_type>(
                query, updates, update_where, inserted_columns, options, qualifier);
        });
        if (updates.empty() && driver == db_driver::postgresql) {
            do_nothing = true;
        }
    }
    if (!options.update_where_.empty()) {
        auto condition = options.update_where_.expression(query, entity_type::table_name(), alias);
        update_where = update_where.empty()
                           ? condition
                           : query.binary(update_where, db_binary_operator::and_value, condition);
    }
    query.on_conflict({
        .columns_ = options.conflict_paths_,
        .target_where_ = target_where,
        .update_ = updates,
        .update_where_ = update_where,
        .do_nothing_ = do_nothing,
        .any_unique_key_ = options.any_unique_key_,
    });
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
            if (!field.has_expression() && !field.is_wildcard()) {
                detail::require_entity_column<entity_type>(field.column_);
            }
        }
        selected_ = detail::apply_projection(query_, fields_value, alias_, false, entity_type::table_name());
        return *this;
    }
    db_query_builder& select(std::initializer_list<db_selection> fields_value) {
        return select(std::span<const db_selection>(fields_value.begin(), fields_value.size()));
    }
    db_query_builder& select(std::span<const std::string_view> columns) {
        std::pmr::vector<db_selection> selections(query_.resource());
        selections.reserve(columns.size());
        for (const auto column : columns) {
            selections.emplace_back(column);
        }
        return select(selections);
    }
    db_query_builder& select(const db_expr& expression, borrowed_text alias_name) {
        return select({expression.as(alias_name)});
    }
    db_query_builder& add_select(std::span<const db_selection> fields_value) {
        require_plain_projection();
        if (fields_value.empty()) {
            throw std::invalid_argument("add_select requires at least one field");
        }
        for (const auto& field : fields_value) {
            if (!field.has_expression() && !field.is_wildcard()) {
                detail::require_entity_column<entity_type>(field.column_);
            }
        }
        detail::db_projection implicit(query_.resource());
        const auto* previous = &selected_;
        if (selected_.empty()) {
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
                implicit.columns_.emplace_back(c_type::name.view());
            });
            previous = &implicit;
        }
        selected_ = detail::apply_projection(query_, fields_value, alias_, false, entity_type::table_name(), previous);
        return *this;
    }
    db_query_builder& add_select(std::initializer_list<db_selection> fields_value) {
        return add_select(std::span<const db_selection>(fields_value.begin(), fields_value.size()));
    }
    db_query_builder& add_select(std::string_view column) {
        return add_select({db_selection{column}});
    }
    db_query_builder& add_select(const db_expr& expression, borrowed_text alias_name) {
        return add_select({expression.as(alias_name)});
    }
    db_query_builder& from_nothing() {
        require_plain_projection();
        query_.clear_source();
        return *this;
    }
    template <typename joined_type>
    db_query_builder& join(db_join_type type, std::string_view alias, const db_predicate& on = {}) {
        static_assert(sql_entity<joined_type>, "SQL joins require a SQL entity");
        query_.join(type, joined_type::table_name(), import_join_predicate<joined_type>(on, alias), alias);
        return *this;
    }
    template <typename joined_type>
    db_query_builder& join(db_join_type type, std::string_view alias, std::span<const std::string_view> using_columns) {
        static_assert(sql_entity<joined_type>, "SQL joins require a SQL entity");
        query_.join(type, joined_type::table_name(), {}, alias, using_columns);
        return *this;
    }
    template <typename joined_type>
    db_query_builder& join(db_join_type type, std::string_view alias, std::initializer_list<std::string_view> using_columns) {
        return join<joined_type>(type, alias, std::span<const std::string_view>(using_columns.begin(), using_columns.size()));
    }
    template <typename source, typename source_executor_type>
    db_query_builder& join(db_join_type type, const db_query_builder<source, source_executor_type>& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        source_value.require_plain_projection();
        query_.join(type, source_value.query_, import_join_predicate<source>(on, alias), alias, options);
        return *this;
    }
    db_query_builder& join(db_join_type type, const db_query& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        query_.join(type, source_value, on.expression(query_, entity_type::table_name(), alias_), alias, options);
        return *this;
    }
    db_query_builder& join_cte(db_join_type type, std::string_view name, std::string_view alias, const db_predicate& on = {}) {
        query_.join(type, name, on.expression(query_, entity_type::table_name(), alias_), alias);
        return *this;
    }
    db_query_builder& join_cte(db_join_type type, std::string_view name, std::string_view alias, std::span<const std::string_view> using_columns) {
        query_.join(type, name, {}, alias, using_columns);
        return *this;
    }
    db_query_builder& join_cte(db_join_type type, std::string_view name, std::string_view alias, std::initializer_list<std::string_view> using_columns) {
        return join_cte(type, name, alias, std::span<const std::string_view>(using_columns.begin(), using_columns.size()));
    }
    db_query_builder& join_function(db_join_type type, db_expression function, std::string_view alias,
        const db_predicate& on = {}, const db_source_options& options = {}) {
        query_.join_function(type, query_.import_expression(function), on.expression(query_, entity_type::table_name(), alias_), alias, options);
        return *this;
    }
    db_query_builder& left_join(std::string_view relation, std::string_view alias, const db_predicate& condition = {}) {
        detail::db_relation_plan::template join_relation<entity_type>(
            query_, alias_, relation, alias, db_join_type::left, condition.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& inner_join(std::string_view relation, std::string_view alias, const db_predicate& condition = {}) {
        detail::db_relation_plan::template join_relation<entity_type>(
            query_, alias_, relation, alias, db_join_type::inner, condition.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    template <typename joined_type>
    db_query_builder& left_join(std::string_view alias, const db_predicate& on = {}) {
        return join<joined_type>(db_join_type::left, alias, on);
    }
    template <typename joined_type>
    db_query_builder& inner_join(std::string_view alias, const db_predicate& on = {}) {
        return join<joined_type>(db_join_type::inner, alias, on);
    }
    template <typename source, typename source_executor_type>
    db_query_builder& left_join(const db_query_builder<source, source_executor_type>& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        return join(db_join_type::left, source_value, alias, on, options);
    }
    template <typename source, typename source_executor_type>
    db_query_builder& inner_join(const db_query_builder<source, source_executor_type>& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        return join(db_join_type::inner, source_value, alias, on, options);
    }
    db_query_builder& left_join(const db_query& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        return join(db_join_type::left, source_value, alias, on, options);
    }
    db_query_builder& inner_join(const db_query& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        return join(db_join_type::inner, source_value, alias, on, options);
    }
    template <typename source, typename source_executor_type>
    db_query_builder& with(std::string_view name, const db_query_builder<source, source_executor_type>& source_value, const db_cte_options& options = {}) {
        query_.with(name, source_value.query(), options);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_query_builder& with(std::string_view name, const db_write_query_builder<source, source_executor_type>& source_value, const db_cte_options& options = {}) {
        require_plain_projection();
        query_.with(name, source_value.query(), options);
        return *this;
    }
    db_query_builder& with(std::string_view name, const db_query& source_value, const db_cte_options& options = {}) {
        if (source_value.has_writes()) {
            require_plain_projection();
        }
        query_.with(name, source_value, options);
        return *this;
    }
    db_query_builder& with(std::string_view name, const db_values_builder& values,
        db_materialization materialization = db_materialization::default_value) {
        query_.with_columns(name, values.query(), materialization, values.columns());
        return *this;
    }
    db_query_builder& from_cte(std::string_view name) {
        require_plain_projection();
        query_.from(name, alias_);
        return *this;
    }
    db_query_builder& from_function(db_expression function, std::string_view alias = {}, const db_source_options& options = {}) {
        require_plain_projection();
        query_.from_function(query_.import_expression(function), alias.empty() ? std::string_view(alias_) : alias, options);
        return *this;
    }
    // Borrows the flat AST, without applying execution-time soft-delete filtering.
    // The builder and its resource must outlive the view; consumers clone on import.
    [[nodiscard]] const db_query& query() const& RUVIA_LIFETIMEBOUND {
        require_plain_projection();
        return query_;
    }
    const db_query& query() const&& = delete;
    db_expression subquery(db_query& expressions) const {
        require_plain_projection();
        return expressions.subquery(query_);
    }
    db_expression exists(db_query& expressions) const {
        require_plain_projection();
        return expressions.exists(query_);
    }
    [[nodiscard]] db_predicate exists() const {
        require_plain_projection();
        return db_predicate::exists(query_);
    }
    [[nodiscard]] db_predicate not_exists() const {
        require_plain_projection();
        return db_predicate::not_exists(query_);
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
    db_query_builder& group_by(std::span<const std::string_view> columns) {
        std::pmr::vector<db_expression> expressions(query_.resource());
        expressions.reserve(columns.size());
        for (const auto column : columns) {
            detail::require_entity_column<entity_type>(column);
            expressions.push_back(query_.column(column, alias_));
        }
        query_.group_by(expressions);
        return *this;
    }
    db_query_builder& group_by(std::initializer_list<std::string_view> columns) {
        return group_by(std::span<const std::string_view>(columns.begin(), columns.size()));
    }
    db_query_builder& add_group_by(db_expression expression) {
        query_.add_group_by(query_.import_expression(expression));
        return *this;
    }
    db_query_builder& add_group_by(std::string_view column) {
        detail::require_entity_column<entity_type>(column);
        query_.add_group_by(query_.column(column, alias_));
        return *this;
    }
    template <typename target_entity, fixed_string field_name>
    db_query_builder& add_group_by(db_field_reference<target_entity, field_name> column) {
        const auto default_alias = std::same_as<target_entity, entity_type> ? std::string_view(alias_) : std::string_view{};
        query_.add_group_by(column.expression(query_, default_alias));
        return *this;
    }
    db_query_builder& having(const db_predicate& predicate) {
        query_.having(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& and_having(const db_predicate& predicate) {
        query_.and_having(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& or_having(const db_predicate& predicate) {
        query_.or_having(predicate.expression(query_, entity_type::table_name(), alias_));
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
    db_query_builder& distinct_on(std::span<const db_expression> expressions) {
        std::pmr::vector<db_expression> imported(query_.resource());
        imported.reserve(expressions.size());
        for (const auto expression : expressions) {
            imported.push_back(query_.import_expression(expression));
        }
        query_.distinct_on(imported);
        return *this;
    }
    db_query_builder& distinct_on(std::initializer_list<db_expression> expressions) {
        return distinct_on(std::span<const db_expression>(expressions.begin(), expressions.size()));
    }
    db_query_builder& distinct_on(std::span<const std::string_view> columns) {
        std::pmr::vector<db_expression> expressions(query_.resource());
        expressions.reserve(columns.size());
        for (const auto column : columns) {
            detail::require_entity_column<entity_type>(column);
            expressions.push_back(query_.column(column, alias_));
        }
        query_.distinct_on(expressions);
        return *this;
    }
    db_query_builder& distinct_on(std::initializer_list<std::string_view> columns) {
        return distinct_on(std::span<const std::string_view>(columns.begin(), columns.size()));
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
    template <typename target_entity, fixed_string field_name>
    db_query_builder& order_by(db_field_reference<target_entity, field_name> column,
        db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value) {
        const auto default_alias = std::same_as<target_entity, entity_type> ? std::string_view(alias_) : std::string_view{};
        query_.order_by(column.expression(query_, default_alias), direction, nulls);
        return *this;
    }
    template <typename target_entity, fixed_string field_name>
    db_query_builder& add_order_by(db_field_reference<target_entity, field_name> column,
        db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value) {
        const auto default_alias = std::same_as<target_entity, entity_type> ? std::string_view(alias_) : std::string_view{};
        query_.add_order_by(column.expression(query_, default_alias), direction, nulls);
        return *this;
    }
    db_query_builder& offset(std::uint64_t count) {
        query_.offset(count);
        return *this;
    }
    db_query_builder& limit(std::uint64_t count) {
        query_.limit(count);
        return *this;
    }
    db_query_builder& with_deleted(bool enabled = true) noexcept {
        with_deleted_ = enabled;
        return *this;
    }
    db_query_builder& left_join_and_select(std::string_view relation, std::string_view alias, const db_predicate& condition = {}) {
        return join_and_select(relation, alias, db_join_type::left, condition.expression(query_, entity_type::table_name(), alias_));
    }
    db_query_builder& inner_join_and_select(std::string_view relation, std::string_view alias, const db_predicate& condition = {}) {
        return join_and_select(relation, alias, db_join_type::inner, condition.expression(query_, entity_type::table_name(), alias_));
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
    [[nodiscard]] task<entity_rows<output_type>> get_many() const {
        auto mapper = projection_mapper<output_type>();
        auto active_query = effective_query();
        const auto& target_query = active_query ? *active_query : query_;
        if constexpr (std::same_as<output_type, entity_type>) {
            if (relations_ && !relations_->empty()) {
                auto prepared = relations_->template prepare<entity_type>(target_query, alias_, executor_.query_driver());
                return executor_.template query_mapped<entity_rows<entity_type>>(prepared ? *prepared : target_query,
                    detail::db_map_related_entities<entity_type>{relations_->clone()});
            }
        }
        return executor_.template query_mapped<entity_rows<output_type>>(target_query, std::move(mapper));
    }
    template <typename output_type = entity_type>
    [[nodiscard]] task<std::optional<output_type>> get_one() const {
        auto mapper = projection_mapper<output_type>();
        auto active_query = effective_query();
        auto query = (active_query ? *active_query : query_).clone(query_.resource());
        query.limit(1);
        // An id-keyed cache entry ignores the SQL, so each result shape derived
        // from one builder is keyed by the id together with its shape.
        query.copy_cache(query_, detail::db_cache_shape::one);
        if constexpr (std::same_as<output_type, entity_type>) {
            if (relations_ && !relations_->empty()) {
                auto prepared = relations_->template prepare<entity_type>(query, alias_, executor_.query_driver());
                return executor_.template query_mapped<std::optional<entity_type>>(prepared ? *prepared : query,
                    detail::db_map_one_related_entity<entity_type>{relations_->clone()});
            }
        }
        return executor_.template query_mapped<std::optional<output_type>>(query, detail::db_map_one_projection<output_type>{std::move(mapper)});
    }
    [[nodiscard]] task<db_rows> get_raw_many() const {
        auto active_query = effective_query();
        const auto& target_query = active_query ? *active_query : query_;
        return executor_.template query_mapped<db_rows>(target_query, detail::db_map_raw_rows{});
    }
    template <typename output_type = entity_type>
    [[nodiscard]] task<std::pair<entity_rows<output_type>, db_rows>> get_raw_and_entities() const {
        auto mapper = projection_mapper<output_type>(true);
        auto active_query = effective_query();
        const auto& target_query = active_query ? *active_query : query_;
        return executor_.template query_mapped<std::pair<entity_rows<output_type>, db_rows>>(
            target_query, detail::db_map_raw_and_entities<output_type>{std::move(mapper)});
    }
    template <typename scalar_type>
    [[nodiscard]] task<std::optional<scalar_type>> get_scalar() const {
        auto active_query = effective_query();
        auto query = (active_query ? *active_query : query_).clone(query_.resource());
        query.limit(1);
        query.copy_cache(query_, detail::db_cache_shape::one);
        return executor_.template query_mapped<std::optional<scalar_type>>(query, detail::db_map_scalar<scalar_type>{});
    }
    template <typename scalar_type>
    [[nodiscard]] task<std::pmr::vector<scalar_type>> get_scalars() const {
        auto active_query = effective_query();
        const auto& target_query = active_query ? *active_query : query_;
        return executor_.template query_mapped<std::pmr::vector<scalar_type>>(target_query, detail::db_map_scalars<scalar_type>{});
    }
    [[nodiscard]] task<db_stream_result> stream() const
        requires std::same_as<executor_type, db_handle>
    {
        const auto statement = get_query_and_parameters();
        return executor_.query_stream(statement.sql(), statement.params());
    }
    [[nodiscard]] task<std::uint64_t> get_count() const {
        const auto count = count_query();
        return executor_.template query_mapped<std::uint64_t>(count, detail::db_map_count{});
    }
    [[nodiscard]] task<bool> get_exists() const {
        auto active_query = effective_query();
        auto source_value = (active_query ? *active_query : query_).clone(query_.resource());
        source_value.clear_order().clear_lock().limit(std::nullopt).offset(std::nullopt);
        db_query query(query_.resource());
        query.select(query.alias(query.exists(source_value), "exists"));
        query.copy_cache(query_, detail::db_cache_shape::exists);
        return executor_.template query_mapped<bool>(query, detail::db_map_exists{});
    }
    template <typename output_type = entity_type>
    [[nodiscard]] task<std::pair<entity_rows<output_type>, std::uint64_t>> get_many_and_count() const {
        if (query_.has_writes()) {
            throw std::invalid_argument("get_many_and_count cannot execute a write CTE twice");
        }
        auto mapper = projection_mapper<output_type>();
        auto active_query = effective_query();
        const auto& target_query = active_query ? *active_query : query_;
        auto count = count_query();
        if constexpr (std::same_as<output_type, entity_type>) {
            if (relations_ && !relations_->empty()) {
                auto prepared = relations_->template prepare<entity_type>(target_query, alias_, executor_.query_driver());
                return executor_.template query_mapped_and_count<entity_rows<entity_type>>(prepared ? *prepared : target_query, count,
                    detail::db_map_related_entities<entity_type>{relations_->clone()});
            }
        }
        return executor_.template query_mapped_and_count<entity_rows<output_type>>(target_query, count, std::move(mapper));
    }
    [[nodiscard]] db_statement get_query_and_parameters() const {
        auto active_query = effective_query();
        const auto& target_query = active_query ? *active_query : query_;
        if (relations_ && !relations_->empty()) {
            auto prepared = relations_->template prepare<entity_type>(target_query, alias_, executor_.query_driver());
            return (prepared ? *prepared : target_query).compile(executor_.query_driver(), query_.resource());
        }
        return target_query.compile(executor_.query_driver(), query_.resource());
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
    template <typename joined_type>
    [[nodiscard]] db_expression import_join_predicate(const db_predicate& on, std::string_view join_alias) {
        if (on.empty()) {
            return {};
        }
        auto imported = on.expression(query_, entity_type::table_name(), alias_);
        if (!join_alias.empty() && joined_type::table_name() != entity_type::table_name()) {
            imported = query_.import_expression(imported, joined_type::table_name(), join_alias);
        }
        return imported;
    }
    template <typename output_type>
    detail::db_map_projection<output_type> projection_mapper(bool allow_extra_columns = false) const {
        if constexpr (!std::same_as<output_type, entity_type>) {
            require_plain_projection();
            if (selected_.empty()) {
                throw std::invalid_argument("DTO mapping requires an explicit projection");
            }
        }
        return detail::db_map_projection<output_type>(selected_.columns_, allow_extra_columns, selected_.wildcard_);
    }
    db_query_builder(detail::db_repository_input_type<executor_type> executor, std::string_view alias)
        : executor_(executor),
          query_(executor.query_resource()),
          alias_(alias.empty() ? detail::entity_query_alias<entity_type>() : alias, query_.resource()),
          selected_(query_.resource()) {
        detail::select_entity<entity_type>(query_, alias_);
    }
    [[nodiscard]] std::optional<db_query> effective_query() const {
        constexpr auto del_col = detail::delete_date_column<entity_type>();
        if constexpr (!del_col.empty()) {
            if (!with_deleted_) {
                auto filtered = query_.clone(query_.resource());
                const auto qualifier = alias_.empty() ? entity_type::table_name() : std::string_view(alias_);
                filtered.and_where(filtered.unary(db_unary_operator::is_null, filtered.column(del_col, qualifier)));
                return filtered;
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] db_query count_query() const {
        auto active_query = effective_query();
        auto source_value = (active_query ? *active_query : query_).clone(query_.resource());
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
        count.copy_cache(query_, detail::db_cache_shape::count);
        return count;
    }
    db_query_builder& join_and_select(std::string_view relation, std::string_view alias, db_join_type join, db_expression condition = {}) {
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
        relations_->template add<entity_type>(query_, alias_, relation, alias, join, condition);
        return *this;
    }
    detail::db_repository_executor_type<executor_type> executor_;
    db_query query_;
    std::pmr::string alias_;
    detail::db_projection selected_;
    std::optional<detail::db_relation_plan> relations_{};
    bool with_deleted_{false};
};

template <typename entity_type, typename executor_type>
class db_write_query_builder final {
public:
    db_write_query_builder(const db_write_query_builder&) = delete;
    db_write_query_builder& operator=(const db_write_query_builder&) = delete;
    db_write_query_builder(db_write_query_builder&&) = default;
    db_write_query_builder& operator=(db_write_query_builder&&) = delete;

    db_write_query_builder& where(const db_predicate& predicate) {
        require_condition(predicate);
        query_.where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_write_query_builder& and_where(const db_predicate& predicate) {
        require_condition(predicate);
        query_.and_where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_write_query_builder& or_where(const db_predicate& predicate) {
        require_condition(predicate);
        query_.or_where(predicate.expression(query_, entity_type::table_name(), alias_));
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
    template <detail::db_parameter v_type>
        requires(!std::same_as<std::remove_cvref_t<v_type>, db_expression>)
    db_write_query_builder& set(std::string_view column, v_type&& value) {
        detail::require_entity_column<entity_type>(column);
        if (detail::is_generated_entity_column<entity_type>(column)) {
            throw std::invalid_argument("SET requires a writable entity column and expression");
        }
        if constexpr (std::same_as<std::remove_cvref_t<v_type>, std::nullptr_t>) {
            return set_null(column);
        } else {
            query_.set(column, query_.value(detail::make_immediate_db_parameter(std::forward<v_type>(value))));
            return *this;
        }
    }
    db_write_query_builder& set_null(std::string_view column) {
        detail::require_entity_column<entity_type>(column);
        if (!detail::is_nullable_entity_column<entity_type>(column)) {
            throw std::invalid_argument("SET NULL requires a writable nullable entity column");
        }
        query_.set(column, query_.null_value());
        return *this;
    }
    db_write_query_builder& set_now(std::string_view column) {
        detail::require_entity_column<entity_type>(column);
        if (detail::is_generated_entity_column<entity_type>(column)) {
            throw std::invalid_argument("SET requires a writable entity column and expression");
        }
        query_.set(column, query_.call("now"));
        return *this;
    }
    template <typename number_type = std::int64_t>
        requires((std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    db_write_query_builder& increment(std::string_view column, number_type value = number_type{1}) {
        return adjust_number(column, value, db_binary_operator::add);
    }
    template <typename number_type = std::int64_t>
        requires((std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    db_write_query_builder& decrement(std::string_view column, number_type value = number_type{1}) {
        return adjust_number(column, value, db_binary_operator::subtract);
    }
    db_write_query_builder& set(const db_assignment& assignment) {
        detail::require_entity_column<entity_type>(assignment.column_);
        if (detail::is_generated_entity_column<entity_type>(assignment.column_) || assignment.empty()) {
            throw std::invalid_argument("SET requires a writable entity column and expression");
        }
        query_.set(assignment.column_, assignment.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_write_query_builder& set(std::span<const db_assignment> assignments) {
        for (const auto& assignment : assignments) {
            set(assignment);
        }
        return *this;
    }
    db_write_query_builder& set(std::initializer_list<db_assignment> assignments) {
        return set(std::span<const db_assignment>(assignments.begin(), assignments.size()));
    }
    db_write_query_builder& set(const entity_type& changes) {
        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
            if constexpr (detail::is_writable_entity_column_descriptor<c_type>()) {
                if (changes.template is_set<c_type::name>()) {
                    query_.set(c_type::name.view(), detail::entity_column_value<entity_type, c_type>(query_, changes));
                }
            }
        });
        return *this;
    }
    template <typename source>
    db_write_query_builder& update_from(std::string_view alias = {}) {
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
    db_write_query_builder& update_from_cte(std::string_view name, std::string_view alias = {}) {
        query_.update_from(name, alias);
        return *this;
    }
    template <typename source>
    db_write_query_builder& delete_using(std::string_view alias = {}) {
        static_assert(sql_entity<source>, "SQL delete sources require a SQL entity");
        query_.delete_using(source::table_name(), alias);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& delete_using(const db_query_builder<source, source_executor_type>& source_value, std::string_view alias) {
        source_value.require_plain_projection();
        query_.delete_using(source_value.query_, alias);
        return *this;
    }
    db_write_query_builder& delete_using_cte(std::string_view name, std::string_view alias = {}) {
        query_.delete_using(name, alias);
        return *this;
    }
    template <typename joined_type>
    db_write_query_builder& join(db_join_type type, std::string_view alias, const db_predicate& on = {}) {
        static_assert(sql_entity<joined_type>, "SQL joins require a SQL entity");
        query_.join(type, joined_type::table_name(), import_join_predicate<joined_type>(on, alias), alias);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& join(db_join_type type, const db_query_builder<source, source_executor_type>& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        source_value.require_plain_projection();
        query_.join(type, source_value.query_, import_join_predicate<source>(on, alias), alias, options);
        return *this;
    }
    db_write_query_builder& join(db_join_type type, const db_query& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        query_.join(type, source_value, on.expression(query_, entity_type::table_name(), alias_), alias, options);
        return *this;
    }
    db_write_query_builder& join_cte(db_join_type type, std::string_view name, std::string_view alias, const db_predicate& on = {}) {
        query_.join(type, name, on.expression(query_, entity_type::table_name(), alias_), alias);
        return *this;
    }
    db_write_query_builder& join_function(db_join_type type, db_expression function, std::string_view alias,
        const db_predicate& on = {}, const db_source_options& options = {}) {
        query_.join_function(type, query_.import_expression(function), on.expression(query_, entity_type::table_name(), alias_), alias, options);
        return *this;
    }
    db_write_query_builder& values(const entity_type& entity) {
        return values(std::span<const entity_type>(&entity, 1));
    }
    db_write_query_builder& values(std::initializer_list<entity_type> entities) {
        return values(std::span<const entity_type>(entities.begin(), entities.size()));
    }
    db_write_query_builder& values(std::span<const entity_type> entities) {
        if (!inserting_) {
            throw std::invalid_argument("values requires an insert builder");
        }
        if (entities.empty()) {
            throw std::invalid_argument("repository insert requires an entity");
        }
        std::array<bool, std::tuple_size_v<typename entity_type::columns_type>> selected{};
        std::pmr::vector<std::string_view> columns(query_.resource());
        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t index> {
            if constexpr (detail::is_writable_entity_column_descriptor<c_type>()) {
                if (std::ranges::any_of(entities, [](const entity_type& entity) { return entity.template is_set<c_type::name>(); })) {
                    selected[index] = true;
                    columns.push_back(c_type::name.view());
                }
            }
        });
        record_inserted_columns(columns);
        query_.insert_into(entity_type::table_name(), columns, alias_);
        if (columns.empty()) {
            if (entities.size() != 1) {
                throw std::invalid_argument("bulk default-only inserts require at least one explicit column");
            }
            return *this;
        }
        for (const auto& entity : entities) {
            std::pmr::vector<db_expression> row(query_.resource());
            row.reserve(columns.size());
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t index> {
                if constexpr (detail::is_writable_entity_column_descriptor<c_type>()) {
                    if (selected[index]) {
                        row.push_back(detail::entity_column_value<entity_type, c_type>(query_, entity));
                    }
                }
            });
            query_.values(row);
        }
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& insert_from(std::span<const std::string_view> columns, const db_query_builder<source, source_executor_type>& source_value) {
        if (!inserting_) {
            throw std::invalid_argument("insert_from requires an insert builder");
        }
        source_value.require_plain_projection();
        source_value.query_.require_select_query();
        validate_insert_columns(columns);
        const auto count = source_value.selected_.empty() ? std::tuple_size_v<typename source::columns_type> : source_value.selected_.columns_.size();
        if (!source_value.selected_.wildcard_ && count != columns.size()) {
            throw std::invalid_argument("insert_from projection and target column counts differ");
        }
        record_inserted_columns(columns);
        query_.insert_into(entity_type::table_name(), columns, alias_).insert_from(source_value.query_);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& insert_from(std::initializer_list<std::string_view> columns, const db_query_builder<source, source_executor_type>& source_value) {
        return insert_from(std::span<const std::string_view>(columns.begin(), columns.size()), source_value);
    }
    db_write_query_builder& insert_from(std::span<const std::string_view> columns, const db_query& source_value) {
        if (!inserting_) {
            throw std::invalid_argument("insert_from requires an insert builder");
        }
        source_value.require_select_query();
        validate_insert_columns(columns);
        record_inserted_columns(columns);
        query_.insert_into(entity_type::table_name(), columns, alias_).insert_from(source_value);
        return *this;
    }
    db_write_query_builder& insert_from(std::initializer_list<std::string_view> columns, const db_query& source_value) {
        return insert_from(std::span<const std::string_view>(columns.begin(), columns.size()), source_value);
    }
    db_write_query_builder& on_conflict(const db_upsert_options& options) {
        if (!inserting_) {
            throw std::invalid_argument("on_conflict requires an insert builder");
        }
        std::pmr::vector<std::string_view> columns(query_.resource());
        columns.reserve(inserted_columns_.size());
        for (const auto& column : inserted_columns_) {
            columns.push_back(column);
        }
        detail::apply_entity_upsert<entity_type>(query_, columns, options, executor_.query_driver(), qualifier());
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& with(std::string_view name, const db_query_builder<source, source_executor_type>& source_value, const db_cte_options& options = {}) {
        query_.with(name, source_value.query(), options);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& with(std::string_view name, const db_write_query_builder<source, source_executor_type>& source_value, const db_cte_options& options = {}) {
        query_.with(name, source_value.query(), options);
        return *this;
    }
    db_write_query_builder& with(std::string_view name, const db_query& source_value, const db_cte_options& options = {}) {
        query_.with(name, source_value, options);
        return *this;
    }
    db_write_query_builder& with(std::string_view name, const db_values_builder& values,
        db_materialization materialization = db_materialization::default_value) {
        query_.with_columns(name, values.query(), materialization, values.columns());
        return *this;
    }
    db_write_query_builder& returning(std::span<const db_selection> fields = {}) {
        if (fields.empty()) {
            std::pmr::vector<db_expression> expressions(query_.resource());
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> { expressions.push_back(query_.column(c_type::name.view(), qualifier())); });
            query_.returning(expressions);
            selected_.columns_.clear();
            selected_.wildcard_ = false;
        } else {
            for (const auto& field : fields) {
                if (!field.has_expression() && !field.is_wildcard()) {
                    detail::require_entity_column<entity_type>(field.column_);
                }
            }
            selected_ = detail::apply_projection(query_, fields, qualifier(), true, entity_type::table_name());
        }
        return *this;
    }
    db_write_query_builder& returning(std::initializer_list<db_selection> fields_value) {
        return returning(std::span<const db_selection>(fields_value.begin(), fields_value.size()));
    }
    db_write_query_builder& returning(std::span<const std::string_view> columns) {
        std::pmr::vector<db_selection> selections(query_.resource());
        selections.reserve(columns.size());
        for (const auto column : columns) {
            selections.emplace_back(column);
        }
        return returning(selections);
    }
    [[nodiscard]] task<db_exec_result> execute() const {
        validate();
        return executor_.execute_query(query_);
    }
    template <typename output_type = entity_type>
    [[nodiscard]] task<entity_rows<output_type>> get_many() const {
        validate();
        if (!query_.returns_rows()) {
            throw std::invalid_argument("get_many requires RETURNING");
        }
        if constexpr (!std::same_as<output_type, entity_type>) {
            if (selected_.empty()) {
                throw std::invalid_argument("DTO returning requires an explicit projection");
            }
        }
        return executor_.template query_mapped<entity_rows<output_type>>(query_, detail::db_map_projection<output_type>(selected_.columns_, false, selected_.wildcard_));
    }
    template <typename output_type = entity_type>
    [[nodiscard]] task<std::optional<output_type>> get_one() const {
        validate();
        if (!query_.returns_rows()) {
            throw std::invalid_argument("get_one requires RETURNING");
        }
        if constexpr (!std::same_as<output_type, entity_type>) {
            if (selected_.empty()) {
                throw std::invalid_argument("DTO returning requires an explicit projection");
            }
        }
        return executor_.template query_mapped<std::optional<output_type>>(query_,
            detail::db_map_one_projection<output_type>{detail::db_map_projection<output_type>(selected_.columns_, false, selected_.wildcard_)});
    }
    [[nodiscard]] task<db_rows> get_raw_many() const {
        validate();
        if (!query_.returns_rows()) {
            throw std::invalid_argument("get_raw_many requires RETURNING");
        }
        return executor_.template query_mapped<db_rows>(query_, detail::db_map_raw_rows{});
    }
    template <typename scalar_type>
    [[nodiscard]] task<std::optional<scalar_type>> get_scalar() const {
        validate();
        if (!query_.returns_rows()) {
            throw std::invalid_argument("get_scalar requires RETURNING");
        }
        return executor_.template query_mapped<std::optional<scalar_type>>(query_, detail::db_map_scalar<scalar_type>{});
    }
    template <typename scalar_type>
    [[nodiscard]] task<std::pmr::vector<scalar_type>> get_scalars() const {
        validate();
        if (!query_.returns_rows()) {
            throw std::invalid_argument("get_scalars requires RETURNING");
        }
        return executor_.template query_mapped<std::pmr::vector<scalar_type>>(query_, detail::db_map_scalars<scalar_type>{});
    }
    [[nodiscard]] db_statement get_query_and_parameters() const {
        validate();
        return query_.compile(executor_.query_driver(), query_.resource());
    }
    // Borrows the complete validated DML AST, including RETURNING and parameters.
    // The builder and its resource must outlive the view; with() clones on import.
    [[nodiscard]] const db_query& query() const& RUVIA_LIFETIMEBOUND {
        validate();
        return query_;
    }
    const db_query& query() const&& = delete;

private:
    friend class db_repository<entity_type, executor_type>;
    template <typename, typename>
    friend class db_query_builder;
    template <typename, typename>
    friend class db_write_query_builder;
    db_write_query_builder(detail::db_repository_input_type<executor_type> executor, db_query query, bool inserting,
        std::string_view alias = {}, std::span<const std::string_view> inserted_columns = {})
        : executor_(executor),
          query_(std::move(query)),
          inserting_(inserting),
          alias_(alias, query_.resource()),
          selected_(query_.resource()),
          inserted_columns_(query_.resource()) {
        record_inserted_columns(inserted_columns);
    }
    std::string_view qualifier() const {
        return alias_.empty() ? entity_type::table_name() : std::string_view(alias_);
    }
    template <typename number_type>
    db_write_query_builder& adjust_number(std::string_view column, number_type value, db_binary_operator op) {
        detail::require_entity_column<entity_type>(column);
        if (!detail::is_numeric_entity_column<entity_type>(column)) {
            throw std::invalid_argument("increment/decrement requires a writable numeric column");
        }
        query_.set(column, query_.binary(query_.column(column, qualifier()), op, query_.value(value)));
        return *this;
    }
    template <typename joined_type>
    [[nodiscard]] db_expression import_join_predicate(const db_predicate& on, std::string_view join_alias) {
        if (on.empty()) {
            return {};
        }
        auto imported = on.expression(query_, entity_type::table_name(), alias_);
        if (!join_alias.empty() && joined_type::table_name() != entity_type::table_name()) {
            imported = query_.import_expression(imported, joined_type::table_name(), join_alias);
        }
        return imported;
    }
    static void validate_insert_columns(std::span<const std::string_view> columns) {
        if (columns.empty()) {
            throw std::invalid_argument("insert_from requires target columns");
        }
        for (const auto column : columns) {
            detail::require_entity_column<entity_type>(column);
            if (detail::is_generated_entity_column<entity_type>(column)) {
                throw std::invalid_argument("insert_from cannot write a generated column");
            }
        }
    }
    void record_inserted_columns(std::span<const std::string_view> columns) {
        inserted_columns_.clear();
        inserted_columns_.reserve(columns.size());
        for (const auto column : columns) {
            inserted_columns_.emplace_back(column);
        }
    }
    void require_condition(const db_predicate& condition) const {
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
    detail::db_projection selected_;
    std::pmr::vector<std::pmr::string> inserted_columns_;
};

template <typename entity_type, typename executor_type>
class db_repository final {
    static_assert(sql_entity<entity_type>, "SQL repositories require a RUVIA_DB_ENTITY declaration");

public:
    [[nodiscard]] db_query_builder<entity_type, executor_type> query_builder(std::string_view alias = {}) const {
        return db_query_builder<entity_type, executor_type>(executor_, alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> update_builder(std::string_view alias = {}) const {
        db_query query(executor_.query_resource());
        query.update(entity_type::table_name(), alias);
        return db_write_query_builder<entity_type, executor_type>(executor_, std::move(query), false, alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> delete_builder(std::string_view alias = {}) const {
        db_query query(executor_.query_resource());
        query.delete_from(entity_type::table_name(), alias);
        return db_write_query_builder<entity_type, executor_type>(executor_, std::move(query), false, alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> insert_builder(std::string_view alias = {}) const {
        db_query query(executor_.query_resource());
        query.insert_into(entity_type::table_name(), {}, alias);
        return db_write_query_builder<entity_type, executor_type>(executor_, std::move(query), true, alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> insert_builder(const entity_type& entity, std::string_view alias = {}) const {
        return insert_builder(std::span<const entity_type>(&entity, 1), alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> insert_builder(std::span<const entity_type> entities, std::string_view alias = {}) const {
        std::pmr::vector<std::string_view> columns(executor_.query_resource());
        auto query = insert_query(entities, &columns, alias);
        return db_write_query_builder<entity_type, executor_type>(executor_, std::move(query), true, alias, columns);
    }
    [[nodiscard]] task<entity_rows<entity_type>> find(const db_find_options& options = {}) const {
        return find_builder(options).get_many();
    }
    [[nodiscard]] task<std::optional<entity_type>> find_one(const db_find_options& options) const {
        return find_builder(options).get_one();
    }
    [[nodiscard]] task<std::pair<entity_rows<entity_type>, std::uint64_t>> find_and_count(const db_find_options& options = {}) const {
        return find_builder(options).get_many_and_count();
    }
    [[nodiscard]] task<std::uint64_t> count(const db_find_options& options = {}) const {
        return find_builder(options).get_count();
    }
    [[nodiscard]] task<bool> exists(const db_find_options& options = {}) const {
        return find_builder(options).get_exists();
    }
    [[nodiscard]] task<entity_type> save(const entity_type& entity) const {
        auto query = save_query(std::span<const entity_type>(&entity, 1));
        append_entity_returning(query);
        return executor_.template query_mapped<entity_type>(
            query, detail::db_map_required_one<entity_type>{detail::db_map_projection<entity_type>({})});
    }
    [[nodiscard]] task<entity_rows<entity_type>> save(std::span<const entity_type> entities) const {
        auto query = save_query(entities);
        append_entity_returning(query);
        return executor_.template query_mapped<entity_rows<entity_type>>(
            query, detail::db_map_projection<entity_type>({}));
    }
    [[nodiscard]] task<db_exec_result> soft_delete(const db_predicate& predicate) const
        requires(!detail::delete_date_column<entity_type>().empty())
    {
        if (predicate.empty()) {
            throw std::invalid_argument("repository soft_delete requires a condition");
        }
        auto builder = update_builder();
        constexpr auto del_col = detail::delete_date_column<entity_type>();
        builder.set_now(del_col);
        constexpr auto upd_col = detail::update_date_column<entity_type>();
        if constexpr (!upd_col.empty()) {
            builder.set_now(upd_col);
        }
        constexpr auto ver_col = detail::version_column<entity_type>();
        if constexpr (!ver_col.empty()) {
            builder.increment(ver_col, 1);
        }
        builder.where(predicate);
        return builder.execute();
    }
    [[nodiscard]] task<db_exec_result> restore(const db_predicate& predicate) const
        requires(!detail::delete_date_column<entity_type>().empty())
    {
        if (predicate.empty()) {
            throw std::invalid_argument("repository restore requires a condition");
        }
        auto builder = update_builder();
        constexpr auto del_col = detail::delete_date_column<entity_type>();
        builder.set_null(del_col);
        constexpr auto upd_col = detail::update_date_column<entity_type>();
        if constexpr (!upd_col.empty()) {
            builder.set_now(upd_col);
        }
        builder.where(predicate);
        return builder.execute();
    }
    [[nodiscard]] task<db_exec_result> clear() const {
        const auto sql = detail::format_truncate_table(entity_type::table_name(), executor_.query_driver(), executor_.query_resource());
        return executor_.execute(sql);
    }
    [[nodiscard]] task<db_exec_result> insert(const entity_type& entity) const {
        return insert(std::span<const entity_type>(&entity, 1));
    }
    [[nodiscard]] task<db_exec_result> insert(std::span<const entity_type> entities) const {
        auto query = insert_query(entities);
        return executor_.execute_query(query);
    }
    [[nodiscard]] task<db_exec_result> update(const db_predicate& predicate, const entity_type& changes) const {
        auto query = update_query(predicate, changes);
        return executor_.execute_query(query);
    }
    [[nodiscard]] task<db_exec_result> delete_by(const db_predicate& predicate) const {
        if (predicate.empty()) {
            throw std::invalid_argument("repository delete_by requires a condition");
        }
        db_query query(executor_.query_resource());
        query.delete_from(entity_type::table_name()).where(predicate.expression(query, entity_type::table_name()));
        return executor_.execute_query(query);
    }
    template <typename number_type = std::int64_t>
        requires((std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] task<db_exec_result> increment(const db_predicate& predicate, std::string_view property_path, number_type value = number_type{1}) const {
        return adjust_number(predicate, property_path, value, db_binary_operator::add);
    }
    template <typename number_type = std::int64_t>
        requires((std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] task<db_exec_result> decrement(const db_predicate& predicate, std::string_view property_path, number_type value = number_type{1}) const {
        return adjust_number(predicate, property_path, value, db_binary_operator::subtract);
    }
    [[nodiscard]] task<db_exec_result> remove(const entity_type& entity) const {
        db_query query(executor_.query_resource());
        query.delete_from(entity_type::table_name());
        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
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
        return executor_.execute_query(query);
    }
    [[nodiscard]] task<db_exec_result> upsert(const entity_type& entity, const db_upsert_options& options) const {
        return upsert(std::span<const entity_type>(&entity, 1), options);
    }
    [[nodiscard]] task<db_exec_result> upsert(std::span<const entity_type> entities, const db_upsert_options& options) const {
        auto query = upsert_query(entities, options);
        return executor_.execute_query(query);
    }

    [[nodiscard]] task<db_exec_result> update(const db_predicate& predicate, std::span<const db_assignment> changes) const {
        auto query = update_query(predicate, changes);
        return executor_.execute_query(query);
    }
    [[nodiscard]] task<db_exec_result> update(const db_predicate& predicate, std::initializer_list<db_assignment> changes) const {
        return update(predicate, std::span<const db_assignment>(changes.begin(), changes.size()));
    }

private:
    friend class db_handle;
    friend class db_transaction;
    explicit db_repository(detail::db_repository_input_type<executor_type> executor)
        : executor_(executor) {}
    static void append_entity_returning(db_query& query) {
        std::pmr::vector<db_expression> values(query.resource());
        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
            values.push_back(query.column(c_type::name.view()));
        });
        query.returning(values);
    }
    template <typename number_type>
    [[nodiscard]] task<db_exec_result> adjust_number(const db_predicate& predicate, std::string_view property_path, number_type value, db_binary_operator op) const {
        if (predicate.empty()) {
            throw std::invalid_argument("repository increment/decrement requires a condition");
        }
        if (!detail::is_numeric_entity_column<entity_type>(property_path)) {
            throw std::invalid_argument("repository increment/decrement requires a writable numeric column");
        }
        db_query query(executor_.query_resource());
        query.update(entity_type::table_name()).where(predicate.expression(query, entity_type::table_name()));
        query.set(property_path, query.binary(query.column(property_path), op, query.value(value)));
        return executor_.execute_query(query);
    }
    [[nodiscard]] db_query save_query(std::span<const entity_type> entities) const {
        constexpr auto keys = detail::db_primary_key_columns<entity_type>();
        if constexpr (keys.empty()) {
            return insert_query(entities);
        } else {
            bool all_keys_set = true;
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
                if constexpr (c_type::options.primary_key_) {
                    for (const auto& entity : entities) {
                        all_keys_set &= entity.template is_set<c_type::name>() && !entity.template is_null<c_type::name>();
                    }
                }
            });
            if (!all_keys_set) {
                return insert_query(entities);
            }
            std::pmr::vector<std::string_view> columns(executor_.query_resource());
            auto query = insert_query(entities, &columns);
            const auto driver = executor_.query_driver();
            constexpr auto upd_col = detail::update_date_column<entity_type>();
            constexpr auto ver_col = detail::version_column<entity_type>();
            std::pmr::vector<borrowed_text> conflict_columns(query.resource());
            if (driver == db_driver::postgresql) {
                conflict_columns.assign(keys.begin(), keys.end());
            }
            std::pmr::vector<db_assignment> updates(query.resource());
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
                if constexpr (detail::is_writable_entity_column_descriptor<c_type>()) {
                    constexpr auto name = c_type::name.view();
                    if constexpr (!upd_col.empty() && name == upd_col) {
                        updates.push_back(db_assignment{name, db_expr(query.call("now"), {}, true)});
                    } else if constexpr (!ver_col.empty() && name == ver_col) {
                        updates.push_back(db_assignment{name,
                            db_expr(query.binary(query.column(name, entity_type::table_name()), db_binary_operator::add, query.value(1)), {}, true)});
                    } else if constexpr (!c_type::options.primary_key_ && !c_type::options.generated_) {
                        std::size_t set_count = 0;
                        for (const auto& entity : entities) {
                            if (entity.template is_set<c_type::name>()) {
                                ++set_count;
                            }
                        }
                        if (set_count != 0 && set_count != entities.size()) {
                            throw std::invalid_argument("bulk upsert requires identical set columns or explicit update_columns");
                        }
                        if (std::ranges::find(columns, name) != columns.end()) {
                            updates.push_back(db_assignment{name, db_expr(query.excluded(name), {}, true)});
                        }
                    }
                }
            });
            const bool do_nothing = updates.empty() && driver == db_driver::postgresql;
            const bool any_unique_key = driver != db_driver::postgresql;
            query.on_conflict({
                .columns_ = conflict_columns,
                .update_ = updates,
                .do_nothing_ = do_nothing,
                .any_unique_key_ = any_unique_key,
            });
            return query;
        }
    }
    [[nodiscard]] db_query_builder<entity_type, executor_type> find_builder(const db_find_options& options) const {
        auto builder = query_builder();
        builder.with_deleted(options.with_deleted_);
        if (!options.select_.empty()) {
            std::pmr::vector<db_selection> selections(builder.query_.resource());
            selections.reserve(options.select_.size());
            for (const auto& col : options.select_) {
                selections.emplace_back(col);
            }
            builder.select(selections);
        }
        detail::apply_find_options<entity_type>(builder.query_, options, builder.alias_);
        if (!options.relations_.empty()) {
            builder.relations_.emplace(builder.query_.resource());
            for (const auto& path : options.relations_) {
                builder.relations_->template add<entity_type>(builder.query_, builder.alias_, path);
            }
        }
        return builder;
    }
    db_query update_query(const db_predicate& predicate, const entity_type& changes) const {
        if (predicate.empty()) {
            throw std::invalid_argument("repository update requires a condition");
        }
        db_query query(executor_.query_resource());
        query.update(entity_type::table_name()).where(predicate.expression(query, entity_type::table_name()));
        bool selected = false;
        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
            if constexpr (detail::is_writable_entity_column_descriptor<c_type>()) {
                if (changes.template is_set<c_type::name>()) {
                    query.set(c_type::name.view(), detail::entity_column_value<entity_type, c_type>(query, changes));
                    selected = true;
                } else if constexpr (c_type::options.update_date_) {
                    query.set(c_type::name.view(), query.call("now"));
                    selected = true;
                } else if constexpr (c_type::options.version_) {
                    query.set(c_type::name.view(), query.binary(query.column(c_type::name.view(), entity_type::table_name()), db_binary_operator::add, query.value(1)));
                    selected = true;
                }
            }
        });
        if (!selected) {
            throw std::invalid_argument("repository update requires a writable column");
        }
        return query;
    }
    db_query update_query(const db_predicate& predicate, std::span<const db_assignment> changes) const {
        if (predicate.empty() || changes.empty()) {
            throw std::invalid_argument("repository update requires a condition and writable columns");
        }
        db_query query(executor_.query_resource());
        query.update(entity_type::table_name()).where(predicate.expression(query, entity_type::table_name()));
        for (std::size_t i = 0; i < changes.size(); ++i) {
            const auto& item = changes[i];
            detail::require_entity_column<entity_type>(item.column_);
            if (detail::is_generated_entity_column<entity_type>(item.column_) || item.empty()) {
                throw std::invalid_argument("update expression requires a writable column");
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (changes[j].column_ == item.column_) {
                    throw std::invalid_argument("duplicate update column");
                }
            }
            query.set(item.column_, item.expression(query, entity_type::table_name()));
        }
        return query;
    }
    db_query upsert_query(std::span<const entity_type> entities, const db_upsert_options& options) const {
        std::pmr::vector<std::string_view> columns(executor_.query_resource());
        auto query = insert_query(entities, &columns);
        if (!options.do_nothing_ && options.update_columns_.empty()) {
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
                if constexpr (!c_type::options.primary_key_ && !c_type::options.generated_ &&
                              detail::is_writable_entity_column_descriptor<c_type>()) {
                    const auto name = c_type::name.view();
                    const bool has_custom = std::ranges::any_of(options.update_expressions_, [name](const auto& item) { return item.column_ == name; });
                    if (!has_custom) {
                        std::size_t set_count = 0;
                        for (const auto& entity : entities) {
                            if (entity.template is_set<c_type::name>()) {
                                ++set_count;
                            }
                        }
                        if (set_count != 0 && set_count != entities.size()) {
                            throw std::invalid_argument("bulk upsert requires identical set columns or explicit update_columns");
                        }
                    }
                }
            });
        }
        detail::apply_entity_upsert<entity_type>(query, columns, options, executor_.query_driver(), entity_type::table_name());
        return query;
    }
    db_query insert_query(std::span<const entity_type> entities,
        std::pmr::vector<std::string_view>* out_columns = nullptr, std::string_view alias = {}) const {
        if (entities.empty()) {
            throw std::invalid_argument("repository insert requires an entity");
        }
        db_query query(executor_.query_resource());
        std::array<bool, std::tuple_size_v<typename entity_type::columns_type>> selected{};
        std::pmr::vector<std::string_view> columns(query.resource());
        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t index> {
            if constexpr (detail::is_writable_entity_column_descriptor<c_type>()) {
                if (std::ranges::any_of(entities, [](const entity_type& entity) { return entity.template is_set<c_type::name>(); })) {
                    selected[index] = true;
                    columns.push_back(c_type::name.view());
                }
            }
        });
        if (out_columns != nullptr) {
            *out_columns = columns;
        }
        query.insert_into(entity_type::table_name(), columns, alias);
        if (columns.empty()) {
            if (entities.size() != 1) {
                throw std::invalid_argument("bulk default-only inserts require at least one explicit column");
            }
            return query;
        }
        for (const auto& entity : entities) {
            std::pmr::vector<db_expression> row(query.resource());
            row.reserve(columns.size());
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t index> {
                if constexpr (detail::is_writable_entity_column_descriptor<c_type>()) {
                    if (selected[index]) {
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
db_repository<entity_type, db_handle> db_handle::repository() const {
    registration_.require_active();
    return db_repository<entity_type, db_handle>(*this);
}
template <typename entity_type>
db_repository<entity_type, db_transaction> db_transaction::repository() & {
    (void)query_resource();
    return db_repository<entity_type, db_transaction>(*this);
}

}  // namespace ruvia
