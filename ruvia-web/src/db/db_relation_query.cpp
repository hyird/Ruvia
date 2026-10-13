#include "ruvia/web/detail/db/db_relation_query.h"

#include <algorithm>
#include <cstdint>

#include "ruvia/web/detail/db/db_utils.h"

#include "query_storage.h"

namespace ruvia::detail {

db_relation_plan db_relation_plan::clone() const {
    db_relation_plan result(resource_);
    result.column_count_ = column_count_;
    result.next_alias_ = next_alias_;
    result.next_projection_ = next_projection_;
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
                std::pmr::string result_value(node.path_, resource_);
                result_value.append(path.substr(dot));
                return result_value;
            }
        }
    }
    return std::pmr::string(path, resource_);
}

bool db_relation_plan::uses_source_name(const db_query& query, std::string_view name) {
    const auto matches = [name](const db_query_source& source_value) {
        if (!source_value.alias_.empty()) {
            return source_value.alias_ == name;
        }
        const auto full = std::string_view(source_value.name_);
        const auto dot = full.rfind('.');
        return full == name || full.substr(dot == std::string_view::npos ? 0 : dot + 1) == name;
    };
    const auto& s = query.storage();
    return (s.source_ && matches(*s.source_)) || std::ranges::any_of(s.joins_,
                                                     [&](const auto& join) { return matches(join.source_); });
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
    auto result_value = query.clone(s.resource_);
    if (scope_lock) {
        result_value.storage().lock_->tables_.emplace_back(root_alias);
    }
    if (!paginated) {
        return result_value;
    }
    if (query.has_grouping() || !s.set_operations_.empty() || !s.distinct_on_.empty()) {
        throw std::invalid_argument("paged relation loading requires an ungrouped SELECT without set operations or DISTINCT ON");
    }
    if (s.lock_ && s.lock_->skip_locked_) {
        throw std::invalid_argument("paged relation loading cannot combine SKIP LOCKED with collection expansion");
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
    std::pmr::vector<std::pmr::string> key_names(s.resource_), order_names(s.resource_);
    for (std::size_t i = 0; i < primary_key.size(); ++i) {
        auto& name = key_names.emplace_back("__ruvia_page_key_");
        append_db_number(name, static_cast<std::uint64_t>(i));
        const auto key = ranked.column(primary_key[i], root_alias);
        window.args_.push_back(ranked.require_expression(key));
        ranked.add_select(ranked.alias(key, name));
    }
    if (orders.empty()) {
        for (const auto key : window.args_) {
            orders.push_back({key, db_order_direction::asc, db_nulls_order::default_value});
        }
    }
    // Give the outer result the same stable tie order as its page.
    for (const auto key : primary_key) {
        result_value.add_order_by(result_value.column(key, root_alias));
    }
    for (std::size_t i = 0; i < orders.size(); ++i) {
        auto& name = order_names.emplace_back("__ruvia_page_order_");
        append_db_number(name, static_cast<std::uint64_t>(i));
        ranked.add_select(ranked.alias(db_expression(&rank_storage, orders[i].expression_), name));
    }
    window.orders_ = orders;
    ranked.add_select(ranked.alias(ranked.push_node(std::move(window)), "__ruvia_page_rank"));

    db_query page(s.resource_);
    for (const auto& key : key_names) {
        page.add_select(page.column(key, "__ruvia_ranked"));
    }
    page.from(ranked, "__ruvia_ranked");
    page.where(page.binary(page.column("__ruvia_page_rank", "__ruvia_ranked"),
        db_binary_operator::equal, page.value(1)));
    for (std::size_t i = 0; i < orders.size(); ++i) {
        page.add_order_by(page.column(order_names[i], "__ruvia_ranked"), orders[i].direction_, orders[i].nulls_);
    }
    // Break ordering ties using the complete primary key, including composite keys.
    for (const auto& key : key_names) {
        page.add_order_by(page.column(key, "__ruvia_ranked"));
    }
    page.limit(s.limit_).offset(s.offset_);
    db_query included(s.resource_);
    const auto page_alias = root_alias == "__ruvia_page" ? "__ruvia_page_1" : "__ruvia_page";
    included.select(included.value(1)).from(page, page_alias);
    for (std::size_t i = 0; i < primary_key.size(); ++i) {
        included.and_where(included.binary(included.column(key_names[i], page_alias),
            db_binary_operator::equal, included.column(primary_key[i], root_alias)));
    }
    result_value.limit(std::nullopt).offset(std::nullopt);
    result_value.and_where(result_value.exists(included));
    return result_value;
}

std::pmr::string db_relation_plan::unique_alias(const db_query& query, std::string_view reserved) {
    for (;;) {
        std::pmr::string alias("__ruvia_relation_", resource_);
        append_db_number(alias, static_cast<std::uint64_t>(next_alias_++));
        if (alias != reserved && !uses_source_name(query, alias)) {
            return alias;
        }
    }
}

std::pmr::string db_relation_plan::unique_projection(const db_query& query) {
    for (;;) {
        std::pmr::string name("__ruvia_column_", resource_);
        append_db_number(name, static_cast<std::uint64_t>(next_projection_++));
        if (!uses_projection_name(query, name)) {
            return name;
        }
    }
}

void db_relation_plan::conjunction(db_query& query, db_expression& predicate, db_expression term) {
    predicate = predicate.empty() ? term : query.binary(predicate, db_binary_operator::and_value, term);
}

}  // namespace ruvia::detail
