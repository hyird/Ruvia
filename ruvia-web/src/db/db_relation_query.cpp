#include "ruvia/web/detail/db/db_relation_query.h"

#include <cstdint>

#include "ruvia/web/detail/db/db_utils.h"

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

std::pmr::string db_relation_plan::unique_alias(const db_query& query, std::string_view reserved) {
    for (;;) {
        std::pmr::string alias("__ruvia_relation_", resource_);
        append_db_number(alias, static_cast<std::uint64_t>(next_alias_++));
        if (alias != reserved && !query.uses_source_name(alias)) {
            return alias;
        }
    }
}

std::pmr::string db_relation_plan::unique_projection(const db_query& query) {
    for (;;) {
        std::pmr::string name("__ruvia_column_", resource_);
        append_db_number(name, static_cast<std::uint64_t>(next_projection_++));
        if (!query.uses_projection_name(name)) {
            return name;
        }
    }
}

void db_relation_plan::conjunction(db_query& query, db_expression& predicate, db_expression term) {
    predicate = predicate.empty() ? term : query.binary(predicate, db_binary_operator::and_value, term);
}

}  // namespace ruvia::detail
