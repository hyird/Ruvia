#include "ruvia/web/db/db_query.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>

#include "ruvia/core/memory/pmr_resource.h"

#include "db/db_sql_format.h"
#include "query_storage.h"

namespace ruvia::detail {

namespace {
void require_name(std::string_view name, bool qualified = false) {
    if (name.empty() || name.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("database identifier must be nonempty and contain no NUL");
    }
    if (qualified && (name.front() == '.' || name.back() == '.' || name.find("..") != std::string_view::npos)) {
        throw std::invalid_argument("database qualified identifier has an empty component");
    }
}
void set_source_options(db_query_source& source_value, const db_source_options& options, std::pmr::memory_resource* resource) {
    source_value.lateral_ = options.lateral_;
    source_value.ordinality_ = options.with_ordinality_;
    for (const auto& column : options.columns_) {
        require_name(column.name_);
        source_value.columns_.emplace_back(column, resource);
    }
}
}  // namespace

struct qualified_expression_importer final {
    std::span<const db_query_node> source_nodes_;
    std::span<const db_query> source_queries_;
    db_query_storage& target_;
    std::string_view source_qualifier_;
    std::string_view target_qualifier_;
    std::pmr::vector<std::size_t> mapped_;

    qualified_expression_importer(std::span<const db_query_node> source_nodes,
        std::span<const db_query> source_queries, db_query_storage& target,
        std::string_view source_qualifier, std::string_view target_qualifier)
        : source_nodes_(source_nodes),
          source_queries_(source_queries),
          target_(target),
          source_qualifier_(source_qualifier),
          target_qualifier_(target_qualifier),
          mapped_(source_nodes.size(), no_db_node, target.resource_) {}

    qualified_expression_importer(const db_query_storage& source_value, db_query_storage& target,
        std::string_view source_qualifier, std::string_view target_qualifier)
        : qualified_expression_importer(
              source_value.nodes_, source_value.queries_, target, source_qualifier, target_qualifier) {}
    [[nodiscard]] static bool binds_qualifier(const db_query_source& src, std::string_view qualifier) noexcept {
        if (!src.alias_.empty()) {
            return src.alias_ == qualifier;
        }
        const std::string_view name = src.name_;
        // Unaliased qualified tables also bind their unqualified table name.
        return name == qualifier ||
               (name.size() > qualifier.size() && name.ends_with(qualifier) &&
                   name[name.size() - qualifier.size() - 1] == '.');
    }

    static void require_depth(std::size_t depth) {
        if (depth > 256) {
            throw std::length_error("database expression nesting exceeds 256 levels");
        }
    }

    void rewrite_subquery_node(db_query_storage& query_storage, std::pmr::vector<bool>& visited,
        bool source_bound, bool target_bound, std::size_t node, std::size_t node_depth) {
        if (node == no_db_node) {
            return;
        }
        require_depth(node_depth);
        if (visited[node]) {
            return;
        }
        visited[node] = true;
        auto& sub_node = query_storage.nodes_[node];
        if (!source_bound && !sub_node.explicit_alias_ &&
            (sub_node.kind_ == db_node_kind::column || sub_node.kind_ == db_node_kind::star) &&
            sub_node.qualifier_ == source_qualifier_) {
            if (target_bound) {
                throw std::invalid_argument("expression qualifier mapping would capture a correlated reference");
            }
            sub_node.qualifier_ = target_qualifier_;
        }
        rewrite_subquery_node(query_storage, visited, source_bound, target_bound, sub_node.left_, node_depth + 1);
        rewrite_subquery_node(query_storage, visited, source_bound, target_bound, sub_node.right_, node_depth + 1);
        for (const auto arg : sub_node.args_) {
            rewrite_subquery_node(query_storage, visited, source_bound, target_bound, arg, node_depth + 1);
        }
        for (const auto& arg : sub_node.named_) {
            rewrite_subquery_node(
                query_storage, visited, source_bound, target_bound, arg.expression_, node_depth + 1);
        }
        for (const auto& order : sub_node.orders_) {
            rewrite_subquery_node(
                query_storage, visited, source_bound, target_bound, order.expression_, node_depth + 1);
        }
        if (sub_node.query_ != no_db_node) {
            rewrite_subquery(
                query_storage.queries_[sub_node.query_].storage(), source_bound, target_bound, node_depth + 1);
        }
    }

    void rewrite_subquery_source(db_query_storage& query_storage, std::pmr::vector<bool>& visited,
        bool outer_source_bound, bool outer_target_bound, bool source_bound, bool target_bound,
        const db_query_source& src, std::size_t subquery_depth) {
        rewrite_subquery_node(
            query_storage, visited, source_bound, target_bound, src.expression_, subquery_depth + 1);
        if (src.query_ != no_db_node) {
            rewrite_subquery(query_storage.queries_[src.query_].storage(),
                src.lateral_ ? source_bound : outer_source_bound,
                src.lateral_ ? target_bound : outer_target_bound, subquery_depth + 1);
        }
    }

    void rewrite_subquery(db_query_storage& query_storage, bool outer_source_bound, bool outer_target_bound,
        std::size_t subquery_depth) {
        require_depth(subquery_depth);
        bool source_bound = outer_source_bound;
        bool target_bound = outer_target_bound;
        if (query_storage.source_) {
            source_bound = source_bound || binds_qualifier(*query_storage.source_, source_qualifier_);
            target_bound = target_bound || binds_qualifier(*query_storage.source_, target_qualifier_);
        }
        for (const auto& join : query_storage.joins_) {
            source_bound = source_bound || binds_qualifier(join.source_, source_qualifier_);
            target_bound = target_bound || binds_qualifier(join.source_, target_qualifier_);
        }
        // Only reachable expressions participate: abandoned builder nodes cannot
        // introduce a capture or an otherwise invisible nested query.
        std::pmr::vector<bool> visited(query_storage.nodes_.size(), false, target_.resource_);
        for (const auto node : query_storage.projections_) {
            rewrite_subquery_node(query_storage, visited, source_bound, target_bound, node, subquery_depth + 1);
        }
        for (const auto node : query_storage.distinct_on_) {
            rewrite_subquery_node(query_storage, visited, source_bound, target_bound, node, subquery_depth + 1);
        }
        for (const auto node : query_storage.groups_) {
            rewrite_subquery_node(query_storage, visited, source_bound, target_bound, node, subquery_depth + 1);
        }
        for (const auto& order : query_storage.orders_) {
            rewrite_subquery_node(
                query_storage, visited, source_bound, target_bound, order.expression_, subquery_depth + 1);
        }
        rewrite_subquery_node(
            query_storage, visited, source_bound, target_bound, query_storage.predicate_, subquery_depth + 1);
        rewrite_subquery_node(
            query_storage, visited, source_bound, target_bound, query_storage.having_, subquery_depth + 1);
        for (const auto& row : query_storage.rows_) {
            for (const auto node : row) {
                rewrite_subquery_node(query_storage, visited, source_bound, target_bound, node, subquery_depth + 1);
            }
        }
        if (query_storage.source_) {
            rewrite_subquery_source(query_storage, visited, outer_source_bound, outer_target_bound, source_bound,
                target_bound, *query_storage.source_, subquery_depth);
        }
        for (const auto& join : query_storage.joins_) {
            rewrite_subquery_source(query_storage, visited, outer_source_bound, outer_target_bound, source_bound,
                target_bound, join.source_, subquery_depth);
            rewrite_subquery_node(query_storage, visited, source_bound, target_bound, join.on_, subquery_depth + 1);
        }
        // CTE bodies and set operands are siblings of this query's body:
        // its FROM bindings do not shadow their correlated references.
        for (const auto& cte : query_storage.ctes_) {
            rewrite_subquery(query_storage.queries_[cte.query_].storage(), outer_source_bound, outer_target_bound,
                subquery_depth + 1);
        }
        for (const auto& operation : query_storage.set_operations_) {
            rewrite_subquery(query_storage.queries_[operation.query_].storage(), outer_source_bound,
                outer_target_bound, subquery_depth + 1);
        }
    }

    [[nodiscard]] std::size_t import_node(std::size_t id, std::size_t depth) {
        require_depth(depth);
        if (mapped_[id] != no_db_node) {
            return mapped_[id];
        }
        db_query_node node_value(source_nodes_[id], target_.resource_);
        if (!source_qualifier_.empty() && !node_value.explicit_alias_ &&
            (node_value.kind_ == db_node_kind::column || node_value.kind_ == db_node_kind::star) &&
            node_value.qualifier_ == source_qualifier_) {
            node_value.qualifier_ = target_qualifier_;
        }
        if (node_value.left_ != no_db_node) {
            node_value.left_ = import_node(node_value.left_, depth + 1);
        }
        if (node_value.right_ != no_db_node) {
            node_value.right_ = import_node(node_value.right_, depth + 1);
        }
        for (auto& arg : node_value.args_) {
            arg = import_node(arg, depth + 1);
        }
        for (auto& arg : node_value.named_) {
            arg.expression_ = import_node(arg.expression_, depth + 1);
        }
        for (auto& order : node_value.orders_) {
            order.expression_ = import_node(order.expression_, depth + 1);
        }
        if (node_value.query_ != no_db_node) {
            auto snapshot = source_queries_[node_value.query_].clone(target_.resource_);
            if (!source_qualifier_.empty()) {
                rewrite_subquery(snapshot.storage(), false, false, depth + 1);
            }
            node_value.query_ = target_.queries_.size();
            target_.queries_.push_back(std::move(snapshot));
        }
        const auto index = target_.nodes_.size();
        target_.nodes_.push_back(std::move(node_value));
        mapped_[id] = index;
        return index;
    }
};

}  // namespace ruvia::detail

namespace ruvia {

using detail::db_node_kind;
using detail::db_query_kind;
using detail::db_source_kind;
using detail::no_db_node;

namespace detail {

struct db_expr_tree final {
    explicit db_expr_tree(std::pmr::memory_resource* resource)
        : resource_(pmr_resource_or_default(resource)),
          nodes_(resource_),
          queries_(resource_) {}

    db_expr_tree(const db_expr_tree& other, std::pmr::memory_resource* resource)
        : resource_(pmr_resource_or_default(resource)),
          nodes_(resource_),
          queries_(resource_),
          root_(other.root_) {
        nodes_.reserve(other.nodes_.size());
        for (const auto& node : other.nodes_) {
            nodes_.emplace_back(node, resource_);
        }
        queries_.reserve(other.queries_.size());
        for (const auto& query : other.queries_) {
            queries_.push_back(query.clone(resource_));
        }
    }

    std::pmr::memory_resource* resource_;
    std::pmr::vector<db_query_node> nodes_;
    std::pmr::vector<db_query> queries_;
    std::size_t root_{no_db_node};
};

void db_expr_tree_deleter::operator()(db_expr_tree* tree) const noexcept {
    if (tree != nullptr) {
        std::destroy_at(tree);
        resource_->deallocate(tree, sizeof(db_expr_tree), alignof(db_expr_tree));
    }
}
namespace {

[[nodiscard]] std::size_t push_tree_node(db_expr_tree& tree, db_query_node&& node) {
    tree.nodes_.push_back(std::move(node));
    return tree.nodes_.size() - 1;
}

[[nodiscard]] std::size_t push_tree_value(db_expr_tree& tree, const db_value& value) {
    db_query_node node_value(db_node_kind::value, tree.resource_);
    auto owned = clone_db_value_for_resource(value, tree.resource_);
    std::destroy_at(&node_value.value_);
    std::construct_at(&node_value.value_, std::move(owned));
    return push_tree_node(tree, std::move(node_value));
}

[[nodiscard]] std::size_t push_tree_text_array(
    db_expr_tree& tree, std::span<const std::string_view> strings) {
    db_query_node array_node(db_node_kind::array, tree.resource_);
    array_node.args_.reserve(strings.size());
    for (const auto item : strings) {
        array_node.args_.push_back(push_tree_value(tree, db_value(item)));
    }
    const auto array_idx = push_tree_node(tree, std::move(array_node));
    db_query_node cast_node(db_node_kind::cast, tree.resource_);
    cast_node.left_ = array_idx;
    cast_node.type_ = db_stored_type(
        db_type_definition{.data_type_ = db_data_type::text, .array_ = true}, tree.resource_);
    return push_tree_node(tree, std::move(cast_node));
}

[[nodiscard]] std::size_t import_borrowed_node_to_tree(const db_query_storage& source,
    db_expr_tree& target, std::pmr::vector<std::size_t>& mapped, bool aliased, std::size_t id, std::size_t depth) {
    qualified_expression_importer::require_depth(depth);
    if (mapped[id] != no_db_node) {
        return mapped[id];
    }
    db_query_node node_value(source.nodes_[id], target.resource_);
    if (aliased && (node_value.kind_ == db_node_kind::column || node_value.kind_ == db_node_kind::star)) {
        node_value.explicit_alias_ = true;
    }
    if (node_value.left_ != no_db_node) {
        node_value.left_ = import_borrowed_node_to_tree(source, target, mapped, aliased, node_value.left_, depth + 1);
    }
    if (node_value.right_ != no_db_node) {
        node_value.right_ = import_borrowed_node_to_tree(source, target, mapped, aliased, node_value.right_, depth + 1);
    }
    for (auto& arg : node_value.args_) {
        arg = import_borrowed_node_to_tree(source, target, mapped, aliased, arg, depth + 1);
    }
    for (auto& arg : node_value.named_) {
        arg.expression_ = import_borrowed_node_to_tree(source, target, mapped, aliased, arg.expression_, depth + 1);
    }
    for (auto& order : node_value.orders_) {
        order.expression_ = import_borrowed_node_to_tree(source, target, mapped, aliased, order.expression_, depth + 1);
    }
    if (node_value.query_ != no_db_node) {
        node_value.query_ = target.queries_.size();
        target.queries_.push_back(source.queries_[source.nodes_[id].query_].clone(target.resource_));
    }
    const auto index = push_tree_node(target, std::move(node_value));
    mapped[id] = index;
    return index;
}

}  // namespace

db_owned_expression::db_owned_expression() noexcept = default;

db_owned_expression::db_owned_expression(const db_owned_expression& other)
    : kind_(other.kind_),
      aliased_(other.aliased_),
      borrowed_(other.borrowed_),
      text_(other.text_),
      table_(other.table_),
      scalar_(other.scalar_) {
    if (other.tree_) {
        auto* resource = other.tree_->resource_;
        void* block = resource->allocate(sizeof(db_expr_tree), alignof(db_expr_tree));
        try {
            tree_ = tree_owner_type(
                std::construct_at(static_cast<db_expr_tree*>(block), *other.tree_, resource),
                tree_deleter_type{resource});
        } catch (...) {
            resource->deallocate(block, sizeof(db_expr_tree), alignof(db_expr_tree));
            throw;
        }
    }
}

db_owned_expression& db_owned_expression::operator=(const db_owned_expression& other) {
    if (this != &other) {
        *this = db_owned_expression(other);
    }
    return *this;
}

db_owned_expression::db_owned_expression(db_owned_expression&& other) noexcept
    : kind_(std::exchange(other.kind_, leaf_kind::empty)),
      aliased_(std::exchange(other.aliased_, false)),
      borrowed_(std::exchange(other.borrowed_, db_expression{})),
      text_(std::exchange(other.text_, std::string_view{})),
      table_(std::exchange(other.table_, std::string_view{})),
      scalar_(other.scalar_),
      tree_(std::move(other.tree_)) {}

db_owned_expression& db_owned_expression::operator=(db_owned_expression&& other) noexcept {
    if (this != &other) {
        kind_ = std::exchange(other.kind_, leaf_kind::empty);
        aliased_ = std::exchange(other.aliased_, false);
        borrowed_ = std::exchange(other.borrowed_, db_expression{});
        text_ = std::exchange(other.text_, std::string_view{});
        table_ = std::exchange(other.table_, std::string_view{});
        scalar_ = other.scalar_;
        tree_ = std::move(other.tree_);
    }
    return *this;
}

db_owned_expression::~db_owned_expression() = default;

db_expression db_owned_expression::import_into(db_query& target, std::string_view table, std::string_view alias) const {
    switch (kind_) {
        case leaf_kind::empty:
            return {};
        case leaf_kind::borrowed: {
            const auto source_table = !table_.empty() ? table_ : table;
            if (!aliased_ && !source_table.empty() && !alias.empty()) {
                return target.import_expression(borrowed_, source_table, alias);
            }
            return target.import_expression(borrowed_);
        }
        case leaf_kind::null_value:
            return target.null_value();
        case leaf_kind::default_value:
            return target.default_value();
        case leaf_kind::star: {
            const auto qualifier =
                (!aliased_ && !table_.empty() && table_ == table && !alias.empty()) ? alias : table_;
            return target.star(qualifier);
        }
        case leaf_kind::column: {
            const auto qualifier =
                (!aliased_ && !table_.empty() && table_ == table && !alias.empty()) ? alias : table_;
            require_name(text_);
            if (!qualifier.empty()) {
                require_name(qualifier, true);
            }
            auto node_value = target.make_node(db_node_kind::column);
            node_value.text_ = text_;
            node_value.qualifier_ = qualifier;
            node_value.explicit_alias_ = aliased_;
            return target.push_node(std::move(node_value));
        }
        case leaf_kind::excluded:
            return target.excluded(text_);
        case leaf_kind::signed_value:
            return target.value(db_value(scalar_.signed_value_));
        case leaf_kind::unsigned_value:
            return target.value(db_value(scalar_.unsigned_value_));
        case leaf_kind::double_value:
            return target.value(db_value(scalar_.double_value_));
        case leaf_kind::bool_value:
            return target.value(db_value(scalar_.bool_value_));
        case leaf_kind::tree: {
            auto& target_storage = target.storage();
            const auto source_qualifier = alias.empty() ? std::string_view{} : table;
            const auto target_qualifier = alias.empty() ? std::string_view{} : alias;
            if (!source_qualifier.empty()) {
                require_name(source_qualifier, true);
                require_name(target_qualifier, true);
            }
            qualified_expression_importer importer(
                tree_->nodes_, tree_->queries_, target_storage, source_qualifier, target_qualifier);
            return db_expression(&target_storage, importer.import_node(tree_->root_, 0));
        }
    }
    return {};
}

db_owned_expression db_owned_expression::from_borrowed(
    db_expression expression, std::string_view table, bool aliased) noexcept {
    db_owned_expression result;
    if (!expression.empty()) {
        result.kind_ = leaf_kind::borrowed;
        result.borrowed_ = expression;
        result.table_ = table;
        result.aliased_ = aliased;
    }
    return result;
}

db_owned_expression::tree_owner_type db_owned_expression::allocate_tree(std::pmr::memory_resource* resource) {
    auto* resolved = pmr_resource_or_default(resource);
    void* block = resolved->allocate(sizeof(db_expr_tree), alignof(db_expr_tree));
    try {
        return tree_owner_type(
            std::construct_at(static_cast<db_expr_tree*>(block), resolved),
            tree_deleter_type{resolved});
    } catch (...) {
        resolved->deallocate(block, sizeof(db_expr_tree), alignof(db_expr_tree));
        throw;
    }
}

db_owned_expression db_owned_expression::from_tree(tree_owner_type tree) noexcept {
    db_owned_expression result;
    result.kind_ = leaf_kind::tree;
    result.tree_ = std::move(tree);
    return result;
}

std::size_t db_owned_expression::append_to_tree(db_expr_tree& target) const {
    switch (kind_) {
        case leaf_kind::empty:
            throw std::invalid_argument("cannot evaluate an empty database expression");
        case leaf_kind::borrowed: {
            if (borrowed_.empty() || borrowed_.node_ >= borrowed_.owner_->nodes_.size()) {
                throw std::invalid_argument("cannot import an empty database expression");
            }
            std::pmr::vector<std::size_t> mapped(borrowed_.owner_->nodes_.size(), no_db_node, target.resource_);
            return import_borrowed_node_to_tree(
                *borrowed_.owner_, target, mapped, aliased_, borrowed_.node_, 0);
        }
        case leaf_kind::null_value:
            return push_tree_value(target, db_value(nullptr));
        case leaf_kind::default_value:
            return push_tree_node(target, db_query_node(db_node_kind::default_value, target.resource_));
        case leaf_kind::star: {
            if (!table_.empty()) {
                require_name(table_, true);
            }
            db_query_node node_value(db_node_kind::star, target.resource_);
            node_value.qualifier_ = table_;
            node_value.explicit_alias_ = aliased_;
            return push_tree_node(target, std::move(node_value));
        }
        case leaf_kind::column: {
            require_name(text_);
            if (!table_.empty()) {
                require_name(table_, true);
            }
            db_query_node node_value(db_node_kind::column, target.resource_);
            node_value.text_ = text_;
            node_value.qualifier_ = table_;
            node_value.explicit_alias_ = aliased_;
            return push_tree_node(target, std::move(node_value));
        }
        case leaf_kind::excluded: {
            require_name(text_);
            db_query_node node_value(db_node_kind::excluded, target.resource_);
            node_value.text_ = text_;
            return push_tree_node(target, std::move(node_value));
        }
        case leaf_kind::signed_value:
            return push_tree_value(target, db_value(scalar_.signed_value_));
        case leaf_kind::unsigned_value:
            return push_tree_value(target, db_value(scalar_.unsigned_value_));
        case leaf_kind::double_value:
            return push_tree_value(target, db_value(scalar_.double_value_));
        case leaf_kind::bool_value:
            return push_tree_value(target, db_value(scalar_.bool_value_));
        case leaf_kind::tree: {
            const auto node_offset = target.nodes_.size();
            const auto query_offset = target.queries_.size();
            target.queries_.reserve(query_offset + tree_->queries_.size());
            for (const auto& query : tree_->queries_) {
                target.queries_.push_back(query.clone(target.resource_));
            }
            target.nodes_.reserve(node_offset + tree_->nodes_.size());
            for (const auto& node : tree_->nodes_) {
                db_query_node copy(node, target.resource_);
                if (copy.left_ != no_db_node) {
                    copy.left_ += node_offset;
                }
                if (copy.right_ != no_db_node) {
                    copy.right_ += node_offset;
                }
                for (auto& arg : copy.args_) {
                    arg += node_offset;
                }
                for (auto& named : copy.named_) {
                    named.expression_ += node_offset;
                }
                for (auto& order : copy.orders_) {
                    order.expression_ += node_offset;
                }
                if (copy.query_ != no_db_node) {
                    copy.query_ += query_offset;
                }
                target.nodes_.push_back(std::move(copy));
            }
            return tree_->root_ + node_offset;
        }
    }
    return no_db_node;
}

db_owned_expression db_owned_expression::from_expression(
    db_expression expression, std::string_view table, bool aliased) {
    if (expression.empty() || expression.node_ >= expression.owner_->nodes_.size()) {
        throw std::invalid_argument("cannot construct an owned expression from an empty expression");
    }
    auto tree = allocate_tree(expression.owner_->resource_);
    std::pmr::vector<std::size_t> mapped(expression.owner_->nodes_.size(), no_db_node, tree->resource_);
    tree->root_ = import_borrowed_node_to_tree(
        *expression.owner_, *tree, mapped, aliased, expression.node_, 0);
    auto result = from_tree(std::move(tree));
    result.table_ = table;
    result.aliased_ = aliased;
    return result;
}

db_owned_expression db_owned_expression::make_null() noexcept {
    db_owned_expression result;
    result.kind_ = leaf_kind::null_value;
    return result;
}

db_owned_expression db_owned_expression::make_default() noexcept {
    db_owned_expression result;
    result.kind_ = leaf_kind::default_value;
    return result;
}

db_owned_expression db_owned_expression::make_star(std::string_view table) {
    if (table.empty()) {
        db_owned_expression result;
        result.kind_ = leaf_kind::star;
        return result;
    }
    require_name(table, true);
    auto tree = allocate_tree();
    db_query_node node_value(db_node_kind::star, tree->resource_);
    node_value.qualifier_ = table;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_column(
    std::string_view name, std::string_view table, bool explicit_alias) {
    require_name(name);
    if (!table.empty()) {
        require_name(table, true);
    }
    auto tree = allocate_tree();
    db_query_node node_value(db_node_kind::column, tree->resource_);
    node_value.text_ = name;
    node_value.qualifier_ = table;
    node_value.explicit_alias_ = explicit_alias;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_static_column(
    std::string_view name, std::string_view table, std::string_view alias) {
    if (alias.empty()) {
        db_owned_expression result;
        result.kind_ = leaf_kind::column;
        result.text_ = name;
        result.table_ = table;
        result.aliased_ = false;
        return result;
    }
    return make_column(name, alias, true);
}

db_owned_expression db_owned_expression::make_excluded(std::string_view name) {
    require_name(name);
    auto tree = allocate_tree();
    db_query_node node_value(db_node_kind::excluded, tree->resource_);
    node_value.text_ = name;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_static_excluded(
    std::string_view name, std::string_view table) noexcept {
    db_owned_expression result;
    result.kind_ = leaf_kind::excluded;
    result.text_ = name;
    result.table_ = table;
    result.aliased_ = true;
    return result;
}

db_owned_expression db_owned_expression::make_value(const db_value& value) {
    db_owned_expression result;
    switch (db_value_access::type(value)) {
        case db_value_type::null:
            result.kind_ = leaf_kind::null_value;
            return result;
        case db_value_type::signed_value:
            result.kind_ = leaf_kind::signed_value;
            result.scalar_.signed_value_ = db_value_access::signed_value(value);
            return result;
        case db_value_type::unsigned_value:
            result.kind_ = leaf_kind::unsigned_value;
            result.scalar_.unsigned_value_ = db_value_access::unsigned_value(value);
            return result;
        case db_value_type::double_value:
            result.kind_ = leaf_kind::double_value;
            result.scalar_.double_value_ = db_value_access::get_double_value(value);
            return result;
        case db_value_type::bool_value:
            result.kind_ = leaf_kind::bool_value;
            result.scalar_.bool_value_ = db_value_access::get_bool_value(value);
            return result;
        case db_value_type::string: {
            auto tree = allocate_tree();
            tree->root_ = push_tree_value(*tree, value);
            return from_tree(std::move(tree));
        }
    }
    return result;
}

db_owned_expression db_owned_expression::make_raw_sql(
    std::span<const std::string_view> parts, std::span<const db_expr> args) {
    if (parts.size() != args.size() + 1 || (args.empty() && parts.front().empty())) {
        throw std::invalid_argument("SQL expression requires one more syntax part than arguments");
    }
    for (const auto part : parts) {
        if (part.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("SQL expression cannot contain NUL");
        }
    }
    auto tree = allocate_tree();
    db_query_node node_value(db_node_kind::sql, tree->resource_);
    for (std::size_t i = 0; i < args.size(); ++i) {
        node_value.named_.emplace_back(parts[i], args[i].owned().append_to_tree(*tree), tree->resource_);
    }
    node_value.text_ = parts.back();
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_call(
    std::string_view function, std::span<const db_expr> args) {
    require_name(function, true);
    auto tree = allocate_tree();
    db_query_node node_value(db_node_kind::function, tree->resource_);
    node_value.text_ = function;
    node_value.args_.reserve(args.size());
    for (const auto& arg : args) {
        node_value.args_.push_back(arg.owned().append_to_tree(*tree));
    }
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_coalesce(std::span<const db_expr> args) {
    if (args.empty()) {
        throw std::invalid_argument("COALESCE requires an argument");
    }
    auto tree = allocate_tree();
    db_query_node node_value(db_node_kind::coalesce, tree->resource_);
    node_value.args_.reserve(args.size());
    for (const auto& arg : args) {
        node_value.args_.push_back(arg.owned().append_to_tree(*tree));
    }
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_null_if(
    const db_owned_expression& lhs, const db_owned_expression& rhs) {
    auto tree = allocate_tree();
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto right_idx = rhs.append_to_tree(*tree);
    db_query_node node_value(db_node_kind::null_if, tree->resource_);
    node_value.args_.push_back(left_idx);
    node_value.args_.push_back(right_idx);
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_greatest(std::span<const db_expr> args) {
    if (args.empty()) {
        throw std::invalid_argument("GREATEST requires an argument");
    }
    auto tree = allocate_tree();
    db_query_node node_value(db_node_kind::greatest, tree->resource_);
    node_value.args_.reserve(args.size());
    for (const auto& arg : args) {
        node_value.args_.push_back(arg.owned().append_to_tree(*tree));
    }
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_least(std::span<const db_expr> args) {
    if (args.empty()) {
        throw std::invalid_argument("LEAST requires an argument");
    }
    auto tree = allocate_tree();
    db_query_node node_value(db_node_kind::least, tree->resource_);
    node_value.args_.reserve(args.size());
    for (const auto& arg : args) {
        node_value.args_.push_back(arg.owned().append_to_tree(*tree));
    }
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_case_when(
    std::span<const db_case_branch> branches, const db_owned_expression& otherwise) {
    if (branches.empty()) {
        throw std::invalid_argument("CASE requires a branch");
    }
    auto tree = allocate_tree();
    db_query_node node_value(db_node_kind::case_value, tree->resource_);
    node_value.args_.reserve(branches.size() * 2);
    for (const auto& branch : branches) {
        if (branch.when_.empty()) {
            throw std::invalid_argument("CASE WHEN requires a nonempty condition");
        }
        node_value.args_.push_back(branch.when_.owned().append_to_tree(*tree));
        node_value.args_.push_back(branch.then_.owned().append_to_tree(*tree));
    }
    if (!otherwise.empty()) {
        node_value.left_ = otherwise.append_to_tree(*tree);
    }
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_jsonb_set(const db_owned_expression& target,
    std::span<const std::string_view> path, const db_owned_expression& new_value, bool create_if_missing) {
    auto tree = allocate_tree();
    const auto target_idx = target.append_to_tree(*tree);
    const auto path_idx = push_tree_text_array(*tree, path);
    const auto val_idx = new_value.append_to_tree(*tree);
    const auto flag_idx = push_tree_value(*tree, db_value(create_if_missing));
    db_query_node node_value(db_node_kind::function, tree->resource_);
    node_value.text_ = "jsonb_set";
    node_value.args_ = {target_idx, path_idx, val_idx, flag_idx};
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_json_path_op(
    const db_owned_expression& target, db_binary_operator op, std::span<const std::string_view> path) {
    auto tree = allocate_tree();
    const auto target_idx = target.append_to_tree(*tree);
    const auto path_idx = push_tree_text_array(*tree, path);
    db_query_node node_value(db_node_kind::binary, tree->resource_);
    node_value.left_ = target_idx;
    node_value.right_ = path_idx;
    node_value.binary_ = op;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_aggregate(std::string_view function,
    std::span<const db_expr> args, bool distinct, std::span<const db_order_term> order) {
    require_name(function, true);
    auto tree = allocate_tree();
    db_query_node node_value(db_node_kind::aggregate, tree->resource_);
    node_value.text_ = function;
    node_value.flag_ = distinct;
    node_value.args_.reserve(args.size());
    for (const auto& arg : args) {
        node_value.args_.push_back(arg.owned().append_to_tree(*tree));
    }
    node_value.orders_.reserve(order.size());
    for (const auto& term : order) {
        node_value.orders_.push_back(
            {term.expression_.owned().append_to_tree(*tree), term.direction_, term.nulls_});
    }
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_count_star() {
    auto tree = allocate_tree();
    const auto star_idx = push_tree_node(*tree, db_query_node(db_node_kind::star, tree->resource_));
    db_query_node node_value(db_node_kind::aggregate, tree->resource_);
    node_value.text_ = "count";
    node_value.args_.push_back(star_idx);
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_filter(
    const db_owned_expression& aggregate, const db_owned_expression& predicate) {
    auto tree = allocate_tree();
    const auto agg_idx = aggregate.append_to_tree(*tree);
    const auto pred_idx = predicate.append_to_tree(*tree);
    db_query_node node_value(db_node_kind::filter, tree->resource_);
    node_value.left_ = agg_idx;
    node_value.right_ = pred_idx;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_over(
    const db_owned_expression& function, const db_window_options& window) {
    auto tree = allocate_tree();
    const auto fn_idx = function.append_to_tree(*tree);
    db_query_node node_value(db_node_kind::window, tree->resource_);
    node_value.left_ = fn_idx;
    node_value.args_.reserve(window.partition_by_.size());
    for (const auto& item : window.partition_by_) {
        node_value.args_.push_back(item.owned().append_to_tree(*tree));
    }
    node_value.orders_.reserve(window.order_by_.size());
    for (const auto& term : window.order_by_) {
        node_value.orders_.push_back(
            {term.expression_.owned().append_to_tree(*tree), term.direction_, term.nulls_});
    }
    node_value.frame_ = window.frame_;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_within_group(
    const db_owned_expression& function, std::span<const db_order_term> order) {
    if (order.empty()) {
        throw std::invalid_argument("WITHIN GROUP requires ordering");
    }
    auto tree = allocate_tree();
    const auto fn_idx = function.append_to_tree(*tree);
    db_query_node node_value(db_node_kind::within_group, tree->resource_);
    node_value.left_ = fn_idx;
    node_value.orders_.reserve(order.size());
    for (const auto& term : order) {
        node_value.orders_.push_back(
            {term.expression_.owned().append_to_tree(*tree), term.direction_, term.nulls_});
    }
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_cast(
    const db_owned_expression& operand, const db_type_definition& type) {
    auto tree = allocate_tree();
    const auto op_idx = operand.append_to_tree(*tree);
    db_query_node node_value(db_node_kind::cast, tree->resource_);
    node_value.left_ = op_idx;
    node_value.type_ = db_stored_type(type, tree->resource_);
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_at_time_zone(
    const db_owned_expression& operand, const db_owned_expression& zone) {
    auto tree = allocate_tree();
    const auto op_idx = operand.append_to_tree(*tree);
    const auto zone_idx = zone.append_to_tree(*tree);
    db_query_node node_value(db_node_kind::at_time_zone, tree->resource_);
    node_value.left_ = op_idx;
    node_value.right_ = zone_idx;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_extract(
    db_date_part part, const db_owned_expression& operand) {
    auto tree = allocate_tree();
    const auto op_idx = operand.append_to_tree(*tree);
    db_query_node node_value(db_node_kind::extract, tree->resource_);
    node_value.left_ = op_idx;
    node_value.date_part_ = part;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_subscript(
    const db_owned_expression& array, const db_owned_expression& index) {
    auto tree = allocate_tree();
    const auto arr_idx = array.append_to_tree(*tree);
    const auto idx_idx = index.append_to_tree(*tree);
    db_query_node node_value(db_node_kind::subscript, tree->resource_);
    node_value.left_ = arr_idx;
    node_value.right_ = idx_idx;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_collate(
    const db_owned_expression& operand, std::string_view collation) {
    require_name(collation, true);
    auto tree = allocate_tree();
    const auto op_idx = operand.append_to_tree(*tree);
    db_query_node node_value(db_node_kind::collate, tree->resource_);
    node_value.left_ = op_idx;
    node_value.text_ = collation;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_binary(
    const db_owned_expression& lhs, db_binary_operator op, const db_owned_expression& rhs) {
    auto tree = allocate_tree();
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto right_idx = rhs.append_to_tree(*tree);
    db_query_node node_value(db_node_kind::binary, tree->resource_);
    node_value.left_ = left_idx;
    node_value.right_ = right_idx;
    node_value.binary_ = op;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_binary(
    db_owned_expression&& lhs, db_binary_operator op, const db_owned_expression& rhs) {
    if (lhs.kind_ == leaf_kind::tree && lhs.tree_ != nullptr) {
        const auto left_idx = lhs.tree_->root_;
        const auto right_idx = rhs.append_to_tree(*lhs.tree_);
        db_query_node node_value(db_node_kind::binary, lhs.tree_->resource_);
        node_value.left_ = left_idx;
        node_value.right_ = right_idx;
        node_value.binary_ = op;
        lhs.tree_->root_ = push_tree_node(*lhs.tree_, std::move(node_value));
        return std::move(lhs);
    }
    return make_binary(std::as_const(lhs), op, rhs);
}

db_owned_expression db_owned_expression::make_unary(
    db_unary_operator op, const db_owned_expression& operand) {
    auto tree = allocate_tree();
    const auto op_idx = operand.append_to_tree(*tree);
    db_query_node node_value(db_node_kind::unary, tree->resource_);
    node_value.left_ = op_idx;
    node_value.unary_ = op;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_unary(
    db_unary_operator op, db_owned_expression&& operand) {
    if (operand.kind_ == leaf_kind::tree && operand.tree_ != nullptr) {
        const auto op_idx = operand.tree_->root_;
        db_query_node node_value(db_node_kind::unary, operand.tree_->resource_);
        node_value.left_ = op_idx;
        node_value.unary_ = op;
        operand.tree_->root_ = push_tree_node(*operand.tree_, std::move(node_value));
        return std::move(operand);
    }
    return make_unary(op, std::as_const(operand));
}

db_owned_expression db_owned_expression::make_between(const db_owned_expression& operand,
    const db_value& lower, const db_value& upper, bool negate) {
    auto tree = allocate_tree();
    const auto val_idx = operand.append_to_tree(*tree);
    const auto low_idx = push_tree_value(*tree, lower);
    const auto high_idx = push_tree_value(*tree, upper);
    db_query_node node_value(db_node_kind::between, tree->resource_);
    node_value.args_ = {val_idx, low_idx, high_idx};
    node_value.flag_ = negate;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_tuple(std::span<const db_expr> elements) {
    if (elements.empty()) {
        throw std::invalid_argument("database tuple requires at least one element");
    }
    auto tree = allocate_tree();
    db_query_node node_value(db_node_kind::tuple, tree->resource_);
    node_value.args_.reserve(elements.size());
    for (const auto& elem : elements) {
        node_value.args_.push_back(elem.owned().append_to_tree(*tree));
    }
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_in_list(
    const db_owned_expression& lhs, db_binary_operator op, std::span<const db_value> values) {
    auto tree = allocate_tree();
    const auto left_idx = lhs.append_to_tree(*tree);
    db_query_node list_node(db_node_kind::list, tree->resource_);
    list_node.args_.reserve(values.size());
    for (const auto& value : values) {
        list_node.args_.push_back(push_tree_value(*tree, value));
    }
    const auto list_idx = push_tree_node(*tree, std::move(list_node));
    db_query_node node_value(db_node_kind::binary, tree->resource_);
    node_value.left_ = left_idx;
    node_value.right_ = list_idx;
    node_value.binary_ = op;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_in_expr_list(
    const db_owned_expression& lhs, db_binary_operator op, std::span<const db_owned_expression> values) {
    auto tree = allocate_tree();
    const auto left_idx = lhs.append_to_tree(*tree);
    db_query_node list_node(db_node_kind::list, tree->resource_);
    list_node.args_.reserve(values.size());
    for (const auto& value : values) {
        list_node.args_.push_back(value.append_to_tree(*tree));
    }
    const auto list_idx = push_tree_node(*tree, std::move(list_node));
    db_query_node node_value(db_node_kind::binary, tree->resource_);
    node_value.left_ = left_idx;
    node_value.right_ = list_idx;
    node_value.binary_ = op;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_in_subquery(
    const db_owned_expression& lhs, db_binary_operator op, const db_query& subquery) {
    subquery.require_select_query();
    auto tree = allocate_tree();
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto query_idx = tree->queries_.size();
    tree->queries_.push_back(subquery.clone(tree->resource_));
    db_query_node sub_node(db_node_kind::subquery, tree->resource_);
    sub_node.query_ = query_idx;
    const auto sub_idx = push_tree_node(*tree, std::move(sub_node));
    db_query_node node_value(db_node_kind::binary, tree->resource_);
    node_value.left_ = left_idx;
    node_value.right_ = sub_idx;
    node_value.binary_ = op;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_exists(const db_query& subquery) {
    subquery.require_select_query();
    auto tree = allocate_tree();
    const auto query_idx = tree->queries_.size();
    tree->queries_.push_back(subquery.clone(tree->resource_));
    db_query_node node_value(db_node_kind::exists, tree->resource_);
    node_value.query_ = query_idx;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_array_op(const db_owned_expression& lhs,
    db_binary_operator op, std::span<const db_value> values, const db_type_definition& array_type) {
    auto tree = allocate_tree();
    const auto left_idx = lhs.append_to_tree(*tree);
    db_query_node array_node(db_node_kind::array, tree->resource_);
    array_node.args_.reserve(values.size());
    for (const auto& value : values) {
        array_node.args_.push_back(push_tree_value(*tree, value));
    }
    const auto array_idx = push_tree_node(*tree, std::move(array_node));
    db_query_node cast_node(db_node_kind::cast, tree->resource_);
    cast_node.left_ = array_idx;
    cast_node.type_ = db_stored_type(array_type, tree->resource_);
    const auto cast_idx = push_tree_node(*tree, std::move(cast_node));
    db_query_node node_value(db_node_kind::binary, tree->resource_);
    node_value.left_ = left_idx;
    node_value.right_ = cast_idx;
    node_value.binary_ = op;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_json_keys_op(
    const db_owned_expression& lhs, db_binary_operator op, std::span<const std::string_view> keys) {
    auto tree = allocate_tree();
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto keys_idx = push_tree_text_array(*tree, keys);
    db_query_node node_value(db_node_kind::binary, tree->resource_);
    node_value.left_ = left_idx;
    node_value.right_ = keys_idx;
    node_value.binary_ = op;
    tree->root_ = push_tree_node(*tree, std::move(node_value));
    return from_tree(std::move(tree));
}

}  // namespace detail

void db_query::storage_deleter_type::operator()(detail::db_query_storage* storage) const noexcept {
    if (storage != nullptr) {
        std::destroy_at(storage);
        resource_->deallocate(storage, sizeof(detail::db_query_storage), alignof(detail::db_query_storage));
    }
}

db_query::db_query(std::pmr::memory_resource* resource)
    : storage_(nullptr, storage_deleter_type{pmr_resource_or_default(resource)}) {
    auto* resolved = storage_.get_deleter().resource_;
    void* block = resolved->allocate(sizeof(detail::db_query_storage), alignof(detail::db_query_storage));
    try {
        storage_.reset(std::construct_at(static_cast<detail::db_query_storage*>(block), resolved));
    } catch (...) {
        resolved->deallocate(block, sizeof(detail::db_query_storage), alignof(detail::db_query_storage));
        throw;
    }
}
db_query::db_query(storage_owner_type storage) noexcept
    : storage_(std::move(storage)) {}
db_query::db_query(db_query&&) noexcept = default;
db_query& db_query::operator=(db_query&&) noexcept = default;
db_query::~db_query() = default;

detail::db_query_storage& db_query::storage() {
    if (!storage_) {
        throw std::logic_error("database query was moved from");
    }
    return *storage_;
}
const detail::db_query_storage& db_query::storage() const {
    if (!storage_) {
        throw std::logic_error("database query was moved from");
    }
    return *storage_;
}
std::pmr::memory_resource* db_query::resource() const {
    return storage().resource_;
}
bool db_query::returns_rows() const {
    return storage().returns_rows();
}
void db_query::require_select_query() const {
    if (storage().kind_ != db_query_kind::select && storage().kind_ != db_query_kind::values) {
        throw std::invalid_argument("subqueries require SELECT or VALUES; use a CTE for DML RETURNING");
    }
    for (const auto& cte : storage().ctes_) {
        storage().queries_.at(cte.query_).require_select_query();
    }
}
bool db_query::has_writes() const {
    const auto& s = storage();
    return (s.kind_ != db_query_kind::select && s.kind_ != db_query_kind::values) ||
           std::ranges::any_of(s.queries_, [](const auto& query) { return query.has_writes(); });
}
bool db_query::has_where() const {
    return storage().predicate_ != no_db_node;
}
bool db_query::has_grouping() const {
    return !storage().groups_.empty() || storage().having_ != no_db_node;
}
std::size_t db_query::require_expression(expr_type expression) const {
    if (expression.owner_ != storage_.get() || expression.node_ >= storage().nodes_.size()) {
        throw std::invalid_argument("database expression belongs to another query; import it first");
    }
    return expression.node_;
}

db_query::storage_owner_type db_query::copy_storage(const detail::db_query_storage& source_value, std::pmr::memory_resource* resource) {
    db_query copy(resource);
    auto& target = copy.storage();
    target.cache_enabled_ = source_value.cache_enabled_;
    target.cache_duration_ = source_value.cache_duration_;
    target.cache_id_ = source_value.cache_id_;
    target.cache_shape_ = source_value.cache_shape_;
    target.kind_ = source_value.kind_;
    target.target_ = source_value.target_;
    target.target_alias_ = source_value.target_alias_;
    target.columns_ = source_value.columns_;
    target.projections_ = source_value.projections_;
    target.groups_ = source_value.groups_;
    target.orders_ = source_value.orders_;
    target.distinct_ = source_value.distinct_;
    target.distinct_on_ = source_value.distinct_on_;
    target.returning_ = source_value.returning_;
    target.rows_ = source_value.rows_;
    target.insert_query_ = source_value.insert_query_;
    target.predicate_ = source_value.predicate_;
    target.having_ = source_value.having_;
    target.limit_ = source_value.limit_;
    target.offset_ = source_value.offset_;
    target.set_operations_ = source_value.set_operations_;
    target.nodes_.reserve(source_value.nodes_.size());
    for (const auto& node : source_value.nodes_) {
        target.nodes_.emplace_back(node, target.resource_);
    }
    target.queries_.reserve(source_value.queries_.size());
    for (const auto& query : source_value.queries_) {
        target.queries_.push_back(query.clone(target.resource_));
    }
    if (source_value.source_) {
        target.source_.emplace(*source_value.source_, target.resource_);
    }
    for (const auto& join : source_value.joins_) {
        target.joins_.emplace_back(join, target.resource_);
    }
    for (const auto& assignment : source_value.assignments_) {
        target.assignments_.emplace_back(assignment.column_, assignment.expression_, target.resource_);
    }
    for (const auto& cte : source_value.ctes_) {
        target.ctes_.emplace_back(cte, target.resource_);
    }
    if (source_value.conflict_) {
        target.conflict_.emplace(*source_value.conflict_, target.resource_);
    }
    if (source_value.lock_) {
        target.lock_.emplace(*source_value.lock_, target.resource_);
    }
    return std::move(copy.storage_);
}
db_query db_query::clone(std::pmr::memory_resource* resource) const {
    return db_query(copy_storage(storage(), pmr_resource_or_default(resource)));
}

bool db_query::cacheable() const {
    const auto& s = storage();
    return (s.kind_ == db_query_kind::select || s.kind_ == db_query_kind::values) && !s.lock_ &&
           std::ranges::all_of(s.queries_, [](const auto& query) { return query.cacheable(); });
}
std::optional<bool> db_query::cache_enabled() const {
    return storage().cache_enabled_;
}
std::optional<std::chrono::milliseconds> db_query::cache_duration() const {
    return storage().cache_duration_;
}
std::string_view db_query::cache_id() const {
    return storage().cache_id_;
}
detail::db_cache_shape db_query::cache_shape() const {
    return storage().cache_shape_;
}
void db_query::copy_cache(const db_query& source_value, detail::db_cache_shape shape) {
    auto& s = storage();
    s.cache_enabled_ = source_value.cache_enabled();
    s.cache_duration_ = source_value.cache_duration();
    s.cache_id_ = source_value.cache_id();
    s.cache_shape_ = shape;
}
db_query& db_query::cache(const db_cache_setting_type& setting) {
    return std::visit([&](const auto& value) -> db_query& {
        using t_type = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<t_type, db_cache_options>) {
            return cache(std::string_view(value.id_), value.milliseconds_);
        } else if constexpr (std::same_as<t_type, std::chrono::milliseconds>) {
            return cache(std::string_view{}, value);
        } else {
            auto& s = storage();
            s.cache_enabled_.reset();
            if constexpr (std::same_as<t_type, bool>) {
                s.cache_enabled_ = value;
            }
            s.cache_duration_.reset();
            s.cache_id_.clear();
            return *this;
        }
    },
        setting);
}
db_query& db_query::cache(std::string_view id, std::optional<std::chrono::milliseconds> milliseconds) {
    if (milliseconds && milliseconds->count() <= 0) {
        throw std::invalid_argument("cache duration must be positive");
    }
    auto& s = storage();
    s.cache_id_ = id;
    s.cache_duration_ = milliseconds;
    s.cache_enabled_ = true;
    return *this;
}

detail::db_query_node db_query::make_node(db_node_kind kind, std::span<const expr_type> args) const {
    detail::db_query_node node_value(kind, resource());
    for (const auto arg : args) {
        node_value.args_.push_back(require_expression(arg));
    }
    return node_value;
}
db_query::expr_type db_query::push_node(detail::db_query_node&& node_value) {
    auto& s = storage();
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}

db_query::expr_type db_query::column(std::string_view name, std::string_view table_value) {
    detail::require_name(name);
    if (!table_value.empty()) {
        detail::require_name(table_value, true);
    }
    auto node_value = make_node(db_node_kind::column);
    node_value.text_ = name;
    node_value.qualifier_ = table_value;
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::star(std::string_view table_value) {
    if (!table_value.empty()) {
        detail::require_name(table_value, true);
    }
    auto node_value = make_node(db_node_kind::star);
    node_value.qualifier_ = table_value;
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::value(const db_value& value) {
    auto node_value = make_node(db_node_kind::value);
    auto owned = detail::clone_db_value_for_resource(value, resource());
    std::destroy_at(&node_value.value_);
    std::construct_at(&node_value.value_, std::move(owned));
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::null_value() {
    return value(db_value(nullptr));
}
db_query::expr_type db_query::excluded(std::string_view column) {
    detail::require_name(column);
    auto node_value = make_node(db_node_kind::excluded);
    node_value.text_ = column;
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::default_value() {
    return push_node(make_node(db_node_kind::default_value));
}
db_query::expr_type db_query::sql(std::span<const std::string_view> parts, std::span<const expr_type> args) {
    if (parts.size() != args.size() + 1 || (args.empty() && parts.front().empty())) {
        throw std::invalid_argument("SQL expression requires one more syntax part than arguments");
    }
    auto node_value = make_node(db_node_kind::sql);
    for (auto part : parts) {
        if (part.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("SQL expression cannot contain NUL");
        }
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        node_value.named_.emplace_back(parts[i], require_expression(args[i]), resource());
    }
    node_value.text_ = parts.back();
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::call(std::string_view function, std::span<const expr_type> args, std::span<const db_named_argument> named) {
    detail::require_name(function, true);
    auto node_value = make_node(db_node_kind::function, args);
    node_value.text_ = function;
    for (const auto& arg : named) {
        detail::require_name(arg.name_);
        if (std::ranges::any_of(node_value.named_, [&](const auto& existing) { return std::string_view(existing.name_) == arg.name_; })) {
            throw std::invalid_argument("duplicate named database function argument");
        }
        node_value.named_.emplace_back(arg.name_, require_expression(arg.value_), resource());
    }
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::coalesce(std::span<const expr_type> args) {
    if (args.empty()) {
        throw std::invalid_argument("COALESCE requires an argument");
    }
    return push_node(make_node(db_node_kind::coalesce, args));
}
db_query::expr_type db_query::null_if(expr_type value, expr_type other) {
    const std::array args{value, other};
    return push_node(make_node(db_node_kind::null_if, args));
}
db_query::expr_type db_query::greatest(std::span<const expr_type> args) {
    if (args.empty()) {
        throw std::invalid_argument("GREATEST requires an argument");
    }
    return push_node(make_node(db_node_kind::greatest, args));
}
db_query::expr_type db_query::least(std::span<const expr_type> args) {
    if (args.empty()) {
        throw std::invalid_argument("LEAST requires an argument");
    }
    return push_node(make_node(db_node_kind::least, args));
}
db_query::expr_type db_query::binary(expr_type lhs, db_binary_operator op, expr_type rhs) {
    auto node_value = make_node(db_node_kind::binary);
    node_value.left_ = require_expression(lhs);
    node_value.right_ = require_expression(rhs);
    node_value.binary_ = op;
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::unary(db_unary_operator op, expr_type value) {
    auto node_value = make_node(db_node_kind::unary);
    node_value.left_ = require_expression(value);
    node_value.unary_ = op;
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::between(expr_type value, expr_type lower, expr_type upper, bool negate) {
    const std::array args{value, lower, upper};
    auto node_value = make_node(db_node_kind::between, args);
    node_value.flag_ = negate;
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::tuple(std::span<const expr_type> values) {
    return push_node(make_node(db_node_kind::tuple, values));
}
db_query::expr_type db_query::list(std::span<const expr_type> values) {
    return push_node(make_node(db_node_kind::list, values));
}
db_query::expr_type db_query::array(std::span<const expr_type> values) {
    return push_node(make_node(db_node_kind::array, values));
}
db_query::expr_type db_query::any(expr_type array_value) {
    auto node_value = make_node(db_node_kind::any);
    node_value.left_ = require_expression(array_value);
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::all(expr_type array_value) {
    auto node_value = make_node(db_node_kind::all);
    node_value.left_ = require_expression(array_value);
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::cast(expr_type value, const db_type_definition& type) {
    auto node_value = make_node(db_node_kind::cast);
    node_value.left_ = require_expression(value);
    node_value.type_ = detail::db_stored_type(type, resource());
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::alias(expr_type value, std::string_view name) {
    detail::require_name(name);
    auto node_value = make_node(db_node_kind::alias);
    node_value.left_ = require_expression(value);
    node_value.text_ = name;
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::case_when(std::span<const db_case_branch> branches, expr_type otherwise) {
    if (branches.empty()) {
        throw std::invalid_argument("CASE requires a branch");
    }
    auto node_value = make_node(db_node_kind::case_value);
    for (const auto& branch : branches) {
        node_value.args_.push_back(require_expression(branch.when_.expression(*this)));
        node_value.args_.push_back(require_expression(branch.then_.expression(*this)));
    }
    if (!otherwise.empty()) {
        node_value.left_ = require_expression(otherwise);
    }
    return push_node(std::move(node_value));
}
std::size_t db_query::add_subquery(const db_query& query) {
    query.require_select_query();
    auto snapshot = query.clone(resource());
    auto& s = storage();
    s.queries_.push_back(std::move(snapshot));
    return s.queries_.size() - 1;
}
db_query::expr_type db_query::subquery(const db_query& query) {
    const auto query_index = add_subquery(query);
    auto node_value = make_node(db_node_kind::subquery);
    node_value.query_ = query_index;
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::exists(const db_query& query) {
    const auto query_index = add_subquery(query);
    auto node_value = make_node(db_node_kind::exists);
    node_value.query_ = query_index;
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::aggregate(std::string_view function, std::span<const expr_type> args, bool distinct, std::span<const db_order_term> order) {
    detail::require_name(function, true);
    auto node_value = make_node(db_node_kind::aggregate, args);
    node_value.text_ = function;
    node_value.flag_ = distinct;
    for (const auto& term : order) {
        node_value.orders_.push_back({require_expression(term.expression_.expression(*this)), term.direction_, term.nulls_});
    }
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::filter(expr_type aggregate, expr_type predicate) {
    auto node_value = make_node(db_node_kind::filter);
    node_value.left_ = require_expression(aggregate);
    node_value.right_ = require_expression(predicate);
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::over(expr_type function, const db_window_options& window) {
    const auto function_node = require_expression(function);
    auto node_value = make_node(db_node_kind::window);
    node_value.left_ = function_node;
    node_value.args_.reserve(window.partition_by_.size());
    for (const auto& item : window.partition_by_) {
        node_value.args_.push_back(require_expression(item.expression(*this)));
    }
    for (const auto& term : window.order_by_) {
        node_value.orders_.push_back({require_expression(term.expression_.expression(*this)), term.direction_, term.nulls_});
    }
    node_value.frame_ = window.frame_;
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::within_group(expr_type function, std::span<const db_order_term> order) {
    if (order.empty()) {
        throw std::invalid_argument("WITHIN GROUP requires ordering");
    }
    auto node_value = make_node(db_node_kind::within_group);
    node_value.left_ = require_expression(function);
    for (const auto& term : order) {
        node_value.orders_.push_back({require_expression(term.expression_.expression(*this)), term.direction_, term.nulls_});
    }
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::extract(db_date_part part, expr_type value) {
    auto node_value = make_node(db_node_kind::extract);
    node_value.left_ = require_expression(value);
    node_value.date_part_ = part;
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::subscript(expr_type array_value, expr_type index) {
    auto node_value = make_node(db_node_kind::subscript);
    node_value.left_ = require_expression(array_value);
    node_value.right_ = require_expression(index);
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::collate(expr_type value, std::string_view collation) {
    // Collation names may be schema-qualified (the dot separates components),
    // so they follow the same rule as every other qualified name.
    detail::require_name(collation, true);
    auto node_value = make_node(db_node_kind::collate);
    node_value.left_ = require_expression(value);
    node_value.text_ = collation;
    return push_node(std::move(node_value));
}
db_query::expr_type db_query::at_time_zone(expr_type value, expr_type zone) {
    auto node_value = make_node(db_node_kind::at_time_zone);
    node_value.left_ = require_expression(value);
    node_value.right_ = require_expression(zone);
    return push_node(std::move(node_value));
}

db_query::expr_type db_query::import_expression(expr_type expression, std::string_view source_qualifier, std::string_view target_qualifier) {
    if (expression.empty() || expression.node_ >= expression.owner_->nodes_.size()) {
        throw std::invalid_argument("cannot import an empty database expression");
    }
    auto& target = storage();
    if (source_qualifier.empty() != target_qualifier.empty()) {
        throw std::invalid_argument("expression qualifier mapping requires both source and target");
    }
    if (!source_qualifier.empty()) {
        detail::require_name(source_qualifier, true);
        detail::require_name(target_qualifier, true);
    }
    if (expression.owner_ == &target && source_qualifier.empty()) {
        return expression;
    }
    detail::qualified_expression_importer importer(
        *expression.owner_, target, source_qualifier, target_qualifier);
    return expr_type(&target, importer.import_node(expression.node_, 0));
}

db_query& db_query::select(std::span<const expr_type> projections) {
    auto& s = storage();
    std::pmr::vector<std::size_t> owned(s.resource_);
    for (const auto expression : projections) {
        owned.push_back(require_expression(expression));
    }
    s.projections_ = std::move(owned);
    return *this;
}
db_query& db_query::add_select(expr_type projection) {
    storage().projections_.push_back(require_expression(projection));
    return *this;
}
db_query& db_query::from(std::string_view table_value, std::string_view alias) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_name(alias);
    }
    auto& s = storage();
    detail::db_query_source source_value(s.resource_);
    source_value.name_ = table_value;
    source_value.alias_ = alias;
    s.source_ = std::move(source_value);
    return *this;
}
db_query& db_query::from(const db_query& query, std::string_view alias, const db_source_options& options) {
    detail::require_name(alias);
    query.require_select_query();
    auto snapshot = query.clone(resource());
    auto& s = storage();
    detail::db_query_source source_value(s.resource_);
    source_value.kind_ = db_source_kind::query;
    source_value.alias_ = alias;
    source_value.query_ = s.queries_.size();
    detail::set_source_options(source_value, options, s.resource_);
    s.queries_.push_back(std::move(snapshot));
    s.source_ = std::move(source_value);
    return *this;
}
db_query& db_query::from_function(expr_type function, std::string_view alias, const db_source_options& options) {
    detail::require_name(alias);
    const auto index = require_expression(function);
    auto& s = storage();
    detail::db_query_source source_value(s.resource_);
    source_value.kind_ = db_source_kind::function;
    source_value.expression_ = index;
    source_value.alias_ = alias;
    detail::set_source_options(source_value, options, s.resource_);
    s.source_ = std::move(source_value);
    return *this;
}
db_query& db_query::clear_source() {
    storage().source_.reset();
    return *this;
}
db_query& db_query::join(db_join_type type, std::string_view table_value, expr_type on, std::string_view alias, std::span<const std::string_view> using_columns) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_name(alias);
    }
    auto& s = storage();
    detail::db_query_source source_value(s.resource_);
    source_value.name_ = table_value;
    source_value.alias_ = alias;
    detail::db_stored_join join(type, std::move(source_value), on.empty() ? no_db_node : require_expression(on), s.resource_);
    for (const auto name : using_columns) {
        detail::require_name(name);
        join.using_columns_.emplace_back(name);
    }
    s.joins_.push_back(std::move(join));
    return *this;
}
db_query& db_query::join(db_join_type type, const db_query& query, expr_type on, std::string_view alias, const db_source_options& options) {
    detail::require_name(alias);
    const auto predicate = on.empty() ? no_db_node : require_expression(on);
    query.require_select_query();
    auto snapshot = query.clone(resource());
    auto& s = storage();
    detail::db_query_source source_value(s.resource_);
    source_value.kind_ = db_source_kind::query;
    source_value.query_ = s.queries_.size();
    source_value.alias_ = alias;
    detail::set_source_options(source_value, options, s.resource_);
    s.queries_.push_back(std::move(snapshot));
    s.joins_.emplace_back(type, std::move(source_value), predicate, s.resource_);
    return *this;
}
db_query& db_query::join_function(db_join_type type, expr_type function, expr_type on, std::string_view alias, const db_source_options& options) {
    detail::require_name(alias);
    const auto function_id = require_expression(function);
    const auto predicate = on.empty() ? no_db_node : require_expression(on);
    auto& s = storage();
    detail::db_query_source source_value(s.resource_);
    source_value.kind_ = db_source_kind::function;
    source_value.expression_ = function_id;
    source_value.alias_ = alias;
    detail::set_source_options(source_value, options, s.resource_);
    s.joins_.emplace_back(type, std::move(source_value), predicate, s.resource_);
    return *this;
}
db_query& db_query::where(expr_type predicate) {
    storage().predicate_ = require_expression(predicate);
    return *this;
}
db_query& db_query::and_where(expr_type predicate) {
    const auto previous = storage().predicate_;
    return where(previous == no_db_node ? predicate : binary(expr_type(storage_.get(), previous), db_binary_operator::and_value, predicate));
}
db_query& db_query::or_where(expr_type predicate) {
    const auto previous = storage().predicate_;
    return where(previous == no_db_node ? predicate : binary(expr_type(storage_.get(), previous), db_binary_operator::or_value, predicate));
}
db_query& db_query::group_by(std::span<const expr_type> expressions) {
    std::pmr::vector<std::size_t> groups(resource());
    for (const auto expression : expressions) {
        groups.push_back(require_expression(expression));
    }
    storage().groups_ = std::move(groups);
    return *this;
}
db_query& db_query::add_group_by(expr_type expression) {
    storage().groups_.push_back(require_expression(expression));
    return *this;
}
db_query& db_query::having(expr_type predicate) {
    storage().having_ = require_expression(predicate);
    return *this;
}
db_query& db_query::and_having(expr_type predicate) {
    const auto previous = storage().having_;
    return having(previous == no_db_node ? predicate : binary(expr_type(storage_.get(), previous), db_binary_operator::and_value, predicate));
}
db_query& db_query::or_having(expr_type predicate) {
    const auto previous = storage().having_;
    return having(previous == no_db_node ? predicate : binary(expr_type(storage_.get(), previous), db_binary_operator::or_value, predicate));
}
db_query& db_query::order_by(expr_type expression, db_order_direction direction, db_nulls_order nulls) {
    const auto index = require_expression(expression);
    auto& s = storage();
    s.orders_.clear();
    s.orders_.push_back({index, direction, nulls});
    return *this;
}
db_query& db_query::add_order_by(expr_type expression, db_order_direction direction, db_nulls_order nulls) {
    storage().orders_.push_back({require_expression(expression), direction, nulls});
    return *this;
}
db_query& db_query::clear_order() {
    storage().orders_.clear();
    return *this;
}
db_query& db_query::limit(std::optional<std::uint64_t> count) {
    if (count && *count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("database limit exceeds signed 64-bit range");
    }
    storage().limit_ = count;
    return *this;
}
db_query& db_query::offset(std::optional<std::uint64_t> count) {
    if (count && *count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("database offset exceeds signed 64-bit range");
    }
    storage().offset_ = count;
    return *this;
}
db_query& db_query::distinct(bool enabled) {
    auto& s = storage();
    s.distinct_ = enabled;
    s.distinct_on_.clear();
    return *this;
}
db_query& db_query::distinct_on(std::span<const expr_type> expressions) {
    if (expressions.empty()) {
        throw std::invalid_argument("DISTINCT ON requires an expression");
    }
    std::pmr::vector<std::size_t> columns(resource());
    for (const auto expression : expressions) {
        columns.push_back(require_expression(expression));
    }
    auto& s = storage();
    s.distinct_on_ = std::move(columns);
    s.distinct_ = false;
    return *this;
}
db_query& db_query::returning(std::span<const expr_type> expressions) {
    std::pmr::vector<std::size_t> columns(resource());
    for (const auto expression : expressions) {
        columns.push_back(require_expression(expression));
    }
    storage().returning_ = std::move(columns);
    return *this;
}
db_query& db_query::insert_into(std::string_view table_value, std::span<const std::string_view> columns, std::string_view alias) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_name(alias);
    }
    auto& s = storage();
    std::pmr::vector<std::pmr::string> names(s.resource_);
    for (const auto column : columns) {
        detail::require_name(column);
        if (std::ranges::any_of(names, [&](const auto& name) { return name == column; })) {
            throw std::invalid_argument("duplicate INSERT column");
        }
        names.emplace_back(column);
    }
    s.kind_ = db_query_kind::insert;
    s.target_ = table_value;
    s.target_alias_ = alias;
    s.columns_ = std::move(names);
    return *this;
}
db_query& db_query::values(std::span<const expr_type> row) {
    auto& s = storage();
    if (s.kind_ != db_query_kind::select && s.kind_ != db_query_kind::values && s.kind_ != db_query_kind::insert) {
        throw std::invalid_argument("VALUES requires an INSERT or a values query");
    }
    std::pmr::vector<std::size_t> values(s.resource_);
    for (const auto expression : row) {
        values.push_back(require_expression(expression));
    }
    if (s.kind_ != db_query_kind::insert) {
        s.kind_ = db_query_kind::values;
    }
    s.rows_.push_back(std::move(values));
    return *this;
}
db_query& db_query::insert_from(const db_query& query) {
    if (storage().kind_ != db_query_kind::insert) {
        throw std::invalid_argument("INSERT SELECT requires an INSERT target");
    }
    query.require_select_query();
    auto snapshot = query.clone(resource());
    auto& s = storage();
    const auto index = s.queries_.size();
    s.queries_.push_back(std::move(snapshot));
    s.insert_query_ = index;
    return *this;
}
db_query& db_query::on_conflict(const db_conflict_options& conflict) {
    auto& s = storage();
    if (s.kind_ != db_query_kind::insert) {
        throw std::invalid_argument("ON CONFLICT requires an INSERT");
    }
    detail::db_stored_conflict owned(s.resource_);
    for (const auto& name : conflict.columns_) {
        detail::require_name(name);
        owned.columns_.emplace_back(name);
    }
    if (!conflict.constraint_.empty()) {
        detail::require_name(conflict.constraint_);
        owned.constraint_ = conflict.constraint_;
    }
    owned.do_nothing_ = conflict.do_nothing_;
    owned.any_unique_key_ = conflict.any_unique_key_;
    if (!conflict.target_where_.empty()) {
        owned.target_where_ = require_expression(conflict.target_where_);
    }
    if (!conflict.update_where_.empty()) {
        owned.update_where_ = require_expression(conflict.update_where_);
    }
    for (const auto& assignment : conflict.update_) {
        detail::require_name(assignment.column_);
        if (std::ranges::any_of(owned.assignments_, [&](const auto& existing) { return std::string_view(existing.column_) == assignment.column_; })) {
            throw std::invalid_argument("duplicate conflict update column");
        }
        const auto expression = assignment.expression(*this);
        owned.assignments_.emplace_back(assignment.column_, require_expression(expression), s.resource_);
    }
    s.conflict_ = std::move(owned);
    return *this;
}
db_query& db_query::update(std::string_view table_value, std::string_view alias) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_name(alias);
    }
    auto& s = storage();
    s.kind_ = db_query_kind::update;
    s.target_ = table_value;
    s.target_alias_ = alias;
    return *this;
}
db_query& db_query::set(std::string_view column, expr_type value) {
    detail::require_name(column);
    const auto expression = require_expression(value);
    auto& s = storage();
    if (s.kind_ != db_query_kind::update) {
        throw std::invalid_argument("SET requires an UPDATE");
    }
    for (auto& existing : s.assignments_) {
        if (existing.column_ == column) {
            existing.expression_ = expression;
            return *this;
        }
    }
    s.assignments_.emplace_back(column, expression, s.resource_);
    return *this;
}
db_query& db_query::set(const db_assignment& assignment) {
    return set(assignment.column_, assignment.expression(*this));
}
db_query& db_query::set(std::span<const db_assignment> assignments) {
    for (const auto& assignment : assignments) {
        set(assignment);
    }
    return *this;
}
db_query& db_query::update_from(std::string_view table_value, std::string_view alias) {
    if (storage().kind_ != db_query_kind::update) {
        throw std::invalid_argument("UPDATE FROM requires an UPDATE");
    }
    return from(table_value, alias);
}
db_query& db_query::update_from(const db_query& query, std::string_view alias) {
    if (storage().kind_ != db_query_kind::update) {
        throw std::invalid_argument("UPDATE FROM requires an UPDATE");
    }
    return from(query, alias);
}
db_query& db_query::delete_from(std::string_view table_value, std::string_view alias) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_name(alias);
    }
    auto& s = storage();
    s.kind_ = db_query_kind::delete_value;
    s.target_ = table_value;
    s.target_alias_ = alias;
    return *this;
}
db_query& db_query::delete_using(std::string_view table_value, std::string_view alias) {
    if (storage().kind_ != db_query_kind::delete_value) {
        throw std::invalid_argument("DELETE USING requires a DELETE");
    }
    return from(table_value, alias);
}
db_query& db_query::delete_using(const db_query& query, std::string_view alias) {
    if (storage().kind_ != db_query_kind::delete_value) {
        throw std::invalid_argument("DELETE USING requires a DELETE");
    }
    return from(query, alias);
}
db_query& db_query::lock(const db_lock_options& options) {
    if (options.nowait_ && options.skip_locked_) {
        throw std::invalid_argument("NOWAIT and SKIP LOCKED are mutually exclusive");
    }
    for (const auto& table : options.tables_) {
        detail::require_name(table);
    }
    storage().lock_.emplace(options, resource());
    return *this;
}
db_query& db_query::clear_lock() {
    storage().lock_.reset();
    return *this;
}
db_query& db_query::with(std::string_view name, const db_query& query, const db_cte_options& options) {
    detail::require_name(name);
    auto& s = storage();
    if (std::ranges::any_of(s.ctes_, [&](const auto& existing) { return existing.name_ == name; })) {
        throw std::invalid_argument("duplicate CTE name");
    }
    for (const auto& column : options.columns_) {
        detail::require_name(column);
    }
    auto snapshot = query.clone(resource());
    const auto index = s.queries_.size();
    s.queries_.push_back(std::move(snapshot));
    s.ctes_.emplace_back(name, index, options, s.resource_);
    return *this;
}
db_query& db_query::combine(db_set_operation operation, const db_query& query) {
    auto& s = storage();
    if ((s.kind_ != db_query_kind::select && s.kind_ != db_query_kind::values) ||
        (query.storage().kind_ != db_query_kind::select && query.storage().kind_ != db_query_kind::values)) {
        throw std::invalid_argument("set operations require SELECT or VALUES queries");
    }
    auto snapshot = query.clone(resource());
    const auto index = s.queries_.size();
    s.queries_.push_back(std::move(snapshot));
    s.set_operations_.push_back({operation, index});
    return *this;
}

}  // namespace ruvia
