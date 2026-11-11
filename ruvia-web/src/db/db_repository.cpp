#include "ruvia/web/db/db_repository.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

#include "db/db_sql_format.h"
#include "query_storage.h"

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

db_relation_plan db_relation_plan::clone() const {
    db_relation_plan result(resource_);
    result.column_count_ = column_count_;
    result.next_alias_ = next_alias_;
    result.next_projection_ = next_projection_;
    result.nodes_.reserve(nodes_.size());
    for (const auto& node : nodes_) {
        result.nodes_.emplace_back(node, resource_);
    }
    return result;
}

std::size_t db_relation_plan::find(std::size_t parent_value, std::size_t relation) const noexcept {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].parent_ == parent_value && nodes_[i].relation_ == relation) {
            return i;
        }
    }
    return root;
}

std::pmr::string db_relation_plan::normalize(std::string_view root_alias, std::string_view path) const {
    if (path.size() > root_alias.size() && path.starts_with(root_alias) && path[root_alias.size()] == '.') {
        path.remove_prefix(root_alias.size() + 1);
    } else if (const auto dot = path.find('.'); dot != std::string_view::npos) {
        for (const auto& node : nodes_) {
            if (path.substr(0, dot) == node.alias_) {
                const auto suffix = path.substr(dot);
                std::pmr::string result_value(resource_);
                result_value.reserve(node.path_.size() + suffix.size());
                result_value.append(node.path_);
                result_value.append(suffix);
                return result_value;
            }
        }
    }
    return std::pmr::string(path, resource_);
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
    auto result_value = query.clone(s.resource_);
    if (scope_lock) {
        result_value.storage().lock_->tables_.emplace_back(root_alias);
    }
    if (!paginated) {
        return result_value;
    }

    // Select a page of root identities before filtering the expanded result.
    // Ranking makes joined ordering deterministic for each root, and avoids
    // applying LIMIT to child rows or truncating a loaded collection.
    auto ranked = query.clone(s.resource_);
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
    std::pmr::vector<std::pmr::string> key_names(s.resource_);
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
    std::pmr::vector<std::pmr::string> order_names(s.resource_);
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

    db_query page(s.resource_);
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
    db_query included(s.resource_);
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

std::pmr::string db_relation_plan::unique_alias(const db_query& query, std::string_view reserved) {
    constexpr std::string_view prefix = "__ruvia_relation_";
    std::pmr::string alias(resource_);
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

std::pmr::string db_relation_plan::unique_projection(const db_query& query) {
    constexpr std::string_view prefix = "__ruvia_column_";
    std::pmr::string name(resource_);
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
    db_projection result(query.resource());
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

std::pmr::string format_truncate_table(std::string_view table, db_driver driver, std::pmr::memory_resource* resource) {
    std::pmr::string sql(pmr_resource_or_default(resource));
    sql.reserve(17U + table.size());
    sql.append("TRUNCATE TABLE ");
    append_db_qualified_identifier(sql, table, driver);
    return sql;
}

}  // namespace ruvia::detail
