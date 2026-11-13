#include "ruvia/web/db/db_query.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "db_pipeline.h"

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
// FROM/JOIN/target aliases, CTE names and lock targets are emitted as one
// identifier but referenced through dot-splitting qualifiers, so a dotted
// name could never be referenced.
void require_unqualified_name(std::string_view name) {
    require_name(name);
    if (name.find('.') != std::string_view::npos) {
        throw std::invalid_argument("database alias, CTE or lock name must be one unqualified identifier");
    }
}
// Switching a query to INSERT, UPDATE or DELETE keeps only its WHERE and WITH
// clauses: a retained FROM would turn into UPDATE FROM / DELETE USING and
// silently widen the target, and the other clauses mean nothing there.
void require_statement_kind_change(const db_query_storage& s, db_query_kind next) {
    if (s.kind_ == next) {
        return;
    }
    if (s.source_ || !s.joins_.empty() || !s.projections_.empty() || !s.rows_.empty() ||
        !s.assignments_.empty() || s.insert_query_ != no_db_node || s.conflict_ || !s.groups_.empty() ||
        s.having_ != no_db_node || !s.orders_.empty() || s.limit_ || s.offset_ || s.lock_ ||
        !s.set_operations_.empty() || s.distinct_ || !s.distinct_on_.empty()) {
        throw std::invalid_argument("statement kind change requires a query without FROM/JOIN/SELECT/VALUES clauses");
    }
}
void set_source_options(db_query_source& source_value, const db_source_options& options) {
    source_value.lateral_ = options.lateral_;
    source_value.ordinality_ = options.with_ordinality_;
    source_value.columns_.reserve(options.columns_.size());
    for (const auto& column : options.columns_) {
        require_name(column.name_);
        source_value.columns_.emplace_back(column);
    }
}
void validate_db_sql_parts(std::span<const std::string_view> parts, std::size_t argument_count) {
    if (parts.size() != argument_count + 1 || (argument_count == 0 && parts.front().empty())) {
        throw std::invalid_argument("SQL expression requires one more syntax part than arguments");
    }
    for (const auto part : parts) {
        if (part.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("SQL expression cannot contain NUL");
        }
    }
}
void require_variadic_arguments(std::size_t count, std::string_view function) {
    if (count == 0) {
        throw std::invalid_argument(std::string(function) + " requires an argument");
    }
}
// Appending through reserve(size() + extra) would allocate exactly and move
// every element on each later append; keep geometric growth instead.
template <class vector_type>
void reserve_append(vector_type& values, std::size_t extra) {
    if (values.capacity() - values.size() >= extra) {
        return;
    }
    if (extra > values.max_size() - values.size()) {
        throw std::length_error("database query storage exceeds its maximum size");
    }
    const auto doubled = values.capacity() > values.max_size() / 2 ? values.max_size() : values.capacity() * 2;
    values.reserve((std::max)(values.size() + extra, doubled));
}
template <class column_range>
void validate_cte(const db_query_storage& s, std::string_view name, const column_range& columns) {
    require_unqualified_name(name);
    if (std::ranges::any_of(s.ctes_, [&](const auto& existing) { return existing.name_ == name; })) {
        throw std::invalid_argument("duplicate CTE name");
    }
    for (const auto& column : columns) {
        require_name(std::string_view(column));
    }
}
template <class column_range>
db_stored_cte prepare_cte(db_query_storage& s, std::string_view name, bool recursive,
    db_materialization materialization, const column_range& columns) {
    db_stored_cte prepared(name, s.queries_.size(), recursive, materialization, columns);
    reserve_append(s.queries_, 1);
    reserve_append(s.ctes_, 1);
    return prepared;
}
}  // namespace

struct node_index_map final {
    static constexpr std::size_t inline_capacity = 16;
    std::array<std::size_t, inline_capacity> inline_{};
    std::vector<std::size_t> heap_;

    node_index_map(std::size_t count) {
        if (count <= inline_capacity) {
            std::fill_n(inline_.data(), count, no_db_node);
        } else {
            heap_.assign(count, no_db_node);
        }
    }

    [[nodiscard]] std::size_t& operator[](std::size_t index) noexcept {
        return heap_.empty() ? inline_[index] : heap_[index];
    }
    [[nodiscard]] std::size_t operator[](std::size_t index) const noexcept {
        return heap_.empty() ? inline_[index] : heap_[index];
    }
};

struct node_visit_set final {
    static constexpr std::size_t inline_capacity = 64;
    std::uint64_t inline_{0};
    std::vector<bool> heap_;

    node_visit_set(std::size_t count) {
        if (count > inline_capacity) {
            heap_.assign(count, false);
        }
    }

    [[nodiscard]] bool mark(std::size_t index) noexcept {
        if (heap_.empty()) {
            const std::uint64_t bit = std::uint64_t{1} << index;
            if ((inline_ & bit) != 0) {
                return false;
            }
            inline_ |= bit;
            return true;
        }
        if (heap_[index]) {
            return false;
        }
        heap_[index] = true;
        return true;
    }
};

// Numbers the nodes and nested queries the current statement reaches, in
// storage order. A builder retains replaced clauses, retired sources and
// abandoned nodes; a copy carries only this graph. The walk is iterative, so
// it accepts any depth the builder accepted.
class reachable_query_graph final {
public:
    reachable_query_graph(const db_query_storage& s)
        : nodes_(s.nodes_.size()),
          queries_(s.queries_.size()) {
        mark_nodes(s.projections_);
        mark_nodes(s.distinct_on_);
        mark_nodes(s.groups_);
        mark_nodes(s.returning_);
        for (const auto& order : s.orders_) {
            mark_node(order.expression_);
        }
        for (const auto& row : s.rows_) {
            mark_nodes(row);
        }
        for (const auto& assignment : s.assignments_) {
            mark_node(assignment.expression_);
        }
        mark_node(s.predicate_);
        mark_node(s.having_);
        mark_query(s.insert_query_);
        if (s.source_) {
            mark_source(*s.source_);
        }
        for (const auto& join : s.joins_) {
            mark_source(join.source_);
            mark_node(join.on_);
        }
        for (const auto& cte : s.ctes_) {
            mark_query(cte.query_);
        }
        for (const auto& operation : s.set_operations_) {
            mark_query(operation.query_);
        }
        if (s.conflict_) {
            mark_node(s.conflict_->target_where_);
            mark_node(s.conflict_->update_where_);
            for (const auto& assignment : s.conflict_->assignments_) {
                mark_node(assignment.expression_);
            }
        }
        while (!pending_.empty()) {
            const auto& current = s.nodes_[pending_.back()];
            pending_.pop_back();
            mark_node(current.left_);
            mark_node(current.right_);
            mark_nodes(current.args_);
            for (const auto& argument : current.named_) {
                mark_node(argument.expression_);
            }
            for (const auto& order : current.orders_) {
                mark_node(order.expression_);
            }
            mark_query(current.query_);
        }
        node_count_ = number(nodes_, s.nodes_.size());
        query_count_ = number(queries_, s.queries_.size());
    }

    // The copy's index for a source index, or no_db_node if unreachable.
    [[nodiscard]] std::size_t node(std::size_t index) const noexcept {
        return index == no_db_node ? no_db_node : nodes_[index];
    }
    [[nodiscard]] std::size_t query(std::size_t index) const noexcept {
        return index == no_db_node ? no_db_node : queries_[index];
    }
    [[nodiscard]] std::size_t node_count() const noexcept {
        return node_count_;
    }
    [[nodiscard]] std::size_t query_count() const noexcept {
        return query_count_;
    }

private:
    static constexpr std::size_t reached = 0;

    void mark_node(std::size_t index) {
        if (index != no_db_node && nodes_[index] == no_db_node) {
            nodes_[index] = reached;
            pending_.push_back(index);
        }
    }
    template <class range_type>
    void mark_nodes(const range_type& indices) {
        for (const auto index : indices) {
            mark_node(index);
        }
    }
    void mark_query(std::size_t index) noexcept {
        if (index != no_db_node) {
            queries_[index] = reached;
        }
    }
    void mark_source(const db_query_source& source_value) {
        mark_node(source_value.expression_);
        mark_query(source_value.query_);
    }
    [[nodiscard]] static std::size_t number(node_index_map& map, std::size_t count) noexcept {
        std::size_t next = 0;
        for (std::size_t index = 0; index < count; ++index) {
            if (map[index] != no_db_node) {
                map[index] = next++;
            }
        }
        return next;
    }

    node_index_map nodes_;
    node_index_map queries_;
    std::vector<std::size_t> pending_;
    std::size_t node_count_{0};
    std::size_t query_count_{0};
};

struct qualified_expression_importer final {
    const std::vector<db_query_node>& source_nodes_;
    const std::vector<db_query>& source_queries_;
    std::vector<db_query_node>& target_nodes_;
    std::vector<db_query>& target_queries_;
    std::string_view source_qualifier_{};
    std::string_view target_qualifier_{};
    bool mark_aliased_{false};
    node_index_map mapped_;

    qualified_expression_importer(const std::vector<db_query_node>& source_nodes,
        const std::vector<db_query>& source_queries, db_query_storage& target,
        std::string_view source_qualifier, std::string_view target_qualifier)
        : source_nodes_(source_nodes),
          source_queries_(source_queries),
          target_nodes_(target.nodes_),
          target_queries_(target.queries_),
          source_qualifier_(source_qualifier),
          target_qualifier_(target_qualifier),
          mapped_(source_nodes.size()) {}

    qualified_expression_importer(const db_query_storage& source_value, db_query_storage& target,
        std::string_view source_qualifier, std::string_view target_qualifier)
        : qualified_expression_importer(
              source_value.nodes_, source_value.queries_, target, source_qualifier, target_qualifier) {}

    qualified_expression_importer(const db_query_storage& source_value,
        std::vector<db_query_node>& target_nodes, std::vector<db_query>& target_queries, bool mark_aliased)
        : source_nodes_(source_value.nodes_),
          source_queries_(source_value.queries_),
          target_nodes_(target_nodes),
          target_queries_(target_queries),
          mark_aliased_(mark_aliased),
          mapped_(source_value.nodes_.size()) {}
    static void require_depth(std::size_t depth) {
        if (depth > max_db_query_depth) {
            throw std::length_error("database expression nesting exceeds 256 levels");
        }
    }

    // Collects the not yet imported left spine of an AND/OR chain, root first.
    static void collect_logical_spine(const std::vector<db_query_node>& nodes, std::size_t id,
        node_index_map& mapped, std::vector<std::size_t>& spine) {
        spine.push_back(id);
        for (auto left = nodes[id].left_;
            continues_logical_chain(nodes, nodes[spine.back()], left) && mapped[left] == no_db_node;
            left = nodes[left].left_) {
            spine.push_back(left);
        }
    }

    void rewrite_subquery_node(db_query_storage& query_storage, node_visit_set& visited,
        bool source_bound, bool target_bound, std::size_t node, std::size_t node_depth) {
        // The left operand is visited last, iteratively: continuing an AND/OR
        // chain does not add a level.
        while (node != no_db_node) {
            require_depth(node_depth);
            if (!visited.mark(node)) {
                return;
            }
            auto& sub_node = query_storage.nodes_[node];
            if (!source_bound && !sub_node.explicit_alias_ &&
                (sub_node.kind_ == db_node_kind::column || sub_node.kind_ == db_node_kind::star) &&
                sub_node.qualifier_ == source_qualifier_) {
                if (target_bound) {
                    throw std::invalid_argument("expression qualifier mapping would capture a correlated reference");
                }
                sub_node.qualifier_ = target_qualifier_;
            }
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
            if (!continues_logical_chain(query_storage.nodes_, sub_node, sub_node.left_)) {
                ++node_depth;
            }
            node = sub_node.left_;
        }
    }

    void rewrite_subquery_source(db_query_storage& query_storage, node_visit_set& visited,
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
            source_bound = source_bound || source_binds_name(*query_storage.source_, source_qualifier_);
            target_bound = target_bound || source_binds_name(*query_storage.source_, target_qualifier_);
        }
        for (const auto& join : query_storage.joins_) {
            source_bound = source_bound || source_binds_name(join.source_, source_qualifier_);
            target_bound = target_bound || source_binds_name(join.source_, target_qualifier_);
        }
        // Only reachable expressions participate: abandoned builder nodes cannot
        // introduce a capture or an otherwise invisible nested query.
        node_visit_set visited(query_storage.nodes_.size());
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
        if (!continues_logical_chain(source_nodes_, source_nodes_[id], source_nodes_[id].left_)) {
            return import_one(id, no_db_node, depth);
        }
        std::vector<std::size_t> spine;
        collect_logical_spine(source_nodes_, id, mapped_, spine);
        auto imported = import_node(source_nodes_[spine.back()].left_, depth + 1);
        for (auto link = spine.rbegin(); link != spine.rend(); ++link) {
            imported = import_one(*link, imported, depth);
        }
        return imported;
    }

private:
    // Copies one node; `imported_left` is its already imported chain operand.
    [[nodiscard]] std::size_t import_one(std::size_t id, std::size_t imported_left, std::size_t depth) {
        db_query_node node_value(source_nodes_[id]);
        if (mark_aliased_ && (node_value.kind_ == db_node_kind::column || node_value.kind_ == db_node_kind::star)) {
            node_value.explicit_alias_ = true;
        }
        if (!source_qualifier_.empty() && !node_value.explicit_alias_ &&
            (node_value.kind_ == db_node_kind::column || node_value.kind_ == db_node_kind::star) &&
            node_value.qualifier_ == source_qualifier_) {
            node_value.qualifier_ = target_qualifier_;
        }
        if (imported_left != no_db_node) {
            node_value.left_ = imported_left;
        } else if (node_value.left_ != no_db_node) {
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
            auto snapshot = source_queries_[node_value.query_].clone();
            if (!source_qualifier_.empty()) {
                rewrite_subquery(snapshot.storage(), false, false, depth + 1);
            }
            node_value.query_ = target_queries_.size();
            target_queries_.push_back(std::move(snapshot));
        }
        const auto index = target_nodes_.size();
        target_nodes_.push_back(std::move(node_value));
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
    db_expr_tree() = default;

    db_expr_tree(const db_expr_tree& other)
        : table_(other.table_),
          root_(other.root_) {
        nodes_.reserve(other.nodes_.size());
        for (const auto& node : other.nodes_) {
            nodes_.emplace_back(node);
        }
        queries_.reserve(other.queries_.size());
        for (const auto& query : other.queries_) {
            queries_.push_back(query.clone());
        }
    }

    std::vector<db_query_node> nodes_;
    std::vector<db_query> queries_;
    std::string table_;
    std::size_t root_{no_db_node};
};

namespace {

// The caller's table names the FROM item an owned expression is imported
// for; the table stored with the expression stands in only when the caller
// names none. Qualifiers of any other table stay untouched, as for columns.
[[nodiscard]] std::string_view import_source_qualifier(std::string_view stored_table, std::string_view caller_table) noexcept {
    return caller_table.empty() ? stored_table : caller_table;
}

// Borrows either an owned expression's compact tree or a query's node arena.
// Import callbacks keep each route's ownership rules without temporary arrays.
struct expression_nodes final {
    const std::vector<db_query_node>& nodes_;

    template <class arena_type>
    explicit expression_nodes(arena_type& arena)
        : nodes_(arena.nodes_) {}

    [[nodiscard]] static std::size_t push(std::vector<db_query_node>& nodes, db_query_node&& node) {
        nodes.push_back(std::move(node));
        return nodes.size() - 1;
    }

    [[nodiscard]] db_query_node value(const db_value& value) const {
        return db_query_node(db_node_kind::value, clone_db_value_for_resource(value));
    }

    [[nodiscard]] db_query_node named(db_node_kind kind, std::string_view text = {},
        std::string_view qualifier = {}, bool explicit_alias = false) const {
        if (kind != db_node_kind::star) {
            require_name(text, kind == db_node_kind::collate);
        }
        if (!qualifier.empty()) {
            require_name(qualifier, true);
        }
        db_query_node node(kind);
        node.text_ = text;
        node.qualifier_ = qualifier;
        node.explicit_alias_ = explicit_alias;
        return node;
    }

    template <class range_type, class import_type>
    [[nodiscard]] db_query_node arguments(db_node_kind kind, const range_type& args, import_type import_argument) const {
        db_query_node node(kind);
        node.args_.reserve(args.size());
        for (const auto& arg : args) {
            node.args_.push_back(import_argument(arg));
        }
        return node;
    }

    template <class range_type, class import_type>
    [[nodiscard]] db_query_node variadic(db_node_kind kind, const range_type& args,
        std::string_view function, import_type import_argument) const {
        require_variadic_arguments(args.size(), function);
        return arguments(kind, args, import_argument);
    }

    template <class range_type, class import_type>
    [[nodiscard]] db_query_node sql(std::span<const std::string_view> parts,
        const range_type& args, import_type import_argument) const {
        validate_db_sql_parts(parts, args.size());
        db_query_node node(db_node_kind::sql);
        node.named_.reserve(args.size());
        for (std::size_t i = 0; i < args.size(); ++i) {
            node.named_.emplace_back(parts[i], import_argument(args[i]));
        }
        node.text_ = parts.back();
        return node;
    }

    template <class range_type, class import_type>
    [[nodiscard]] db_query_node call(db_node_kind kind, std::string_view function,
        const range_type& args, import_type import_argument) const {
        require_name(function, true);
        auto node = arguments(kind, args, import_argument);
        node.text_ = function;
        return node;
    }

    template <class import_type>
    void orders(db_query_node& node, std::span<const db_order_term> order, import_type import_argument) const {
        node.orders_.reserve(order.size());
        for (const auto& term : order) {
            node.orders_.push_back({import_argument(term.expression_), term.direction_, term.nulls_});
        }
    }

    template <class range_type, class import_type>
    [[nodiscard]] db_query_node aggregate(std::string_view function, const range_type& args,
        bool distinct, std::span<const db_order_term> order, import_type import_argument) const {
        auto node = call(db_node_kind::aggregate, function, args, import_argument);
        node.flag_ = distinct;
        orders(node, order, import_argument);
        return node;
    }

    [[nodiscard]] db_query_node operands(db_node_kind kind, std::size_t left,
        std::size_t right = no_db_node) const {
        db_query_node node(kind);
        node.left_ = left;
        node.right_ = right;
        return node;
    }

    [[nodiscard]] db_query_node unary(db_unary_operator op, std::size_t operand) const {
        auto node = operands(db_node_kind::unary, operand);
        node.unary_ = op;
        return node;
    }

    [[nodiscard]] db_query_node binary(std::size_t left, db_binary_operator op, std::size_t right) const {
        if (op == db_binary_operator::equal || op == db_binary_operator::not_equal) {
            const auto is_null = [&](std::size_t index) {
                const auto& node = nodes_[index];
                return node.kind_ == db_node_kind::value && node.value_.type() == db_value_type::null;
            };
            const bool null_left = is_null(left);
            if (null_left || is_null(right)) {
                return unary(op == db_binary_operator::equal ? db_unary_operator::is_null : db_unary_operator::is_not_null,
                    null_left ? right : left);
            }
        }
        auto node = operands(db_node_kind::binary, left, right);
        node.binary_ = op;
        return node;
    }

    [[nodiscard]] db_query_node cast(std::size_t operand, const db_type_definition& type) const {
        auto node = operands(db_node_kind::cast, operand);
        node.type_ = db_stored_type(type);
        return node;
    }

    [[nodiscard]] db_query_node extract(db_date_part part, std::size_t operand) const {
        auto node = operands(db_node_kind::extract, operand);
        node.date_part_ = part;
        return node;
    }

    [[nodiscard]] db_query_node collate(std::size_t operand, std::string_view collation) const {
        auto node = named(db_node_kind::collate, collation);
        node.left_ = operand;
        return node;
    }

    [[nodiscard]] db_query_node between(std::size_t operand, std::size_t lower,
        std::size_t upper, bool negate) const {
        const std::array args{operand, lower, upper};
        auto node = arguments(db_node_kind::between, args, [](auto index) { return index; });
        node.flag_ = negate;
        return node;
    }

    template <class import_type, class otherwise_type>
    [[nodiscard]] db_query_node case_when(std::span<const db_case_branch> branches,
        import_type import_argument, otherwise_type otherwise) const {
        if (branches.empty()) {
            throw std::invalid_argument("CASE requires a branch");
        }
        db_query_node node(db_node_kind::case_value);
        node.args_.reserve(branches.size() * 2U);
        for (const auto& branch : branches) {
            if (branch.when_.empty()) {
                throw std::invalid_argument("CASE WHEN requires a nonempty condition");
            }
            node.args_.push_back(import_argument(branch.when_));
            node.args_.push_back(import_argument(branch.then_));
        }
        node.left_ = otherwise();
        return node;
    }

    template <class import_type>
    [[nodiscard]] db_query_node over(std::size_t function, const db_window_options& window, import_type import_argument) const {
        auto node = arguments(db_node_kind::window, window.partition_by_, import_argument);
        node.left_ = function;
        orders(node, window.order_by_, import_argument);
        node.frame_ = window.frame_;
        return node;
    }

    template <class import_type>
    [[nodiscard]] db_query_node within_group(std::size_t function,
        std::span<const db_order_term> order, import_type import_argument) const {
        if (order.empty()) {
            throw std::invalid_argument("WITHIN GROUP requires ordering");
        }
        auto node = operands(db_node_kind::within_group, function);
        orders(node, order, import_argument);
        return node;
    }

    [[nodiscard]] db_query_node subquery(db_node_kind kind, std::size_t query) const {
        db_query_node node(kind);
        node.query_ = query;
        return node;
    }
};

[[nodiscard]] std::size_t push_tree_node(db_expr_tree& tree, db_query_node&& node) {
    return expression_nodes::push(tree.nodes_, std::move(node));
}

[[nodiscard]] std::size_t push_tree_value(db_expr_tree& tree, const db_value& value) {
    return push_tree_node(tree, expression_nodes(tree).value(value));
}

[[nodiscard]] std::size_t push_tree_text_array(
    db_expr_tree& tree, std::span<const std::string_view> strings) {
    const auto array_idx = push_tree_node(tree, expression_nodes(tree).arguments(db_node_kind::array, strings,
                                                    [&](std::string_view item) { return push_tree_value(tree, db_value(item)); }));
    return push_tree_node(tree, expression_nodes(tree).cast(array_idx,
                                    db_type_definition{.data_type_ = db_data_type::text, .array_ = true}));
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
        tree_ = std::make_unique<db_expr_tree>(*other.tree_);
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
            const auto source_table = import_source_qualifier(table_, table);
            if (!aliased_ && !source_table.empty() && !alias.empty()) {
                return target.import_expression(borrowed_, source_table, alias);
            }
            return target.import_expression(borrowed_);
        }
        case leaf_kind::null_value:
            return target.null_value();
        case leaf_kind::default_value:
            return target.default_value();
        case leaf_kind::star:
            return target.star();
        case leaf_kind::column: {
            const auto qualifier =
                (!aliased_ && !table_.empty() && table_ == table && !alias.empty()) ? alias : table_;
            return target.push_node(expression_nodes(target.storage()).named(db_node_kind::column, text_, qualifier, aliased_));
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
            const auto source_table = import_source_qualifier(tree_->table_, table);
            const bool rewrite = !aliased_ && !source_table.empty() && !alias.empty();
            const auto source_qualifier = rewrite ? source_table : std::string_view{};
            const auto target_qualifier = rewrite ? alias : std::string_view{};
            if (!source_qualifier.empty()) {
                require_name(source_qualifier, true);
                require_name(target_qualifier, true);
            }
            reserve_append(target_storage.nodes_, tree_->nodes_.size());
            reserve_append(target_storage.queries_, tree_->queries_.size());
            qualified_expression_importer importer(
                tree_->nodes_, tree_->queries_, target_storage, source_qualifier, target_qualifier);
            return db_expression(&target_storage, importer.import_node(tree_->root_, 0));
        }
    }
    return {};
}

db_owned_expression db_owned_expression::from_borrowed(
    db_expression expression, borrowed_text table, bool aliased) noexcept {
    db_owned_expression result;
    if (!expression.empty()) {
        result.kind_ = leaf_kind::borrowed;
        result.borrowed_ = expression;
        result.table_ = table.view();
        result.aliased_ = aliased;
    }
    return result;
}

db_owned_expression::tree_owner_type db_owned_expression::allocate_tree() {
    return std::make_unique<db_expr_tree>();
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
            if (!aliased_ && !table_.empty() && target.table_.empty()) {
                target.table_ = table_;
            }
            qualified_expression_importer importer(
                *borrowed_.owner_, target.nodes_, target.queries_, aliased_);
            return importer.import_node(borrowed_.node_, 0);
        }
        case leaf_kind::null_value:
            return push_tree_value(target, db_value(nullptr));
        case leaf_kind::default_value:
            return push_tree_node(target, db_query_node(db_node_kind::default_value));
        case leaf_kind::star:
            return push_tree_node(target, db_query_node(db_node_kind::star));
        case leaf_kind::column: {
            return push_tree_node(target, expression_nodes(target).named(db_node_kind::column, text_, table_, aliased_));
        }
        case leaf_kind::excluded: {
            return push_tree_node(target, expression_nodes(target).named(db_node_kind::excluded, text_));
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
            if (!aliased_ && !tree_->table_.empty() && target.table_.empty()) {
                target.table_ = tree_->table_;
            }
            const auto node_offset = target.nodes_.size();
            const auto query_offset = target.queries_.size();
            reserve_append(target.queries_, tree_->queries_.size());
            for (const auto& query : tree_->queries_) {
                target.queries_.push_back(query.clone());
            }
            reserve_append(target.nodes_, tree_->nodes_.size());
            for (const auto& node : tree_->nodes_) {
                db_query_node copy(node);
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
    if (!table.empty()) {
        require_name(table, true);
    }
    auto tree = allocate_tree();
    if (!aliased) {
        tree->table_ = table;
    }
    qualified_expression_importer importer(
        *expression.owner_, tree->nodes_, tree->queries_, aliased);
    tree->root_ = importer.import_node(expression.node_, 0);
    auto result = from_tree(std::move(tree));
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
    auto tree = allocate_tree();
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).named(db_node_kind::star, {}, table));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_column(
    std::string_view name, std::string_view table, bool explicit_alias) {
    auto tree = allocate_tree();
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).named(db_node_kind::column, name, table, explicit_alias));
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
    auto tree = allocate_tree();
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).named(db_node_kind::excluded, name));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_static_excluded(std::string_view name) noexcept {
    db_owned_expression result;
    result.kind_ = leaf_kind::excluded;
    result.text_ = name;
    return result;
}

db_owned_expression db_owned_expression::make_value(const db_value& value) {
    db_owned_expression result;
    switch (value.type()) {
        case db_value_type::null:
            result.kind_ = leaf_kind::null_value;
            return result;
        case db_value_type::signed_value:
            result.kind_ = leaf_kind::signed_value;
            result.scalar_.signed_value_ = value.signed_value();
            return result;
        case db_value_type::unsigned_value:
            result.kind_ = leaf_kind::unsigned_value;
            result.scalar_.unsigned_value_ = value.unsigned_value();
            return result;
        case db_value_type::double_value:
            result.kind_ = leaf_kind::double_value;
            result.scalar_.double_value_ = value.double_value();
            return result;
        case db_value_type::bool_value:
            result.kind_ = leaf_kind::bool_value;
            result.scalar_.bool_value_ = value.bool_value();
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
    auto tree = allocate_tree();
    tree->nodes_.reserve(args.size() + 1U);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).sql(parts, args,
                                            [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_call(
    std::string_view function, std::span<const db_expr> args) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(args.size() + 1U);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).call(db_node_kind::function, function, args,
                                            [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_variadic(
    db_node_kind kind, std::span<const db_expr> args, std::string_view function) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(args.size() + 1U);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).variadic(kind, args, function,
                                            [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_two_operand_node(
    db_node_kind kind, const db_owned_expression& lhs, const db_owned_expression& rhs) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(3U);
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto right_idx = rhs.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).operands(kind, left_idx, right_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_coalesce(std::span<const db_expr> args) {
    return make_variadic(db_node_kind::coalesce, args, "COALESCE");
}

db_owned_expression db_owned_expression::make_null_if(
    const db_owned_expression& lhs, const db_owned_expression& rhs) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(3U);
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto right_idx = rhs.append_to_tree(*tree);
    const std::array args{left_idx, right_idx};
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).arguments(db_node_kind::null_if, args,
                                            [](auto index) { return index; }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_greatest(std::span<const db_expr> args) {
    return make_variadic(db_node_kind::greatest, args, "GREATEST");
}

db_owned_expression db_owned_expression::make_least(std::span<const db_expr> args) {
    return make_variadic(db_node_kind::least, args, "LEAST");
}

db_owned_expression db_owned_expression::make_case_when(
    std::span<const db_case_branch> branches, const db_owned_expression& otherwise) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(branches.size() * 2U + 2U);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).case_when(branches, [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }, [&] { return otherwise.empty() ? no_db_node : otherwise.append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_jsonb_set(const db_owned_expression& target,
    std::span<const std::string_view> path, const db_owned_expression& new_value, bool create_if_missing) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(path.size() + 6U);
    const auto target_idx = target.append_to_tree(*tree);
    const auto path_idx = push_tree_text_array(*tree, path);
    const auto val_idx = new_value.append_to_tree(*tree);
    const auto flag_idx = push_tree_value(*tree, db_value(create_if_missing));
    const std::array args{target_idx, path_idx, val_idx, flag_idx};
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).call(db_node_kind::function, "jsonb_set", args,
                                            [](auto index) { return index; }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_json_path_op(
    const db_owned_expression& target, db_binary_operator op, std::span<const std::string_view> path) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(path.size() + 4U);
    const auto target_idx = target.append_to_tree(*tree);
    const auto path_idx = push_tree_text_array(*tree, path);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).binary(target_idx, op, path_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_aggregate(std::string_view function,
    std::span<const db_expr> args, bool distinct, std::span<const db_order_term> order) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(args.size() + order.size() + 1U);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).aggregate(function, args, distinct, order,
                                            [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_count_star() {
    auto tree = allocate_tree();
    tree->nodes_.reserve(2U);
    const auto star_idx = push_tree_node(*tree, db_query_node(db_node_kind::star));
    const std::array args{star_idx};
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).call(db_node_kind::aggregate, "count", args,
                                            [](auto index) { return index; }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_filter(
    const db_owned_expression& aggregate, const db_owned_expression& predicate) {
    return make_two_operand_node(db_node_kind::filter, aggregate, predicate);
}

db_owned_expression db_owned_expression::make_over(
    const db_owned_expression& function, const db_window_options& window) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(window.partition_by_.size() + window.order_by_.size() + 2U);
    const auto fn_idx = function.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).over(fn_idx, window,
                                            [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_within_group(
    const db_owned_expression& function, std::span<const db_order_term> order) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(order.size() + 2U);
    const auto fn_idx = function.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).within_group(fn_idx, order,
                                            [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_cast(
    const db_owned_expression& operand, const db_type_definition& type) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(2U);
    const auto op_idx = operand.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).cast(op_idx, type));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_at_time_zone(
    const db_owned_expression& operand, const db_owned_expression& zone) {
    return make_two_operand_node(db_node_kind::at_time_zone, operand, zone);
}

db_owned_expression db_owned_expression::make_extract(
    db_date_part part, const db_owned_expression& operand) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(2U);
    const auto op_idx = operand.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).extract(part, op_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_subscript(
    const db_owned_expression& array, const db_owned_expression& index) {
    return make_two_operand_node(db_node_kind::subscript, array, index);
}

db_owned_expression db_owned_expression::make_collate(
    const db_owned_expression& operand, std::string_view collation) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(2U);
    const auto op_idx = operand.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).collate(op_idx, collation));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_binary(
    const db_owned_expression& lhs, db_binary_operator op, const db_owned_expression& rhs) {
    const auto lhs_nodes = (lhs.kind_ == leaf_kind::tree && lhs.tree_ != nullptr) ? lhs.tree_->nodes_.size() : 1U;
    const auto rhs_nodes = (rhs.kind_ == leaf_kind::tree && rhs.tree_ != nullptr) ? rhs.tree_->nodes_.size() : 1U;
    auto tree = allocate_tree();
    tree->nodes_.reserve(lhs_nodes + rhs_nodes + 1U);
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto right_idx = rhs.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).binary(left_idx, op, right_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_binary(
    db_owned_expression&& lhs, db_binary_operator op, const db_owned_expression& rhs) {
    if (&lhs != &rhs && lhs.kind_ == leaf_kind::tree && lhs.tree_ != nullptr) {
        const auto rhs_nodes = (rhs.kind_ == leaf_kind::tree && rhs.tree_ != nullptr) ? rhs.tree_->nodes_.size() : 1U;
        reserve_append(lhs.tree_->nodes_, rhs_nodes + 1U);
        const auto left_idx = lhs.tree_->root_;
        const auto right_idx = rhs.append_to_tree(*lhs.tree_);
        lhs.tree_->root_ = push_tree_node(*lhs.tree_, expression_nodes(*lhs.tree_).binary(left_idx, op, right_idx));
        lhs.aliased_ = false;
        return std::move(lhs);
    }
    return make_binary(std::as_const(lhs), op, rhs);
}

db_owned_expression db_owned_expression::make_unary(
    db_unary_operator op, const db_owned_expression& operand) {
    const auto operand_nodes =
        (operand.kind_ == leaf_kind::tree && operand.tree_ != nullptr) ? operand.tree_->nodes_.size() : 1U;
    auto tree = allocate_tree();
    tree->nodes_.reserve(operand_nodes + 1U);
    const auto op_idx = operand.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).unary(op, op_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_unary(
    db_unary_operator op, db_owned_expression&& operand) {
    if (operand.kind_ == leaf_kind::tree && operand.tree_ != nullptr) {
        const auto op_idx = operand.tree_->root_;
        operand.tree_->root_ = push_tree_node(*operand.tree_, expression_nodes(*operand.tree_).unary(op, op_idx));
        operand.aliased_ = false;
        return std::move(operand);
    }
    return make_unary(op, std::as_const(operand));
}

db_owned_expression db_owned_expression::make_between(const db_owned_expression& operand,
    const db_value& lower, const db_value& upper, bool negate) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(4U);
    const auto val_idx = operand.append_to_tree(*tree);
    const auto low_idx = push_tree_value(*tree, lower);
    const auto high_idx = push_tree_value(*tree, upper);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).between(val_idx, low_idx, high_idx, negate));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_tuple(std::span<const db_expr> elements) {
    if (elements.empty()) {
        throw std::invalid_argument("database tuple requires at least one element");
    }
    auto tree = allocate_tree();
    tree->nodes_.reserve(elements.size() + 1U);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).arguments(db_node_kind::tuple, elements,
                                            [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_in_list(const db_owned_expression& lhs,
    db_binary_operator op, std::size_t count, const void* context,
    db_value (*element_at)(const void* context, std::size_t index)) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(count + 3U);
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto list_idx = push_tree_node(*tree, expression_nodes(*tree).arguments(db_node_kind::list,
                                                    std::views::iota(std::size_t{0}, count),
                                                    [&](std::size_t index) { return push_tree_value(*tree, element_at(context, index)); }));
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).binary(left_idx, op, list_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_in_expr_list(
    const db_owned_expression& lhs, db_binary_operator op, std::span<const db_tuple> values) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(values.size() + 3U);
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto list_idx = push_tree_node(*tree, expression_nodes(*tree).arguments(db_node_kind::list, values,
                                                    [&](const auto& value) { return value.owned_.append_to_tree(*tree); }));
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).binary(left_idx, op, list_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_in_subquery(
    const db_owned_expression& lhs, db_binary_operator op, const db_query& subquery) {
    subquery.require_select_query();
    auto tree = allocate_tree();
    tree->nodes_.reserve(3U);
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto query_idx = tree->queries_.size();
    tree->queries_.push_back(subquery.clone());
    const auto sub_idx = push_tree_node(*tree, expression_nodes(*tree).subquery(db_node_kind::subquery, query_idx));
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).binary(left_idx, op, sub_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_exists(const db_query& subquery) {
    subquery.require_select_query();
    auto tree = allocate_tree();
    const auto query_idx = tree->queries_.size();
    tree->queries_.push_back(subquery.clone());
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).subquery(db_node_kind::exists, query_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_array_op(const db_owned_expression& lhs,
    db_binary_operator op, std::size_t count, const void* context,
    db_value (*element_at)(const void* context, std::size_t index),
    const db_type_definition& array_type) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(count + 4U);
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto array_idx = push_tree_node(*tree, expression_nodes(*tree).arguments(db_node_kind::array,
                                                     std::views::iota(std::size_t{0}, count),
                                                     [&](std::size_t index) { return push_tree_value(*tree, element_at(context, index)); }));
    const auto cast_idx = push_tree_node(*tree, expression_nodes(*tree).cast(array_idx, array_type));
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).binary(left_idx, op, cast_idx));
    return from_tree(std::move(tree));
}

}  // namespace detail

void db_query::storage_deleter_type::operator()(detail::db_query_storage* storage) const noexcept {
    delete storage;
}

db_query::db_query()
    : storage_(new detail::db_query_storage(), storage_deleter_type{}) {}
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
bool db_query::returns_rows() const {
    return storage().returns_rows();
}
void db_query::require_select_query() const {
    if (has_writes()) {
        throw std::invalid_argument("subqueries require SELECT or VALUES; use a CTE for DML RETURNING");
    }
}
bool db_query::has_writes() const {
    const auto& s = storage();
    // Every source/expression subquery passes require_select_query(). Only
    // active CTEs and set operands can carry writes; queries_ also owns retired
    // sources and incomplete appends, which must not classify the current SQL.
    return (s.kind_ != db_query_kind::select && s.kind_ != db_query_kind::values) ||
           std::ranges::any_of(s.ctes_, [&](const auto& cte) { return s.queries_.at(cte.query_).has_writes(); }) ||
           std::ranges::any_of(s.set_operations_, [&](const auto& operand) { return s.queries_.at(operand.query_).has_writes(); });
}
bool db_query::has_where() const {
    return storage().predicate_ != no_db_node;
}
bool db_query::has_grouping() const {
    return !storage().groups_.empty() || storage().having_ != no_db_node;
}
std::size_t db_query::require_expression(db_expression expression) const {
    if (expression.owner_ != storage_.get() || expression.node_ >= storage().nodes_.size()) {
        throw std::invalid_argument("database expression belongs to another query; import it first");
    }
    return expression.node_;
}

detail::db_query_storage::db_query_storage(const db_query_storage& source)
    : nodes_(source.nodes_),
      kind_(source.kind_),
      target_(source.target_),
      target_alias_(source.target_alias_),
      columns_(source.columns_),
      projections_(source.projections_),
      groups_(source.groups_),
      orders_(source.orders_),
      distinct_(source.distinct_),
      distinct_on_(source.distinct_on_),
      returning_(source.returning_),
      source_(source.source_),
      joins_(source.joins_),
      rows_(source.rows_),
      assignments_(source.assignments_),
      insert_query_(source.insert_query_),
      predicate_(source.predicate_),
      having_(source.having_),
      limit_(source.limit_),
      offset_(source.offset_),
      conflict_(source.conflict_),
      lock_(source.lock_),
      ctes_(source.ctes_),
      set_operations_(source.set_operations_),
      cache_enabled_(source.cache_enabled_),
      cache_duration_(source.cache_duration_),
      cache_id_(source.cache_id_),
      cache_shape_(source.cache_shape_) {
    queries_.reserve(source.queries_.size());
    for (const auto& query_value : source.queries_) {
        queries_.push_back(query_value.clone());
    }
}

db_query::storage_owner_type db_query::copy_storage(const detail::db_query_storage& source) {
    return storage_owner_type(new detail::db_query_storage(source), storage_deleter_type{});
}
db_query db_query::clone() const {
    return db_query(copy_storage(storage()));
}

bool db_query::cacheable() const {
    const auto& s = storage();
    if ((s.kind_ != db_query_kind::select && s.kind_ != db_query_kind::values) || s.lock_) {
        return false;
    }
    const detail::reachable_query_graph graph(s);
    for (std::size_t index = 0; index < s.queries_.size(); ++index) {
        if (graph.query(index) != no_db_node && !s.queries_[index].cacheable()) {
            return false;
        }
    }
    return true;
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
            return cache(std::string_view(value.id_), value.duration_);
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
db_query& db_query::cache(std::string_view id, std::optional<std::chrono::milliseconds> duration) {
    if (duration && duration->count() <= 0) {
        throw std::invalid_argument("cache duration must be positive");
    }
    auto& s = storage();
    s.cache_id_ = id;
    s.cache_duration_ = duration;
    s.cache_enabled_ = true;
    return *this;
}

detail::db_query_node db_query::make_node(db_node_kind kind, std::span<const db_expression> args) const {
    return detail::expression_nodes(storage()).arguments(kind, args,
        [&](db_expression arg) { return require_expression(arg); });
}
db_expression db_query::push_node(detail::db_query_node&& node_value) {
    auto& s = storage();
    return db_expression(&s, detail::expression_nodes::push(s.nodes_, std::move(node_value)));
}

db_expression db_query::column(std::string_view name, std::string_view table_value) {
    return push_node(detail::expression_nodes(storage()).named(db_node_kind::column, name, table_value));
}
db_expression db_query::star(std::string_view table_value) {
    return push_node(detail::expression_nodes(storage()).named(db_node_kind::star, {}, table_value));
}
db_expression db_query::value(const db_value& value) {
    return push_node(detail::expression_nodes(storage()).value(value));
}
db_expression db_query::null_value() {
    return value(db_value(nullptr));
}
db_expression db_query::excluded(std::string_view column) {
    return push_node(detail::expression_nodes(storage()).named(db_node_kind::excluded, column));
}
db_expression db_query::default_value() {
    return push_node(make_node(db_node_kind::default_value));
}
db_expression db_query::sql(std::span<const std::string_view> parts, std::span<const db_expression> args) {
    return push_node(detail::expression_nodes(storage()).sql(parts, args,
        [&](db_expression arg) { return require_expression(arg); }));
}
db_expression db_query::call(std::string_view function, std::span<const db_expression> args, std::span<const db_named_argument> named) {
    auto node_value = detail::expression_nodes(storage()).call(db_node_kind::function, function, args,
        [&](db_expression arg) { return require_expression(arg); });
    node_value.named_.reserve(named.size());
    for (const auto& arg : named) {
        detail::require_name(arg.name_);
        if (std::ranges::any_of(node_value.named_, [&](const auto& existing) { return std::string_view(existing.name_) == arg.name_; })) {
            throw std::invalid_argument("duplicate named database function argument");
        }
        node_value.named_.emplace_back(arg.name_, require_expression(arg.value_));
    }
    return push_node(std::move(node_value));
}
db_expression db_query::coalesce(std::span<const db_expression> args) {
    return push_node(detail::expression_nodes(storage()).variadic(db_node_kind::coalesce, args, "COALESCE",
        [&](db_expression arg) { return require_expression(arg); }));
}
db_expression db_query::null_if(db_expression value, db_expression other) {
    const std::array args{value, other};
    return push_node(make_node(db_node_kind::null_if, args));
}
db_expression db_query::greatest(std::span<const db_expression> args) {
    return push_node(detail::expression_nodes(storage()).variadic(db_node_kind::greatest, args, "GREATEST",
        [&](db_expression arg) { return require_expression(arg); }));
}
db_expression db_query::least(std::span<const db_expression> args) {
    return push_node(detail::expression_nodes(storage()).variadic(db_node_kind::least, args, "LEAST",
        [&](db_expression arg) { return require_expression(arg); }));
}
db_expression db_query::binary(db_expression lhs, db_binary_operator op, db_expression rhs) {
    const auto left = require_expression(lhs);
    const auto right = require_expression(rhs);
    return push_node(detail::expression_nodes(storage()).binary(left, op, right));
}
db_expression db_query::unary(db_unary_operator op, db_expression value) {
    return push_node(detail::expression_nodes(storage()).unary(op, require_expression(value)));
}
db_expression db_query::between(db_expression value, db_expression lower, db_expression upper, bool negate) {
    return push_node(detail::expression_nodes(storage()).between(
        require_expression(value), require_expression(lower), require_expression(upper), negate));
}
db_expression db_query::tuple(std::span<const db_expression> values) {
    return push_node(make_node(db_node_kind::tuple, values));
}
db_expression db_query::list(std::span<const db_expression> values) {
    return push_node(make_node(db_node_kind::list, values));
}
db_expression db_query::array(std::span<const db_expression> values) {
    return push_node(make_node(db_node_kind::array, values));
}
db_expression db_query::any(db_expression array_value) {
    return push_node(detail::expression_nodes(storage()).operands(db_node_kind::any, require_expression(array_value)));
}
db_expression db_query::all(db_expression array_value) {
    return push_node(detail::expression_nodes(storage()).operands(db_node_kind::all, require_expression(array_value)));
}
db_expression db_query::cast(db_expression value, const db_type_definition& type) {
    return push_node(detail::expression_nodes(storage()).cast(require_expression(value), type));
}
db_expression db_query::alias(db_expression value, std::string_view name) {
    detail::require_name(name);
    auto node_value = make_node(db_node_kind::alias);
    node_value.left_ = require_expression(value);
    node_value.text_ = name;
    return push_node(std::move(node_value));
}
db_expression db_query::case_when(std::span<const db_case_branch> branches, db_expression otherwise) {
    return push_node(detail::expression_nodes(storage()).case_when(branches, [&](const auto& arg) { return require_expression(arg.expression(*this)); }, [&] { return otherwise.empty() ? no_db_node : require_expression(otherwise); }));
}
db_query db_query::adopt(db_query&& query) const {
    if (&query == this) {
        return query.clone();
    }
    return std::move(query);
}
db_expression db_query::add_subquery(db_node_kind kind, db_query&& query) {
    query.require_select_query();
    auto& s = storage();
    auto node_value = detail::expression_nodes(s).subquery(kind, s.queries_.size());
    detail::reserve_append(s.nodes_, 1);
    detail::reserve_append(s.queries_, 1);
    s.queries_.push_back(adopt(std::move(query)));
    return push_node(std::move(node_value));
}
db_expression db_query::subquery(const db_query& query) {
    return subquery(query.clone());
}
db_expression db_query::subquery(db_query&& query) {
    return add_subquery(db_node_kind::subquery, std::move(query));
}
db_expression db_query::exists(const db_query& query) {
    return exists(query.clone());
}
db_expression db_query::exists(db_query&& query) {
    return add_subquery(db_node_kind::exists, std::move(query));
}
db_expression db_query::aggregate(std::string_view function, std::span<const db_expression> args, bool distinct, std::span<const db_order_term> order) {
    return push_node(detail::expression_nodes(storage()).aggregate(function, args, distinct, order,
        [&](const auto& arg) {
            if constexpr (std::same_as<std::remove_cvref_t<decltype(arg)>, db_expression>) {
                return require_expression(arg);
            } else {
                return require_expression(arg.expression(*this));
            }
        }));
}
db_expression db_query::filter(db_expression aggregate, db_expression predicate) {
    return push_node(detail::expression_nodes(storage()).operands(db_node_kind::filter,
        require_expression(aggregate), require_expression(predicate)));
}
db_expression db_query::over(db_expression function, const db_window_options& window) {
    const auto function_node = require_expression(function);
    return push_node(detail::expression_nodes(storage()).over(function_node, window,
        [&](const auto& arg) { return require_expression(arg.expression(*this)); }));
}
db_expression db_query::within_group(db_expression function, std::span<const db_order_term> order) {
    return push_node(detail::expression_nodes(storage()).within_group(require_expression(function), order,
        [&](const auto& arg) { return require_expression(arg.expression(*this)); }));
}
db_expression db_query::extract(db_date_part part, db_expression value) {
    return push_node(detail::expression_nodes(storage()).extract(part, require_expression(value)));
}
db_expression db_query::subscript(db_expression array_value, db_expression index) {
    return push_node(detail::expression_nodes(storage()).operands(db_node_kind::subscript,
        require_expression(array_value), require_expression(index)));
}
db_expression db_query::collate(db_expression value, std::string_view collation) {
    return push_node(detail::expression_nodes(storage()).collate(require_expression(value), collation));
}
db_expression db_query::at_time_zone(db_expression value, db_expression zone) {
    return push_node(detail::expression_nodes(storage()).operands(db_node_kind::at_time_zone,
        require_expression(value), require_expression(zone)));
}

db_expression db_query::import_expression(db_expression expression, std::string_view source_qualifier, std::string_view target_qualifier) {
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
    return db_expression(&target, importer.import_node(expression.node_, 0));
}

db_query& db_query::replace_expressions(
    std::vector<std::size_t>& target, std::span<const db_expression> expressions) {
    for (const auto expression : expressions) {
        (void)require_expression(expression);
    }
    target.reserve(expressions.size());
    target.clear();
    for (const auto expression : expressions) {
        target.push_back(expression.node_);
    }
    return *this;
}
db_query& db_query::combine_clause_predicate(
    std::size_t& clause, db_binary_operator op, db_expression predicate) {
    const auto previous = clause;
    clause = require_expression(
        previous == no_db_node ? predicate : binary(db_expression(storage_.get(), previous), op, predicate));
    return *this;
}
db_query& db_query::select(std::span<const db_expression> projections) {
    return replace_expressions(storage().projections_, projections);
}
db_query& db_query::add_select(db_expression projection) {
    storage().projections_.push_back(require_expression(projection));
    return *this;
}
db_query& db_query::from(std::string_view table_value, std::string_view alias) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_unqualified_name(alias);
    }
    auto& s = storage();
    detail::db_query_source source_value;
    source_value.name_ = table_value;
    source_value.alias_ = alias;
    s.source_ = std::move(source_value);
    return *this;
}
db_query& db_query::from(const db_query& query, std::string_view alias, const db_source_options& options) {
    return from(query.clone(), alias, options);
}
db_query& db_query::from(db_query&& query, std::string_view alias, const db_source_options& options) {
    detail::require_unqualified_name(alias);
    query.require_select_query();
    auto& s = storage();
    detail::db_query_source source_value;
    source_value.kind_ = db_source_kind::query;
    source_value.alias_ = alias;
    source_value.query_ = s.queries_.size();
    detail::set_source_options(source_value, options);
    detail::reserve_append(s.queries_, 1);
    s.queries_.push_back(adopt(std::move(query)));
    s.source_.emplace(std::move(source_value));
    return *this;
}
db_query& db_query::from_function(db_expression function, std::string_view alias, const db_source_options& options) {
    detail::require_unqualified_name(alias);
    const auto index = require_expression(function);
    auto& s = storage();
    detail::db_query_source source_value;
    source_value.kind_ = db_source_kind::function;
    source_value.expression_ = index;
    source_value.alias_ = alias;
    detail::set_source_options(source_value, options);
    s.source_ = std::move(source_value);
    return *this;
}
db_query& db_query::clear_source() {
    storage().source_.reset();
    return *this;
}
db_query& db_query::join(db_join_type type, std::string_view table_value, db_expression on, std::string_view alias, std::span<const std::string_view> using_columns) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_unqualified_name(alias);
    }
    auto& s = storage();
    detail::db_query_source source_value;
    source_value.name_ = table_value;
    source_value.alias_ = alias;
    detail::db_stored_join join(type, std::move(source_value), on.empty() ? no_db_node : require_expression(on));
    join.using_columns_.reserve(using_columns.size());
    for (const auto name : using_columns) {
        detail::require_name(name);
        join.using_columns_.emplace_back(name);
    }
    s.joins_.push_back(std::move(join));
    return *this;
}
db_query& db_query::join(db_join_type type, const db_query& query, db_expression on, std::string_view alias, const db_source_options& options) {
    return join(type, query.clone(), on, alias, options);
}
db_query& db_query::join(db_join_type type, db_query&& query, db_expression on, std::string_view alias, const db_source_options& options) {
    detail::require_unqualified_name(alias);
    const auto predicate = on.empty() ? no_db_node : require_expression(on);
    query.require_select_query();
    auto& s = storage();
    detail::db_query_source source_value;
    source_value.kind_ = db_source_kind::query;
    source_value.query_ = s.queries_.size();
    source_value.alias_ = alias;
    detail::set_source_options(source_value, options);
    detail::db_stored_join join_value(type, std::move(source_value), predicate);
    detail::reserve_append(s.queries_, 1);
    detail::reserve_append(s.joins_, 1);
    s.queries_.push_back(adopt(std::move(query)));
    s.joins_.push_back(std::move(join_value));
    return *this;
}
db_query& db_query::join_function(db_join_type type, db_expression function, db_expression on, std::string_view alias, const db_source_options& options) {
    detail::require_unqualified_name(alias);
    const auto function_id = require_expression(function);
    const auto predicate = on.empty() ? no_db_node : require_expression(on);
    auto& s = storage();
    detail::db_query_source source_value;
    source_value.kind_ = db_source_kind::function;
    source_value.expression_ = function_id;
    source_value.alias_ = alias;
    detail::set_source_options(source_value, options);
    s.joins_.emplace_back(type, std::move(source_value), predicate);
    return *this;
}
db_query& db_query::where(db_expression predicate) {
    storage().predicate_ = require_expression(predicate);
    return *this;
}
db_query& db_query::and_where(db_expression predicate) {
    return combine_clause_predicate(storage().predicate_, db_binary_operator::and_value, predicate);
}
db_query& db_query::or_where(db_expression predicate) {
    return combine_clause_predicate(storage().predicate_, db_binary_operator::or_value, predicate);
}
db_query& db_query::clear_where() {
    storage().predicate_ = no_db_node;
    return *this;
}
db_query& db_query::group_by(std::span<const db_expression> expressions) {
    return replace_expressions(storage().groups_, expressions);
}
db_query& db_query::add_group_by(db_expression expression) {
    storage().groups_.push_back(require_expression(expression));
    return *this;
}
db_query& db_query::having(db_expression predicate) {
    storage().having_ = require_expression(predicate);
    return *this;
}
db_query& db_query::and_having(db_expression predicate) {
    return combine_clause_predicate(storage().having_, db_binary_operator::and_value, predicate);
}
db_query& db_query::or_having(db_expression predicate) {
    return combine_clause_predicate(storage().having_, db_binary_operator::or_value, predicate);
}
db_query& db_query::clear_having() {
    storage().having_ = no_db_node;
    return *this;
}
db_query& db_query::order_by(db_expression expression, db_order_direction direction, db_nulls_order nulls) {
    const auto index = require_expression(expression);
    auto& s = storage();
    s.orders_.reserve(1);
    s.orders_.clear();
    s.orders_.push_back({index, direction, nulls});
    return *this;
}
db_query& db_query::add_order_by(db_expression expression, db_order_direction direction, db_nulls_order nulls) {
    storage().orders_.push_back({require_expression(expression), direction, nulls});
    return *this;
}
db_query& db_query::clear_order() {
    storage().orders_.clear();
    return *this;
}
db_query& db_query::limit(std::optional<query_row_count> count) {
    storage().limit_ = count ? std::optional<std::uint64_t>(count->value()) : std::nullopt;
    return *this;
}
db_query& db_query::set_limit_value(std::uint64_t count) {
    storage().limit_ = count;
    return *this;
}
db_query& db_query::offset(std::optional<query_row_count> count) {
    storage().offset_ = count ? std::optional<std::uint64_t>(count->value()) : std::nullopt;
    return *this;
}
db_query& db_query::set_offset_value(std::uint64_t count) {
    storage().offset_ = count;
    return *this;
}
db_query& db_query::distinct(bool enabled) {
    auto& s = storage();
    s.distinct_ = enabled;
    s.distinct_on_.clear();
    return *this;
}
db_query& db_query::distinct_on(std::span<const db_expression> expressions) {
    if (expressions.empty()) {
        throw std::invalid_argument("DISTINCT ON requires an expression");
    }
    auto& s = storage();
    (void)replace_expressions(s.distinct_on_, expressions);
    s.distinct_ = false;
    return *this;
}
db_query& db_query::returning(std::span<const db_expression> expressions) {
    return replace_expressions(storage().returning_, expressions);
}
db_query& db_query::insert_into(std::string_view table_value, std::span<const std::string_view> columns, std::string_view alias) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_unqualified_name(alias);
    }
    for (std::size_t i = 0; i < columns.size(); ++i) {
        detail::require_name(columns[i]);
        for (std::size_t j = 0; j < i; ++j) {
            if (columns[j] == columns[i]) {
                throw std::invalid_argument("duplicate INSERT column");
            }
        }
    }
    auto& s = storage();
    detail::require_statement_kind_change(s, db_query_kind::insert);
    if (s.kind_ == db_query_kind::insert && !s.rows_.empty() && !std::ranges::equal(s.columns_, columns)) {
        throw std::invalid_argument("INSERT rows already use a different column list");
    }
    std::string target(table_value);
    std::string target_alias(alias);
    std::vector<std::string> staged_columns;
    staged_columns.reserve(columns.size());
    for (const auto column : columns) {
        staged_columns.emplace_back(column);
    }
    s.target_.swap(target);
    s.target_alias_.swap(target_alias);
    s.columns_.swap(staged_columns);
    s.kind_ = db_query_kind::insert;
    return *this;
}
db_query& db_query::values(std::span<const db_expression> row) {
    auto& s = storage();
    if (s.kind_ != db_query_kind::select && s.kind_ != db_query_kind::values && s.kind_ != db_query_kind::insert) {
        throw std::invalid_argument("VALUES requires an INSERT or a values query");
    }
    std::vector<std::size_t> values;
    values.reserve(row.size());
    for (const auto expression : row) {
        values.push_back(require_expression(expression));
    }
    s.rows_.push_back(std::move(values));
    if (s.kind_ != db_query_kind::insert) {
        s.kind_ = db_query_kind::values;
    }
    return *this;
}
db_query& db_query::insert_rows(std::span<const std::string_view> columns,
    std::span<const db_expression> values, std::size_t row_count) {
    auto& s = storage();
    if (s.kind_ != db_query_kind::insert || s.insert_query_ != no_db_node) {
        throw std::invalid_argument("named INSERT rows require an INSERT without SELECT");
    }
    if (row_count == 0 ||
        (columns.empty() ? !values.empty()
                         : values.size() / columns.size() != row_count || values.size() % columns.size() != 0)) {
        throw std::invalid_argument("INSERT batch dimensions differ");
    }
    if (row_count > s.rows_.max_size() - s.rows_.size()) {
        throw std::length_error("INSERT has too many rows");
    }
    for (const auto& row : s.rows_) {
        if (row.size() != s.columns_.size()) {
            throw std::invalid_argument("existing INSERT rows do not match their named columns");
        }
    }
    for (const auto expression : values) {
        (void)require_expression(expression);
    }
    detail::node_index_map positions(columns.size());
    std::vector<std::string> new_columns;
    for (std::size_t index = 0; index < columns.size(); ++index) {
        detail::require_name(columns[index]);
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (columns[earlier] == columns[index]) {
                throw std::invalid_argument("duplicate INSERT column");
            }
        }
        const auto existing = std::ranges::find(s.columns_, columns[index]);
        if (existing != s.columns_.end()) {
            positions[index] = static_cast<std::size_t>(existing - s.columns_.begin());
        } else {
            if (new_columns.empty()) {
                new_columns = s.columns_;
            }
            positions[index] = new_columns.size();
            new_columns.emplace_back(columns[index]);
        }
    }
    const auto width = new_columns.empty() ? s.columns_.size() : new_columns.size();
    if (width == 0 && (row_count != 1 || !s.rows_.empty())) {
        throw std::invalid_argument("bulk default-only inserts require at least one explicit column");
    }
    const bool needs_default = width > columns.size() || (!s.rows_.empty() && width > s.columns_.size());
    const auto default_node = needs_default ? default_value().node_ : no_db_node;
    std::vector<std::vector<std::size_t>> rows;
    rows.reserve(row_count);
    for (std::size_t row_index = 0; row_index < row_count; ++row_index) {
        auto& row = rows.emplace_back();
        row.assign(width, default_node);
        for (std::size_t column = 0; column < columns.size(); ++column) {
            row[positions[column]] = values[row_index * columns.size() + column].node_;
        }
    }
    // Capacity changes are not visible query semantics. Complete every possible
    // allocation before extending old rows or publishing any new row.
    detail::reserve_append(s.rows_, row_count);
    if (!new_columns.empty()) {
        for (auto& row : s.rows_) {
            row.reserve(width);
        }
        for (auto& row : s.rows_) {
            row.resize(width, default_node);
        }
        s.columns_.swap(new_columns);
    }
    for (auto& row : rows) {
        s.rows_.push_back(std::move(row));
    }
    return *this;
}
db_query& db_query::insert_from(const db_query& query) {
    return insert_from(query.clone());
}
db_query& db_query::insert_from(db_query&& query) {
    if (storage().kind_ != db_query_kind::insert || !storage().rows_.empty()) {
        throw std::invalid_argument("INSERT SELECT requires an INSERT target without VALUES");
    }
    query.require_select_query();
    auto& s = storage();
    const auto index = s.queries_.size();
    detail::reserve_append(s.queries_, 1);
    s.queries_.push_back(adopt(std::move(query)));
    s.insert_query_ = index;
    return *this;
}
db_query& db_query::on_conflict(const db_conflict_options& conflict) {
    auto& s = storage();
    if (s.kind_ != db_query_kind::insert) {
        throw std::invalid_argument("ON CONFLICT requires an INSERT");
    }
    detail::db_stored_conflict owned;
    owned.columns_.reserve(conflict.columns_.size());
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
    owned.assignments_.reserve(conflict.update_.size());
    for (const auto& assignment : conflict.update_) {
        detail::require_name(assignment.column_);
        if (std::ranges::any_of(owned.assignments_, [&](const auto& existing) { return std::string_view(existing.column_) == assignment.column_; })) {
            throw std::invalid_argument("duplicate conflict update column");
        }
        owned.assignments_.emplace_back(assignment.column_, require_expression(assignment.expression(*this)));
    }
    s.conflict_ = std::move(owned);
    return *this;
}
db_query& db_query::update(std::string_view table_value, std::string_view alias) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_unqualified_name(alias);
    }
    auto& s = storage();
    detail::require_statement_kind_change(s, db_query_kind::update);
    s.kind_ = db_query_kind::update;
    s.target_ = table_value;
    s.target_alias_ = alias;
    return *this;
}
db_query& db_query::set(std::string_view column, db_expression value) {
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
    s.assignments_.emplace_back(column, expression);
    return *this;
}
db_query& db_query::set(const db_assignment& assignment) {
    return set(assignment.column_, assignment.expression(*this));
}
db_query& db_query::set(std::span<const db_assignment> assignments) {
    if (assignments.size() == 1) {
        return set(assignments.front());
    }
    auto& s = storage();
    if (s.kind_ != db_query_kind::update) {
        throw std::invalid_argument("SET requires an UPDATE");
    }
    std::vector<detail::db_stored_assignment> prepared;
    prepared.reserve(assignments.size());
    detail::node_index_map positions(assignments.size());
    const auto previous_count = s.assignments_.size();
    std::size_t added = 0;
    for (const auto& assignment : assignments) {
        const auto column = assignment.column_.view();
        detail::require_name(column);
        const auto expression = require_expression(assignment.expression(*this));
        const auto previous = std::ranges::find(prepared, column, &detail::db_stored_assignment::column_);
        if (previous != prepared.end()) {
            previous->expression_ = expression;
            continue;
        }
        const auto existing = std::ranges::find(s.assignments_, column, &detail::db_stored_assignment::column_);
        if (existing != s.assignments_.end()) {
            positions[prepared.size()] = static_cast<std::size_t>(existing - s.assignments_.begin());
        } else {
            if (added == s.assignments_.max_size() - previous_count) {
                throw std::length_error("UPDATE has too many assignments");
            }
            positions[prepared.size()] = previous_count + added++;
        }
        prepared.emplace_back(column, expression);
    }
    s.assignments_.reserve(previous_count + added);
    for (std::size_t index = 0; index < prepared.size(); ++index) {
        if (positions[index] < previous_count) {
            s.assignments_[positions[index]].expression_ = prepared[index].expression_;
        } else {
            s.assignments_.push_back(std::move(prepared[index]));
        }
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
        detail::require_unqualified_name(alias);
    }
    auto& s = storage();
    detail::require_statement_kind_change(s, db_query_kind::delete_value);
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
        detail::require_unqualified_name(table);
    }
    detail::db_stored_lock lock_value(options);
    storage().lock_.emplace(std::move(lock_value));
    return *this;
}
db_query& db_query::clear_lock() {
    storage().lock_.reset();
    return *this;
}
db_query& db_query::with(std::string_view name, const db_query& query, const db_cte_options& options) {
    return with(name, query.clone(), options);
}
db_query& db_query::with(std::string_view name, db_query&& query, const db_cte_options& options) {
    auto& s = storage();
    detail::validate_cte(s, name, options.columns_.view());
    auto cte = detail::prepare_cte(s, name, options.recursive_, options.materialization_, options.columns_.view());
    s.queries_.push_back(adopt(std::move(query)));
    s.ctes_.push_back(std::move(cte));
    return *this;
}
void db_query::with_columns(std::string_view name, const db_query& query, db_materialization materialization,
    std::span<const std::string> columns) {
    auto& s = storage();
    detail::validate_cte(s, name, columns);
    auto cte = detail::prepare_cte(s, name, false, materialization, columns);
    s.queries_.push_back(query.clone());
    s.ctes_.push_back(std::move(cte));
}
db_query& db_query::combine(db_set_operation operation, const db_query& query) {
    return combine(operation, query.clone());
}
db_query& db_query::combine(db_set_operation operation, db_query&& query) {
    auto& s = storage();
    if ((s.kind_ != db_query_kind::select && s.kind_ != db_query_kind::values) ||
        (query.storage().kind_ != db_query_kind::select && query.storage().kind_ != db_query_kind::values)) {
        throw std::invalid_argument("set operations require SELECT or VALUES queries");
    }
    const auto index = s.queries_.size();
    detail::reserve_append(s.queries_, 1);
    detail::reserve_append(s.set_operations_, 1);
    s.queries_.push_back(adopt(std::move(query)));
    s.set_operations_.push_back({operation, index});
    return *this;
}
db_statement& db_statement::operator=(db_statement&& other) {
    if (this != &other) {
        sql_ = std::move(other.sql_);
        params_ = std::move(other.params_);
        returns_rows_ = std::exchange(other.returns_rows_, false);
        other.sql_.clear();
        other.params_.clear();
    }
    return *this;
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

void validate_query_shape(const db_query_storage& s) {
    const bool select = s.kind_ == db_query_kind::select;
    const bool values = s.kind_ == db_query_kind::values;
    const bool insert = s.kind_ == db_query_kind::insert;
    const bool update = s.kind_ == db_query_kind::update;
    if (!select && (!s.projections_.empty() || s.distinct_ || !s.distinct_on_.empty() || !s.groups_.empty() || s.having_ != no_db_node)) {
        throw std::invalid_argument("SELECT clauses cannot be attached to a non-SELECT query");
    }
    if ((select || values) && (!s.returning_.empty() || !s.target_.empty())) {
        throw std::invalid_argument("a row query cannot have a DML target or RETURNING");
    }
    if ((insert || values) && (s.source_ || !s.joins_.empty() || s.predicate_ != no_db_node)) {
        throw std::invalid_argument("INSERT/VALUES cannot have FROM/JOIN/WHERE clauses");
    }
    if (!s.source_ && !s.joins_.empty()) {
        throw std::invalid_argument("JOIN requires a FROM source");
    }
    if (!insert && (s.conflict_ || s.insert_query_ != no_db_node || !s.columns_.empty())) {
        throw std::invalid_argument("INSERT clauses on a non-INSERT query");
    }
    if (!insert && !values && !s.rows_.empty()) {
        throw std::invalid_argument("VALUES on an incompatible query");
    }
    if (!update && !s.assignments_.empty()) {
        throw std::invalid_argument("SET on a non-UPDATE query");
    }
    if (!select && !values && (!s.orders_.empty() || s.limit_ || s.offset_ || s.lock_ || !s.set_operations_.empty())) {
        throw std::invalid_argument("ordering, pagination, locks and set operations require a row query");
    }
    if (s.lock_ && (!select || !s.set_operations_.empty() || s.distinct_ || !s.distinct_on_.empty() || !s.groups_.empty() || s.having_ != no_db_node)) {
        throw std::invalid_argument("row locks require an ungrouped SELECT without DISTINCT or set operations");
    }
}

bool is_mariadb_bare_function_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    const auto is_ascii_letter = [](char value) noexcept {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
    };
    if (!is_ascii_letter(name.front()) && name.front() != '_') {
        return false;
    }
    return std::ranges::all_of(name.substr(1), [&](char value) noexcept {
        return is_ascii_letter(value) || (value >= '0' && value <= '9') || value == '_' || value == '$';
    });
}

[[nodiscard]] bool same_db_value(const db_value& a, const db_value& b) noexcept {
    if (a.type() != b.type()) {
        return false;
    }
    switch (a.type()) {
        case db_value_type::null:
            return true;
        case db_value_type::string:
            return a.text() == b.text();
        case db_value_type::signed_value:
            return a.signed_value() == b.signed_value();
        case db_value_type::unsigned_value:
            return a.unsigned_value() == b.unsigned_value();
        case db_value_type::double_value:
            return a.double_value() == b.double_value();
        case db_value_type::bool_value:
            return a.bool_value() == b.bool_value();
    }
    return false;
}

struct date_part_sql_spec final {
    std::string_view token_;
    std::string_view pg_feature_{};
};

constexpr std::array<date_part_sql_spec, 11> date_part_sql_specs{{
    {"EPOCH", "EXTRACT EPOCH"},
    {"YEAR"},
    {"MONTH"},
    {"DAY"},
    {"HOUR"},
    {"MINUTE"},
    {"SECOND"},
    {"DOW", "EXTRACT DOW"},
    {"DOY", "EXTRACT DOY"},
    {"WEEK"},
    {"QUARTER"},
}};
static_assert(static_cast<std::size_t>(db_date_part::quarter) + 1 == date_part_sql_specs.size());

struct binary_operator_sql_spec final {
    std::string_view pg_token_;
    std::string_view maria_token_{};
    std::string_view pg_feature_{};
};

constexpr std::string_view pg_extended_operator_feature = "PostgreSQL JSON, array, regex or network operator";

constexpr std::array<binary_operator_sql_spec, 47> binary_operator_sql_specs{{
    {"="},
    {"<>"},
    {"<"},
    {"<="},
    {">"},
    {">="},
    {"AND"},
    {"OR"},
    {"+"},
    {"-"},
    {"*"},
    {"/"},
    {"%"},
    {"||"},
    {"LIKE"},
    {"NOT LIKE"},
    {"ILIKE", {}, "ILIKE"},
    {"NOT ILIKE", {}, "NOT ILIKE"},
    {"IN"},
    {"NOT IN"},
    {"IS DISTINCT FROM", "<=>"},
    {"IS NOT DISTINCT FROM", "<=>"},
    {"&"},
    {"|"},
    {"#", "^"},
    {"->", {}, pg_extended_operator_feature},
    {"->>", {}, pg_extended_operator_feature},
    {"#>", {}, pg_extended_operator_feature},
    {"#>>", {}, pg_extended_operator_feature},
    {"@>", {}, pg_extended_operator_feature},
    {"<@", {}, pg_extended_operator_feature},
    {"?", {}, pg_extended_operator_feature},
    {"?|", {}, pg_extended_operator_feature},
    {"?&", {}, pg_extended_operator_feature},
    {"||", {}, pg_extended_operator_feature},
    {"-", {}, pg_extended_operator_feature},
    {"#-", {}, pg_extended_operator_feature},
    {"@>", {}, pg_extended_operator_feature},
    {"<@", {}, pg_extended_operator_feature},
    {"&&", {}, pg_extended_operator_feature},
    {"~", {}, pg_extended_operator_feature},
    {"~*", {}, pg_extended_operator_feature},
    {">>", {}, pg_extended_operator_feature},
    {">>=", {}, pg_extended_operator_feature},
    {"<<", {}, pg_extended_operator_feature},
    {"<<=", {}, pg_extended_operator_feature},
    {"&&", {}, pg_extended_operator_feature},
}};
static_assert(static_cast<std::size_t>(db_binary_operator::inet_overlap) + 1 == binary_operator_sql_specs.size());

// How a bound PostgreSQL parameter is typed in compiled SQL. By default it is
// sent untyped and PostgreSQL infers its type from the surrounding SQL: the
// other operand, the target column, the parameter type of the selected
// function or operator overload, a Boolean context, an explicit cast. A value
// carries its C++ value category only where PostgreSQL provably cannot type an
// unknown parameter: "any" and polymorphic-only function arguments, numeric
// and boolean operands of arithmetic operators whose operands are all bound
// values, the null predicates, and COALESCE, NULLIF, GREATEST and LEAST over
// bound values only, which PostgreSQL would otherwise resolve as text.
enum class pg_parameter_cast : std::uint8_t {
    // The surrounding SQL types the value.
    context,
    // Nothing types the value: signed integers render ::int8, unsigned
    // integers ::numeric, doubles ::float8, booleans ::bool and text ::text.
    value_category,
    // Bare integer bitwise operands use bigint's signed range.
    bitwise_integer,
    // The PostgreSQL signature at this position accepts only int4; other
    // values stay untyped.
    int4,
};

struct pg_function_parameter_casts final {
    std::string_view name_;
    // Bit i selects argument i.
    std::uint32_t value_category_positions_;
    std::uint32_t int4_positions_;
};

constexpr std::uint32_t pg_every_argument = ~std::uint32_t{0};
// Every argument after a leading text one (a separator or format string).
constexpr std::uint32_t pg_after_first_argument = ~std::uint32_t{1};

// Built-in functions whose arguments at these positions are declared only as
// "any", anyelement or anycompatible, so an untyped argument cannot be
// resolved, and int4-only arguments, where an integer is typed int4 explicitly.
// Every other function argument keeps the type of its selected overload.
// Function names are emitted quoted, so the match is exact.
constexpr std::array pg_function_parameter_cast_table{
    pg_function_parameter_casts{"array_agg", pg_every_argument, 0},
    pg_function_parameter_casts{"array_length", 0, 0b10},
    pg_function_parameter_casts{"array_lower", 0, 0b10},
    pg_function_parameter_casts{"array_upper", 0, 0b10},
    pg_function_parameter_casts{"chr", 0, 0b1},
    pg_function_parameter_casts{"concat", pg_every_argument, 0},
    pg_function_parameter_casts{"concat_ws", pg_after_first_argument, 0},
    pg_function_parameter_casts{"format", pg_after_first_argument, 0},
    pg_function_parameter_casts{"json_agg", pg_every_argument, 0},
    pg_function_parameter_casts{"json_build_array", pg_every_argument, 0},
    pg_function_parameter_casts{"json_build_object", pg_every_argument, 0},
    pg_function_parameter_casts{"json_object_agg", pg_every_argument, 0},
    pg_function_parameter_casts{"jsonb_agg", pg_every_argument, 0},
    pg_function_parameter_casts{"jsonb_build_array", pg_every_argument, 0},
    pg_function_parameter_casts{"jsonb_build_object", pg_every_argument, 0},
    pg_function_parameter_casts{"jsonb_object_agg", pg_every_argument, 0},
    pg_function_parameter_casts{"lag", 0, 0b10},
    pg_function_parameter_casts{"lead", 0, 0b10},
    pg_function_parameter_casts{"left", 0, 0b10},
    pg_function_parameter_casts{"lpad", 0, 0b10},
    pg_function_parameter_casts{"make_date", 0, 0b111},
    pg_function_parameter_casts{"make_interval", 0, 0b111111},
    pg_function_parameter_casts{"make_time", 0, 0b11},
    pg_function_parameter_casts{"make_timestamp", 0, 0b11111},
    pg_function_parameter_casts{"make_timestamptz", 0, 0b11111},
    pg_function_parameter_casts{"nth_value", 0, 0b10},
    pg_function_parameter_casts{"ntile", 0, 0b1},
    pg_function_parameter_casts{"num_nonnulls", pg_every_argument, 0},
    pg_function_parameter_casts{"num_nulls", pg_every_argument, 0},
    pg_function_parameter_casts{"repeat", 0, 0b10},
    pg_function_parameter_casts{"right", 0, 0b10},
    pg_function_parameter_casts{"round", 0, 0b10},
    pg_function_parameter_casts{"rpad", 0, 0b10},
    pg_function_parameter_casts{"split_part", 0, 0b100},
    pg_function_parameter_casts{"substr", 0, 0b110},
    pg_function_parameter_casts{"substring", 0, 0b110},
    pg_function_parameter_casts{"to_json", pg_every_argument, 0},
    pg_function_parameter_casts{"to_jsonb", pg_every_argument, 0},
    pg_function_parameter_casts{"trim_array", 0, 0b10},
    pg_function_parameter_casts{"trunc", 0, 0b10},
    pg_function_parameter_casts{"width_bucket", 0, 0b1000},
};

static_assert(std::ranges::is_sorted(pg_function_parameter_cast_table, {}, &pg_function_parameter_casts::name_));

// The cast for argument `position` of `function`, unqualified or in pg_catalog.
[[nodiscard]] pg_parameter_cast pg_function_parameter_cast(std::string_view function, std::size_t position) noexcept {
    constexpr std::string_view catalog = "pg_catalog.";
    if (function.starts_with(catalog)) {
        function.remove_prefix(catalog.size());
    }
    if (position >= 32) {
        return pg_parameter_cast::context;
    }
    const auto found = std::ranges::lower_bound(
        pg_function_parameter_cast_table, function, {}, &pg_function_parameter_casts::name_);
    if (found != pg_function_parameter_cast_table.end() && found->name_ == function) {
        if ((found->int4_positions_ >> position) & 1U) {
            return pg_parameter_cast::int4;
        }
        if ((found->value_category_positions_ >> position) & 1U) {
            return pg_parameter_cast::value_category;
        }
    }
    return pg_parameter_cast::context;
}

// Two unknown arithmetic operands select no overload. Bitwise integer
// operands need an integral SQL type rather than the wider numeric category.
// Concatenation resolves unknown || unknown to text || text.
[[nodiscard]] pg_parameter_cast pg_binary_operand_cast(db_binary_operator value) noexcept {
    switch (value) {
        case db_binary_operator::add:
        case db_binary_operator::subtract:
        case db_binary_operator::multiply:
        case db_binary_operator::divide:
        case db_binary_operator::modulo:
            return pg_parameter_cast::value_category;
        case db_binary_operator::bit_and:
        case db_binary_operator::bit_or:
        case db_binary_operator::bit_xor:
            return pg_parameter_cast::bitwise_integer;
        default:
            return pg_parameter_cast::context;
    }
}

// A bound non-NULL value: it has no SQL type until a cast or its context gives one.
[[nodiscard]] bool is_untyped_value(const db_query_node& node) noexcept {
    return node.kind_ == db_node_kind::value && node.value_.type() != db_value_type::null;
}

// Only bare integers acquire the bitwise range contract. Other numeric and
// boolean values retain their category; strings retain SQL context because a
// reused parameter may already have been typed by an earlier reference.
[[nodiscard]] pg_parameter_cast pg_operator_operand_cast(const db_query_node& node, pg_parameter_cast cast) noexcept {
    if (!is_untyped_value(node) || node.value_.type() == db_value_type::string) {
        return pg_parameter_cast::context;
    }
    const auto type = node.value_.type();
    return cast == pg_parameter_cast::bitwise_integer &&
                   type != db_value_type::signed_value && type != db_value_type::unsigned_value
               ? pg_parameter_cast::value_category
               : cast;
}

// PostgreSQL's Bind message counts parameters in an Int16 (protocol 3.0).
constexpr std::size_t max_postgresql_parameters = 65535;

// PostgreSQL lexes `$` followed by digits as a positional parameter unless the
// `$` continues an identifier (`a$1`).
[[nodiscard]] bool contains_pg_positional_parameter(std::string_view text) noexcept {
    const auto is_digit = [](char value) noexcept { return value >= '0' && value <= '9'; };
    for (auto position = text.find('$'); position != std::string_view::npos; position = text.find('$', position + 1)) {
        if (position + 1 == text.size() || !is_digit(text[position + 1])) {
            continue;
        }
        if (position == 0) {
            return true;
        }
        const auto previous = static_cast<unsigned char>(text[position - 1]);
        const bool continues_identifier = previous >= 0x80 || previous == '_' || previous == '$' ||
                                          is_digit(static_cast<char>(previous)) ||
                                          (previous >= 'a' && previous <= 'z') || (previous >= 'A' && previous <= 'Z');
        if (!continues_identifier) {
            return true;
        }
    }
    return false;
}

class query_sql_writer final {
public:
    query_sql_writer(db_driver driver, db_parameter_mode mode)
        : driver_(driver),
          mode_(mode) {
        require_db_dialect(driver);
    }

    std::string sql_;
    std::vector<db_value> params_;
    [[nodiscard]] db_driver driver() const noexcept {
        return driver_;
    }

    bool pg() const noexcept {
        return driver_ == db_driver::postgresql;
    }

    [[noreturn]] static void unsupported(std::string_view feature) {
        throw std::invalid_argument(std::string(feature) + " is not supported by this database dialect");
    }

    void require_pg(std::string_view feature) const {
        if (!pg()) {
            unsupported(feature);
        }
    }

    void identifier(std::string_view name) {
        append_db_identifier(sql_, name, driver_);
    }

    void qualified(std::string_view name) {
        append_db_qualified_identifier(sql_, name, driver_);
    }

    void function_name(std::string_view name) {
        if (driver_ == db_driver::mariadb && is_mariadb_bare_function_name(name)) {
            sql_ += name;
        } else {
            qualified(name);
        }
    }

    // `cast` refines PostgreSQL parameter typing for the position the value
    // fills. Bare bitwise integers share the same range and SQL type in bound
    // and literal output; other casts leave literals to PostgreSQL's parser.
    void bound(const db_value& value, pg_parameter_cast cast = pg_parameter_cast::context) {
        append_parameter(value, nullptr, cast);
    }

    // Binds value node `index` of `s`. PostgreSQL numbers each node once,
    // however often grouped or repeated SQL emits it.
    void bound_node(const db_query_storage& s, std::size_t index, pg_parameter_cast cast) {
        append_parameter(s.nodes_[index].value_, parameter_position(s, index), cast);
    }

    void number(std::uint64_t value) {
        if (pg()) {
            if (!std::in_range<std::int64_t>(value)) {
                throw std::invalid_argument("PostgreSQL row count or offset exceeds bigint range");
            }
            bound(db_value(static_cast<std::int64_t>(value)));
        } else {
            bound(db_value(value));
        }
    }

    void type(const db_stored_type& value) {
        append_db_type_name(sql_, value.data_type_, value.custom_name_, value.length_, value.precision_,
            value.scale_, value.array_, driver_, db_sql_type_usage::cast);
    }

private:
    void append_parameter(const db_value& value, std::uint32_t* position, pg_parameter_cast cast) {
        const bool bitwise_integer = pg() && cast == pg_parameter_cast::bitwise_integer;
        if (bitwise_integer && value.type() == db_value_type::unsigned_value &&
            value.unsigned_value() > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            throw std::invalid_argument("PostgreSQL integer bitwise operand exceeds bigint range");
        }
        if (mode_ == db_parameter_mode::literal || value.type() == db_value_type::null) {
            if (bitwise_integer) {
                sql_ += '(';
            }
            append_db_literal(sql_, value, driver_);
            if (bitwise_integer) {
                // Cast the whole signed literal, including a possible minus.
                sql_ += ")::int8";
            }
            return;
        }
        if (position != nullptr && *position != 0) {
            pg_parameter(*position, value, cast);
            return;
        }
        if (pg() && params_.size() == max_postgresql_parameters) {
            throw std::length_error("PostgreSQL statement exceeds 65535 bound parameters");
        }
        params_.push_back(db_value(value));
        if (position != nullptr) {
            *position = static_cast<std::uint32_t>(params_.size());
        }
        if (pg()) {
            pg_parameter(params_.size(), value, cast);
        } else {
            sql_ += '?';
        }
    }

    // The parameter slot of a PostgreSQL bound value node, or nullptr where
    // parameters are not numbered.
    [[nodiscard]] std::uint32_t* parameter_position(const db_query_storage& s, std::size_t index) {
        if (!pg() || mode_ != db_parameter_mode::bound) {
            return nullptr;
        }
        if (positions_owner_ != &s) {
            const auto existing = std::ranges::find_if(parameter_positions_, [&s](const auto& entry_value) noexcept {
                return entry_value.owner_ == &s;
            });
            if (existing != parameter_positions_.end()) {
                positions_ = &existing->positions_;
            } else {
                auto& added = parameter_positions_.emplace_back(&s, std::vector<std::uint32_t>(s.nodes_.size(), 0));
                positions_ = &added.positions_;
            }
            positions_owner_ = &s;
        }
        return &(*positions_)[index];
    }

    void pg_parameter(std::size_t position, const db_value& value, pg_parameter_cast cast) {
        sql_ += '$';
        append_db_number(sql_, static_cast<std::uint64_t>(position));
        const auto type = value.type();
        const bool integer = type == db_value_type::signed_value || type == db_value_type::unsigned_value;
        if (cast == pg_parameter_cast::bitwise_integer) {
            sql_ += "::int8";
            return;
        }
        if (cast == pg_parameter_cast::int4) {
            if (integer) {
                sql_ += "::int4";
            }
            return;
        }
        if (cast == pg_parameter_cast::context) {
            return;
        }
        switch (type) {
            case db_value_type::signed_value:
                sql_ += "::int8";
                break;
            case db_value_type::unsigned_value:
                // The whole uint64 range exceeds bigint; numeric holds every
                // value exactly, independent of the bound value.
                sql_ += "::numeric";
                break;
            case db_value_type::double_value:
                sql_ += "::float8";
                break;
            case db_value_type::bool_value:
                sql_ += "::bool";
                break;
            case db_value_type::string:
                sql_ += "::text";
                break;
            case db_value_type::null:
                break;
        }
    }

    db_driver driver_;
    db_parameter_mode mode_;
    // One dense table per compiled storage, indexed by node and holding the
    // node's 1-based parameter number (0: not yet bound); positions_ caches
    // the table of positions_owner_.
    struct storage_parameter_positions final {
        const db_query_storage* owner_{nullptr};
        std::vector<std::uint32_t> positions_;
    };
    std::vector<storage_parameter_positions> parameter_positions_;
    const db_query_storage* positions_owner_{nullptr};
    std::vector<std::uint32_t>* positions_{nullptr};
};

}  // namespace

class db_query_compiler final {
public:
    db_query_compiler(db_driver driver, db_parameter_mode mode)
        : output_(driver, mode),
          mode_(mode),
          grouped_{nullptr, std::vector<std::pair<std::size_t, std::size_t>>{}} {}

    query_sql_writer output_;
    db_parameter_mode mode_;

    void expression(db_expression value) {
        if (value.empty()) {
            throw std::invalid_argument("empty database expression");
        }
        expr(*value.owner_, value.node_, 0);
    }
    void query(const db_query_storage& s, std::size_t depth = 0) {
        require_depth(depth);
        validate_query_shape(s);
        if (depth == 0) {
            output_.sql_.reserve(64U + s.nodes_.size() * 16U + s.projections_.size() * 12U +
                                 s.orders_.size() * 16U + s.joins_.size() * 32U + s.ctes_.size() * 48U);
        }
        auto enclosing_groups = std::exchange(grouped_, bound_groups(s));
        if (!s.ctes_.empty()) {
            if (!output_.pg() && s.kind_ != db_query_kind::select) {
                output_.unsupported("data-changing WITH");
            }
            output_.sql_ += "WITH ";
            if (std::ranges::any_of(s.ctes_, [](const auto& cte) { return cte.recursive_; })) {
                output_.sql_ += "RECURSIVE ";
            }
            separated(s.ctes_, [&](const auto& cte) {
                output_.identifier(cte.name_);
                if (!cte.columns_.empty()) {
                    output_.sql_ += " (";
                    identifiers(cte.columns_);
                    output_.sql_ += ')';
                }
                output_.sql_ += " AS ";
                switch (cte.materialization_) {
                    case db_materialization::default_value:
                        break;
                    case db_materialization::materialized:
                        output_.require_pg("materialized CTE");
                        output_.sql_ += "MATERIALIZED ";
                        break;
                    case db_materialization::not_materialized:
                        output_.require_pg("not-materialized CTE");
                        output_.sql_ += "NOT MATERIALIZED ";
                        break;
                }
                const auto& child_value = nested(s, cte.query_);
                if (child_value.kind_ != db_query_kind::select && child_value.kind_ != db_query_kind::values) {
                    output_.require_pg("data-changing CTE");
                    if (depth != 0) {
                        throw std::invalid_argument("data-changing CTE requires a top-level WITH statement");
                    }
                }
                output_.sql_ += '(';
                query(child_value, depth + 1);
                output_.sql_ += ')';
            });
            output_.sql_ += ' ';
        }
        for (std::size_t i = 0; i < s.set_operations_.size(); ++i) {
            output_.sql_ += '(';
        }
        body(s, depth + 1);
        for (const auto& operation : s.set_operations_) {
            output_.sql_ += ')';
            switch (operation.operation_) {
                case db_set_operation::union_value:
                    output_.sql_ += " UNION ";
                    break;
                case db_set_operation::union_all:
                    output_.sql_ += " UNION ALL ";
                    break;
                case db_set_operation::intersect:
                    output_.sql_ += " INTERSECT ";
                    break;
                case db_set_operation::intersect_all:
                    output_.require_pg("INTERSECT ALL");
                    output_.sql_ += " INTERSECT ALL ";
                    break;
                case db_set_operation::except:
                    output_.sql_ += " EXCEPT ";
                    break;
                case db_set_operation::except_all:
                    output_.require_pg("EXCEPT ALL");
                    output_.sql_ += " EXCEPT ALL ";
                    break;
            }
            output_.sql_ += '(';
            query(nested(s, operation.query_), depth + 1);
            output_.sql_ += ')';
        }
        if (!s.orders_.empty()) {
            output_.sql_ += " ORDER BY ";
            orders(s, s.orders_, depth + 1);
        }
        if (s.limit_) {
            output_.sql_ += " LIMIT ";
            output_.number(*s.limit_);
        } else if (s.offset_ && !output_.pg()) {
            output_.sql_ += " LIMIT 18446744073709551615";
        }
        if (s.offset_) {
            output_.sql_ += " OFFSET ";
            output_.number(*s.offset_);
        }
        if (s.lock_) {
            const auto& lock = *s.lock_;
            switch (lock.mode_) {
                case db_row_lock::update:
                    output_.sql_ += " FOR UPDATE";
                    break;
                case db_row_lock::no_key_update:
                    output_.require_pg("FOR NO KEY UPDATE");
                    output_.sql_ += " FOR NO KEY UPDATE";
                    break;
                case db_row_lock::share:
                    output_.sql_ += output_.pg() ? " FOR SHARE" : " LOCK IN SHARE MODE";
                    break;
                case db_row_lock::key_share:
                    output_.require_pg("FOR KEY SHARE");
                    output_.sql_ += " FOR KEY SHARE";
                    break;
            }
            if (!lock.tables_.empty()) {
                output_.require_pg("row lock OF");
                output_.sql_ += " OF ";
                identifiers(lock.tables_);
            }
            if (lock.nowait_) {
                output_.sql_ += " NOWAIT";
            }
            if (lock.skip_locked_) {
                if (!output_.pg() && lock.mode_ != db_row_lock::update) {
                    output_.unsupported("SKIP LOCKED for a shared lock");
                }
                output_.sql_ += " SKIP LOCKED";
            }
        }
        if (!s.returning_.empty()) {
            // MariaDB (>= 10.5) accepts RETURNING on INSERT and DELETE but not
            // on UPDATE (MDEV-10014).
            if (s.kind_ == db_query_kind::update) {
                output_.require_pg("UPDATE ... RETURNING");
            }
            output_.sql_ += " RETURNING ";
            expressions(s, s.returning_, depth + 1, true);
        }
        grouped_ = std::move(enclosing_groups);
    }

private:
    // Grouped bound expressions of one storage. PostgreSQL must see the same
    // parameter numbers wherever SELECT, GROUP BY and HAVING repeat a grouped
    // expression, but an imported copy has its own value nodes; each entry
    // maps such a GROUP BY expression to the node emitted for it.
    struct grouped_bound_values final {
        const db_query_storage* storage_{nullptr};
        // (GROUP BY node, node emitted for every equal expression)
        std::vector<std::pair<std::size_t, std::size_t>> groups_;
    };

    static void require_depth(std::size_t depth) {
        if (depth > max_db_query_depth) {
            throw std::length_error("database query nesting exceeds 256 levels");
        }
    }
    static const db_query_storage& nested(const db_query_storage& s, std::size_t index) {
        return s.queries_.at(index).storage();
    }
    [[nodiscard]] grouped_bound_values bound_groups(const db_query_storage& s) const {
        grouped_bound_values result{&s, std::vector<std::pair<std::size_t, std::size_t>>{}};
        if (!output_.pg() || mode_ != db_parameter_mode::bound) {
            return result;
        }
        for (const auto group : s.groups_) {
            if (!contains_bound_value(s, group)) {
                continue;
            }
            auto emitted = group;
            for (auto projection : s.projections_) {
                if (s.nodes_[projection].kind_ == db_node_kind::alias) {
                    projection = s.nodes_[projection].left_;
                }
                if (same_expression(s, group, projection)) {
                    emitted = projection;
                    break;
                }
            }
            result.groups_.emplace_back(group, emitted);
        }
        return result;
    }
    // The node to emit for `index`: the grouped expression it repeats, if any.
    [[nodiscard]] std::size_t grouped_node(const db_query_storage& s, std::size_t index) const {
        if (grouped_.storage_ != &s || grouped_.groups_.empty()) {
            return index;
        }
        const auto kind = s.nodes_.at(index).kind_;
        for (const auto& [group, emitted] : grouped_.groups_) {
            if (s.nodes_[group].kind_ == kind && same_expression(s, group, index)) {
                return emitted;
            }
        }
        return index;
    }

    template <class range_type, class fn_type>
    void separated(const range_type& range, fn_type&& fn) {
        bool first = true;
        for (const auto& item : range) {
            if (!first) {
                output_.sql_ += ", ";
            }
            first = false;
            fn(item);
        }
    }
    template <class range_type>
    void identifiers(const range_type& range) {
        separated(range, [&](const auto& name) { output_.identifier(name); });
    }
    template <class range_type>
    void expressions(const db_query_storage& s, const range_type& range, std::size_t depth, bool aliases = false) {
        separated(range, [&](std::size_t index) { expr(s, index, depth, aliases); });
    }
    void orders(const db_query_storage& s, const std::vector<db_stored_order>& terms, std::size_t depth) {
        separated(terms, [&](const auto& term) {
            if (!output_.pg() && term.nulls_ != db_nulls_order::default_value) {
                output_.sql_ += '(';
                expr(s, term.expression_, depth);
                output_.sql_ += " IS NULL)";
                output_.sql_ += term.nulls_ == db_nulls_order::first ? " DESC, " : " ASC, ";
            }
            expr(s, term.expression_, depth);
            output_.sql_ += term.direction_ == db_order_direction::asc ? " ASC" : " DESC";
            if (output_.pg()) {
                if (term.nulls_ == db_nulls_order::first) {
                    output_.sql_ += " NULLS FIRST";
                }
                if (term.nulls_ == db_nulls_order::last) {
                    output_.sql_ += " NULLS LAST";
                }
            }
        });
    }
    void boundary(const db_window_boundary& boundary) {
        if (boundary.kind_ != db_frame_boundary::preceding && boundary.kind_ != db_frame_boundary::following && boundary.offset_.value() != 0) {
            throw std::invalid_argument("a non-offset window frame boundary has an offset");
        }
        switch (boundary.kind_) {
            case db_frame_boundary::unbounded_preceding:
                output_.sql_ += "UNBOUNDED PRECEDING";
                break;
            case db_frame_boundary::preceding:
                append_db_number(output_.sql_, boundary.offset_.value());
                output_.sql_ += " PRECEDING";
                break;
            case db_frame_boundary::current_row:
                output_.sql_ += "CURRENT ROW";
                break;
            case db_frame_boundary::following:
                append_db_number(output_.sql_, boundary.offset_.value());
                output_.sql_ += " FOLLOWING";
                break;
            case db_frame_boundary::unbounded_following:
                output_.sql_ += "UNBOUNDED FOLLOWING";
                break;
        }
    }
    // Imports own their nodes. Equal grouped expressions must nevertheless use
    // the same PostgreSQL parameter nodes in SELECT, GROUP BY and HAVING.
    static bool same_expression(const db_query_storage& s, std::size_t lhs, std::size_t rhs, std::size_t depth = 0) {
        if (lhs == rhs) {
            return true;
        }
        if (lhs == no_db_node || rhs == no_db_node || depth > max_db_query_depth) {
            return false;
        }
        const auto& a = s.nodes_.at(lhs);
        const auto& b = s.nodes_.at(rhs);
        if (a.kind_ != b.kind_ || a.text_ != b.text_ || a.qualifier_ != b.qualifier_ || a.binary_ != b.binary_ ||
            a.unary_ != b.unary_ || a.date_part_ != b.date_part_ || a.flag_ != b.flag_ ||
            a.query_ != no_db_node || b.query_ != no_db_node || a.args_.size() != b.args_.size() ||
            a.named_.size() != b.named_.size() || a.orders_.size() != b.orders_.size() ||
            a.type_.data_type_ != b.type_.data_type_ || a.type_.custom_name_ != b.type_.custom_name_ ||
            a.type_.length_ != b.type_.length_ || a.type_.precision_ != b.type_.precision_ || a.type_.scale_ != b.type_.scale_ ||
            a.type_.array_ != b.type_.array_ || a.frame_.has_value() != b.frame_.has_value()) {
            return false;
        }
        if (a.frame_ && (a.frame_->kind_ != b.frame_->kind_ || a.frame_->start_.kind_ != b.frame_->start_.kind_ ||
                            a.frame_->start_.offset_ != b.frame_->start_.offset_ || a.frame_->end_.kind_ != b.frame_->end_.kind_ ||
                            a.frame_->end_.offset_ != b.frame_->end_.offset_)) {
            return false;
        }
        if (a.kind_ == db_node_kind::value && !same_db_value(a.value_, b.value_)) {
            return false;
        }
        const auto same = [&](std::size_t x, std::size_t y) { return same_expression(s, x, y, depth + 1); };
        if (!same(a.left_, b.left_) || !same(a.right_, b.right_)) {
            return false;
        }
        for (std::size_t i = 0; i < a.args_.size(); ++i) {
            if (!same(a.args_[i], b.args_[i])) {
                return false;
            }
        }
        for (std::size_t i = 0; i < a.named_.size(); ++i) {
            if (a.named_[i].name_ != b.named_[i].name_ || !same(a.named_[i].expression_, b.named_[i].expression_)) {
                return false;
            }
        }
        for (std::size_t i = 0; i < a.orders_.size(); ++i) {
            if (a.orders_[i].direction_ != b.orders_[i].direction_ || a.orders_[i].nulls_ != b.orders_[i].nulls_ ||
                !same(a.orders_[i].expression_, b.orders_[i].expression_)) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] static bool contains_bound_value(
        const db_query_storage& s, std::size_t index, std::size_t depth = 0) noexcept {
        if (index >= s.nodes_.size() || depth > max_db_query_depth) {
            return false;
        }
        const auto& n = s.nodes_[index];
        switch (n.kind_) {
            case db_node_kind::column:
            case db_node_kind::star:
            case db_node_kind::default_value:
            case db_node_kind::excluded:
                return false;
            case db_node_kind::value:
                return n.value_.type() != db_value_type::null;
            default:
                break;
        }
        if (contains_bound_value(s, n.left_, depth + 1) ||
            contains_bound_value(s, n.right_, depth + 1)) {
            return true;
        }
        for (const auto arg : n.args_) {
            if (contains_bound_value(s, arg, depth + 1)) {
                return true;
            }
        }
        for (const auto& named : n.named_) {
            if (contains_bound_value(s, named.expression_, depth + 1)) {
                return true;
            }
        }
        for (const auto& order : n.orders_) {
            if (contains_bound_value(s, order.expression_, depth + 1)) {
                return true;
            }
        }
        return false;
    }
    // `cast` applies when the expression is a bound value; see pg_parameter_cast.
    void expr(const db_query_storage& s, std::size_t index, std::size_t depth, bool aliases = false,
        pg_parameter_cast cast = pg_parameter_cast::context) {
        require_depth(depth);
        index = grouped_node(s, index);
        const auto& n = s.nodes_.at(index);
        const auto child_value = [&](std::size_t id) { expr(s, id, depth + 1); };
        switch (n.kind_) {
            case db_node_kind::sql:
                if (output_.pg() && mode_ == db_parameter_mode::bound &&
                    (contains_pg_positional_parameter(n.text_) ||
                        std::ranges::any_of(n.named_, [](const auto& part) { return contains_pg_positional_parameter(part.name_); }))) {
                    // It would silently address the compiler's own numbering.
                    throw std::invalid_argument("raw SQL cannot contain PostgreSQL positional parameters; pass value() arguments");
                }
                output_.sql_ += '(';
                for (const auto& part : n.named_) {
                    output_.sql_ += part.name_;
                    // A following negative literal or `*` would otherwise
                    // start a `--` line comment or a `/*` block comment.
                    if (!part.name_.empty() && (part.name_.back() == '-' || part.name_.back() == '/')) {
                        output_.sql_ += ' ';
                    }
                    child_value(part.expression_);
                }
                output_.sql_ += n.text_;
                output_.sql_ += ')';
                break;
            case db_node_kind::column:
            case db_node_kind::star:
                if (!n.qualifier_.empty()) {
                    output_.qualified(n.qualifier_);
                    output_.sql_ += '.';
                }
                if (n.kind_ == db_node_kind::star) {
                    output_.sql_ += '*';
                } else {
                    output_.identifier(n.text_);
                }
                break;
            case db_node_kind::value:
                output_.bound_node(s, index, cast);
                break;
            case db_node_kind::default_value:
                output_.sql_ += "DEFAULT";
                break;
            case db_node_kind::excluded:
                if (output_.pg()) {
                    output_.sql_ += "excluded.";
                    output_.identifier(n.text_);
                } else {
                    output_.sql_ += "VALUES(";
                    output_.identifier(n.text_);
                    output_.sql_ += ')';
                }
                break;
            case db_node_kind::function:
            case db_node_kind::aggregate:
            case db_node_kind::coalesce:
            case db_node_kind::null_if:
            case db_node_kind::greatest:
            case db_node_kind::least: {
                switch (n.kind_) {
                    case db_node_kind::coalesce:
                        output_.sql_ += "COALESCE";
                        break;
                    case db_node_kind::null_if:
                        output_.sql_ += "NULLIF";
                        break;
                    case db_node_kind::greatest:
                        output_.sql_ += "GREATEST";
                        break;
                    case db_node_kind::least:
                        output_.sql_ += "LEAST";
                        break;
                    default:
                        output_.function_name(n.text_);
                        break;
                }
                output_.sql_ += '(';
                if (n.kind_ == db_node_kind::aggregate && n.flag_) {
                    output_.sql_ += "DISTINCT ";
                }
                // COALESCE, NULLIF, GREATEST and LEAST take their type from
                // their arguments; with only bound values there is none.
                const bool only_values = std::ranges::all_of(
                    n.args_, [&](std::size_t arg) { return s.nodes_.at(arg).kind_ == db_node_kind::value; });
                for (std::size_t position = 0; position < n.args_.size(); ++position) {
                    if (position != 0) {
                        output_.sql_ += ", ";
                    }
                    auto argument_cast = pg_parameter_cast::context;
                    if (output_.pg()) {
                        if (n.kind_ == db_node_kind::function || n.kind_ == db_node_kind::aggregate) {
                            argument_cast = pg_function_parameter_cast(n.text_, position);
                        } else if (only_values) {
                            argument_cast = pg_parameter_cast::value_category;
                        }
                    }
                    expr(s, n.args_[position], depth + 1, false, argument_cast);
                }
                if (!n.named_.empty()) {
                    output_.require_pg("named function arguments");
                    if (!n.args_.empty()) {
                        output_.sql_ += ", ";
                    }
                    separated(n.named_, [&](const auto& arg) { output_.identifier(arg.name_); output_.sql_ += " => "; child_value(arg.expression_); });
                }
                if (!n.orders_.empty()) {
                    output_.require_pg("ordered aggregate");
                    output_.sql_ += " ORDER BY ";
                    orders(s, n.orders_, depth + 1);
                }
                output_.sql_ += ')';
                break;
            }
            case db_node_kind::binary:
                binary(s, n, depth + 1);
                break;
            case db_node_kind::unary: {
                // Negation retains the value category; bitwise NOT selects an
                // integer type for bare integer operands. Null predicates take
                // any category. NOT and IS TRUE/FALSE coerce theirs to boolean.
                const auto& operand = s.nodes_.at(n.left_);
                auto operand_cast = pg_parameter_cast::context;
                if (output_.pg()) {
                    if (n.unary_ == db_unary_operator::negate || n.unary_ == db_unary_operator::bit_not) {
                        operand_cast = pg_operator_operand_cast(operand,
                            n.unary_ == db_unary_operator::bit_not ? pg_parameter_cast::bitwise_integer : pg_parameter_cast::value_category);
                    } else if ((n.unary_ == db_unary_operator::is_null ||
                                   n.unary_ == db_unary_operator::is_not_null) &&
                               is_untyped_value(operand)) {
                        operand_cast = pg_parameter_cast::value_category;
                    }
                }
                output_.sql_ += '(';
                switch (n.unary_) {
                    case db_unary_operator::not_value:
                        output_.sql_ += "NOT ";
                        child_value(n.left_);
                        break;
                    case db_unary_operator::negate:
                        // Keep the operator apart from its operand: a negative
                        // literal would otherwise render "--5", which
                        // PostgreSQL lexes as a line comment, and "~-5", which
                        // it lexes as the single operator "~-".
                        output_.sql_ += "- ";
                        expr(s, n.left_, depth + 1, false, operand_cast);
                        break;
                    case db_unary_operator::bit_not:
                        output_.sql_ += "~ ";
                        expr(s, n.left_, depth + 1, false, operand_cast);
                        break;
                    default:
                        expr(s, n.left_, depth + 1, false, operand_cast);
                        switch (n.unary_) {
                            case db_unary_operator::is_null:
                                output_.sql_ += " IS NULL";
                                break;
                            case db_unary_operator::is_not_null:
                                output_.sql_ += " IS NOT NULL";
                                break;
                            case db_unary_operator::is_true:
                                output_.sql_ += " IS TRUE";
                                break;
                            case db_unary_operator::is_false:
                                output_.sql_ += " IS FALSE";
                                break;
                            case db_unary_operator::is_not_true:
                                output_.sql_ += " IS NOT TRUE";
                                break;
                            case db_unary_operator::is_not_false:
                                output_.sql_ += " IS NOT FALSE";
                                break;
                            default:
                                std::terminate();
                        }
                }
                output_.sql_ += ')';
                break;
            }
            case db_node_kind::between:
                output_.sql_ += '(';
                child_value(n.args_[0]);
                output_.sql_ += n.flag_ ? " NOT BETWEEN " : " BETWEEN ";
                child_value(n.args_[1]);
                output_.sql_ += " AND ";
                child_value(n.args_[2]);
                output_.sql_ += ')';
                break;
            case db_node_kind::tuple:
            case db_node_kind::list:
            case db_node_kind::array:
                if (n.kind_ == db_node_kind::array) {
                    output_.require_pg("SQL arrays");
                    output_.sql_ += "ARRAY[";
                } else {
                    if (n.args_.empty()) {
                        throw std::invalid_argument("empty SQL tuple or list");
                    }
                    output_.sql_ += n.kind_ == db_node_kind::tuple ? "ROW(" : "(";
                }
                expressions(s, n.args_, depth + 1);
                output_.sql_ += n.kind_ == db_node_kind::array ? ']' : ')';
                break;
            case db_node_kind::any:
            case db_node_kind::all:
                output_.require_pg("array ANY/ALL");
                output_.sql_ += n.kind_ == db_node_kind::any ? "ANY(" : "ALL(";
                child_value(n.left_);
                output_.sql_ += ')';
                break;
            case db_node_kind::cast:
                output_.sql_ += "CAST(";
                child_value(n.left_);
                output_.sql_ += " AS ";
                output_.type(n.type_);
                output_.sql_ += ')';
                break;
            case db_node_kind::alias:
                if (!aliases) {
                    throw std::invalid_argument("an alias is only valid in a projection or RETURNING");
                }
                child_value(n.left_);
                output_.sql_ += " AS ";
                output_.identifier(n.text_);
                break;
            case db_node_kind::case_value:
                output_.sql_ += "CASE";
                for (std::size_t i = 0; i < n.args_.size(); i += 2) {
                    output_.sql_ += " WHEN ";
                    child_value(n.args_[i]);
                    output_.sql_ += " THEN ";
                    child_value(n.args_[i + 1]);
                }
                if (n.left_ != no_db_node) {
                    output_.sql_ += " ELSE ";
                    child_value(n.left_);
                }
                output_.sql_ += " END";
                break;
            case db_node_kind::exists:
            case db_node_kind::subquery:
                if (n.kind_ == db_node_kind::exists) {
                    output_.sql_ += "EXISTS ";
                }
                output_.sql_ += '(';
                query(nested(s, n.query_), depth + 1);
                output_.sql_ += ')';
                break;
            case db_node_kind::filter:
                output_.require_pg("aggregate FILTER");
                child_value(n.left_);
                output_.sql_ += " FILTER (WHERE ";
                child_value(n.right_);
                output_.sql_ += ')';
                break;
            case db_node_kind::window: {
                child_value(n.left_);
                output_.sql_ += " OVER (";
                bool space = false;
                if (!n.args_.empty()) {
                    output_.sql_ += "PARTITION BY ";
                    expressions(s, n.args_, depth + 1);
                    space = true;
                }
                if (!n.orders_.empty()) {
                    if (space) {
                        output_.sql_ += ' ';
                    }
                    output_.sql_ += "ORDER BY ";
                    orders(s, n.orders_, depth + 1);
                    space = true;
                }
                if (n.frame_) {
                    if (space) {
                        output_.sql_ += ' ';
                    }
                    const auto& frame = *n.frame_;
                    if (frame.start_.kind_ == db_frame_boundary::unbounded_following || frame.end_.kind_ == db_frame_boundary::unbounded_preceding || frame.start_.kind_ > frame.end_.kind_) {
                        throw std::invalid_argument("invalid window frame boundaries");
                    }
                    switch (frame.kind_) {
                        case db_window_frame::rows:
                            output_.sql_ += "ROWS";
                            break;
                        case db_window_frame::range:
                            output_.sql_ += "RANGE";
                            break;
                        case db_window_frame::groups:
                            output_.require_pg("GROUPS window frame");
                            output_.sql_ += "GROUPS";
                            break;
                    }
                    output_.sql_ += " BETWEEN ";
                    boundary(frame.start_);
                    output_.sql_ += " AND ";
                    boundary(frame.end_);
                }
                output_.sql_ += ')';
                break;
            }
            case db_node_kind::within_group:
                output_.require_pg("ordered-set aggregate");
                child_value(n.left_);
                output_.sql_ += " WITHIN GROUP (ORDER BY ";
                orders(s, n.orders_, depth + 1);
                output_.sql_ += ')';
                break;
            case db_node_kind::extract: {
                const auto& spec = date_part_sql_specs.at(static_cast<std::size_t>(n.date_part_));
                if (!spec.pg_feature_.empty()) {
                    output_.require_pg(spec.pg_feature_);
                }
                output_.sql_ += "EXTRACT(";
                output_.sql_ += spec.token_;
                output_.sql_ += " FROM ";
                child_value(n.left_);
                output_.sql_ += ')';
                break;
            }
            case db_node_kind::subscript:
                output_.require_pg("array subscript");
                output_.sql_ += '(';
                child_value(n.left_);
                output_.sql_ += ")[";
                child_value(n.right_);
                output_.sql_ += ']';
                break;
            case db_node_kind::collate:
                output_.sql_ += '(';
                child_value(n.left_);
                output_.sql_ += " COLLATE ";
                output_.qualified(n.text_);
                output_.sql_ += ')';
                break;
            case db_node_kind::at_time_zone: {
                output_.require_pg("AT TIME ZONE");
                const auto& zone = s.nodes_.at(n.right_);
                const auto zone_cast = is_untyped_value(zone) ? pg_parameter_cast::value_category : pg_parameter_cast::context;
                output_.sql_ += '(';
                child_value(n.left_);
                output_.sql_ += " AT TIME ZONE ";
                expr(s, n.right_, depth + 1, false, zone_cast);
                output_.sql_ += ')';
                break;
            }
        }
    }
    // Emits a left-deep AND/OR chain flat, `(a AND b AND c)`, at one nesting
    // level. A link that repeats a grouped expression goes through expr().
    void logical_chain(const db_query_storage& s, const db_query_node& n, std::size_t depth) {
        std::vector<const db_query_node*> spine;
        spine.push_back(&n);
        while (continues_logical_chain(s.nodes_, *spine.back(), spine.back()->left_) &&
               grouped_node(s, spine.back()->left_) == spine.back()->left_) {
            spine.push_back(&s.nodes_[spine.back()->left_]);
        }
        const std::string_view token = n.binary_ == db_binary_operator::and_value ? " AND " : " OR ";
        output_.sql_ += '(';
        expr(s, spine.back()->left_, depth);
        for (auto link = spine.rbegin(); link != spine.rend(); ++link) {
            output_.sql_ += token;
            expr(s, (*link)->right_, depth);
        }
        output_.sql_ += ')';
    }
    void binary(const db_query_storage& s, const db_query_node& n, std::size_t depth) {
        if (continues_logical_chain(s.nodes_, n, n.left_) && grouped_node(s, n.left_) == n.left_) {
            logical_chain(s, n, depth);
            return;
        }
        const auto& left = s.nodes_.at(n.left_);
        const auto& right = s.nodes_.at(n.right_);
        if ((n.binary_ == db_binary_operator::in || n.binary_ == db_binary_operator::not_in) && right.kind_ == db_node_kind::list && right.args_.empty()) {
            output_.sql_ += n.binary_ == db_binary_operator::in ? "FALSE" : "TRUE";
            return;
        }
        if (n.binary_ == db_binary_operator::concat && !output_.pg()) {
            output_.sql_ += "CONCAT(";
            expr(s, n.left_, depth);
            output_.sql_ += ", ";
            expr(s, n.right_, depth);
            output_.sql_ += ')';
            return;
        }
        const bool maria_distinct = !output_.pg() && n.binary_ == db_binary_operator::is_distinct_from;
        output_.sql_ += '(';
        if (maria_distinct) {
            output_.sql_ += "NOT (";
        }
        auto left_cast = pg_parameter_cast::context;
        auto right_cast = pg_parameter_cast::context;
        if (output_.pg()) {
            // Preserve the other operand's SQL type. Only arithmetic over two
            // bare values needs an explicit choice of overload.
            const auto operand_cast = pg_binary_operand_cast(n.binary_);
            if (operand_cast != pg_parameter_cast::context && is_untyped_value(left) && is_untyped_value(right)) {
                left_cast = pg_operator_operand_cast(left, operand_cast);
                right_cast = pg_operator_operand_cast(right, operand_cast);
            }
            // jsonb -> n, ->> n and - n index arrays with int4 only; untyped,
            // an integer would be read as a text key.
            if (n.binary_ == db_binary_operator::json_get || n.binary_ == db_binary_operator::json_get_text ||
                n.binary_ == db_binary_operator::json_delete) {
                right_cast = pg_parameter_cast::int4;
            }
        }
        expr(s, n.left_, depth, false, left_cast);
        const auto op_index = static_cast<std::size_t>(n.binary_);
        if (op_index >= binary_operator_sql_specs.size()) {
            std::terminate();
        }
        const auto& op_spec = binary_operator_sql_specs[op_index];
        if (!op_spec.pg_feature_.empty()) {
            output_.require_pg(op_spec.pg_feature_);
        }
        const auto token = (!output_.pg() && !op_spec.maria_token_.empty()) ? op_spec.maria_token_ : op_spec.pg_token_;
        if ((n.binary_ == db_binary_operator::in || n.binary_ == db_binary_operator::not_in) && right.kind_ != db_node_kind::list && right.kind_ != db_node_kind::subquery) {
            throw std::invalid_argument("IN requires a list or a subquery");
        }
        output_.sql_ += ' ';
        output_.sql_ += token;
        output_.sql_ += ' ';
        expr(s, n.right_, depth, false, right_cast);
        if (maria_distinct) {
            output_.sql_ += ')';
        }
        output_.sql_ += ')';
    }
    void source(const db_query_storage& s, const db_query_source& value, std::size_t depth) {
        if (value.lateral_) {
            output_.require_pg("LATERAL");
            output_.sql_ += "LATERAL ";
        }
        switch (value.kind_) {
            case db_source_kind::table:
                output_.qualified(value.name_);
                break;
            case db_source_kind::query:
                output_.sql_ += '(';
                query(nested(s, value.query_), depth + 1);
                output_.sql_ += ')';
                break;
            case db_source_kind::function:
                output_.require_pg("table function");
                expr(s, value.expression_, depth + 1);
                break;
        }
        if (value.ordinality_) {
            output_.require_pg("WITH ORDINALITY");
            if (value.kind_ != db_source_kind::function) {
                throw std::invalid_argument("WITH ORDINALITY requires a table function");
            }
            output_.sql_ += " WITH ORDINALITY";
        }
        if (!value.alias_.empty()) {
            output_.sql_ += " AS ";
            output_.identifier(value.alias_);
        }
        if (!value.columns_.empty()) {
            if (value.alias_.empty()) {
                throw std::invalid_argument("source column names require an alias");
            }
            output_.sql_ += " (";
            const bool typed = value.columns_.front().type_.data_type_ != db_data_type::inferred || !value.columns_.front().type_.custom_name_.empty();
            if (typed && (value.kind_ != db_source_kind::function || value.ordinality_)) {
                throw std::invalid_argument("record definitions require a function without ordinality");
            }
            separated(value.columns_, [&](const auto& column) {
                const bool has_type = column.type_.data_type_ != db_data_type::inferred || !column.type_.custom_name_.empty();
                if (has_type != typed) {
                    throw std::invalid_argument("source column definitions must all specify a type or all omit it");
                }
                output_.identifier(column.name_);
                if (typed) {
                    output_.sql_ += ' ';
                    const auto& t = column.type_;
                    append_db_type_name(output_.sql_, t.data_type_, t.custom_name_, t.length_, t.precision_, t.scale_, t.array_, output_.driver());
                }
            });
            output_.sql_ += ')';
        }
    }
    void sources(const db_query_storage& s, std::size_t depth) {
        source(s, *s.source_, depth);
        for (const auto& join : s.joins_) {
            switch (join.type_) {
                case db_join_type::inner:
                    output_.sql_ += " INNER JOIN ";
                    break;
                case db_join_type::left:
                    output_.sql_ += " LEFT JOIN ";
                    break;
                case db_join_type::right:
                    output_.sql_ += " RIGHT JOIN ";
                    break;
                case db_join_type::full:
                    output_.require_pg("FULL JOIN");
                    output_.sql_ += " FULL JOIN ";
                    break;
                case db_join_type::cross:
                    output_.sql_ += " CROSS JOIN ";
                    break;
            }
            if (join.type_ == db_join_type::cross) {
                if (join.on_ != no_db_node || !join.using_columns_.empty()) {
                    throw std::invalid_argument("CROSS JOIN has no ON or USING");
                }
            } else if ((join.on_ != no_db_node) == !join.using_columns_.empty()) {
                throw std::invalid_argument("JOIN requires exactly one of ON or USING");
            }
            source(s, join.source_, depth + 1);
            if (join.on_ != no_db_node) {
                output_.sql_ += " ON ";
                expr(s, join.on_, depth + 1);
            }
            if (!join.using_columns_.empty()) {
                output_.sql_ += " USING (";
                identifiers(join.using_columns_);
                output_.sql_ += ')';
            }
        }
    }
    void assignments(const db_query_storage& s, const std::vector<db_stored_assignment>& values, std::size_t depth) {
        if (values.empty()) {
            throw std::invalid_argument("UPDATE requires assignments");
        }
        separated(values, [&](const auto& assignment) { output_.identifier(assignment.column_); output_.sql_ += " = "; expr(s, assignment.expression_, depth + 1); });
    }
    void conflict(const db_query_storage& s, std::size_t depth) {
        const auto& value = *s.conflict_;
        if (value.do_nothing_ && (!value.assignments_.empty() || value.update_where_ != no_db_node)) {
            throw std::invalid_argument("DO NOTHING cannot specify updates");
        }
        if (output_.pg()) {
            if (value.any_unique_key_) {
                throw std::invalid_argument("any_unique_key is specific to MariaDB");
            }
            if (!value.columns_.empty() && !value.constraint_.empty()) {
                throw std::invalid_argument("conflict target cannot have both columns and a constraint");
            }
            if (value.target_where_ != no_db_node && value.columns_.empty()) {
                throw std::invalid_argument("a conflict predicate requires target columns");
            }
            output_.sql_ += " ON CONFLICT";
            if (!value.columns_.empty()) {
                output_.sql_ += " (";
                identifiers(value.columns_);
                output_.sql_ += ')';
            }
            if (!value.constraint_.empty()) {
                output_.sql_ += " ON CONSTRAINT ";
                output_.identifier(value.constraint_);
            }
            if (value.target_where_ != no_db_node) {
                output_.sql_ += " WHERE ";
                expr(s, value.target_where_, depth + 1);
            }
            if (value.do_nothing_) {
                output_.sql_ += " DO NOTHING";
                return;
            }
            if (value.columns_.empty() && value.constraint_.empty()) {
                throw std::invalid_argument("DO UPDATE requires a conflict target");
            }
            output_.sql_ += " DO UPDATE SET ";
            assignments(s, value.assignments_, depth);
            if (value.update_where_ != no_db_node) {
                output_.sql_ += " WHERE ";
                expr(s, value.update_where_, depth + 1);
            }
        } else {
            if (!value.any_unique_key_ || !value.columns_.empty() || !value.constraint_.empty() || value.target_where_ != no_db_node || value.update_where_ != no_db_node || value.do_nothing_) {
                output_.unsupported("targeted or conditional upsert / DO NOTHING");
            }
            output_.sql_ += " ON DUPLICATE KEY UPDATE ";
            assignments(s, value.assignments_, depth);
        }
    }
    void rows(const db_query_storage& s, std::size_t depth) {
        if (s.rows_.empty() || s.rows_.front().empty()) {
            throw std::invalid_argument("VALUES requires nonempty rows");
        }
        const auto width = s.rows_.front().size();
        if (!s.columns_.empty() && width != s.columns_.size()) {
            throw std::invalid_argument("INSERT column and row widths differ");
        }
        output_.sql_ += "VALUES ";
        separated(s.rows_, [&](const auto& row) {
            if (row.size() != width) {
                throw std::invalid_argument("VALUES rows have different widths");
            }
            output_.sql_ += '(';
            expressions(s, row, depth + 1);
            output_.sql_ += ')';
        });
    }
    void body(const db_query_storage& s, std::size_t depth) {
        switch (s.kind_) {
            case db_query_kind::select:
                output_.sql_ += "SELECT ";
                if (s.distinct_) {
                    output_.sql_ += "DISTINCT ";
                }
                if (!s.distinct_on_.empty()) {
                    output_.require_pg("DISTINCT ON");
                    output_.sql_ += "DISTINCT ON (";
                    expressions(s, s.distinct_on_, depth + 1);
                    output_.sql_ += ") ";
                }
                if (s.projections_.empty()) {
                    output_.sql_ += '*';
                } else {
                    expressions(s, s.projections_, depth + 1, true);
                }
                if (s.source_) {
                    output_.sql_ += " FROM ";
                    sources(s, depth + 1);
                }
                break;
            case db_query_kind::values:
                rows(s, depth);
                break;
            case db_query_kind::insert:
                output_.sql_ += "INSERT INTO ";
                output_.qualified(s.target_);
                if (!s.target_alias_.empty()) {
                    output_.require_pg("INSERT target alias");
                    output_.sql_ += " AS ";
                    output_.identifier(s.target_alias_);
                }
                if (!s.columns_.empty()) {
                    output_.sql_ += " (";
                    identifiers(s.columns_);
                    output_.sql_ += ')';
                }
                output_.sql_ += ' ';
                if (s.insert_query_ != no_db_node) {
                    if (!s.rows_.empty()) {
                        throw std::invalid_argument("INSERT cannot have both VALUES and SELECT");
                    }
                    query(nested(s, s.insert_query_), depth + 1);
                } else if (s.rows_.empty() ||
                           (s.columns_.empty() && s.rows_.size() == 1 && s.rows_.front().empty())) {
                    if (!s.columns_.empty()) {
                        throw std::invalid_argument("INSERT column list requires input rows");
                    }
                    output_.sql_ += output_.pg() ? "DEFAULT VALUES" : "() VALUES ()";
                } else {
                    rows(s, depth);
                }
                if (s.conflict_) {
                    conflict(s, depth + 1);
                }
                break;
            case db_query_kind::update:
            case db_query_kind::delete_value:
                output_.sql_ += s.kind_ == db_query_kind::update ? "UPDATE " : "DELETE FROM ";
                output_.qualified(s.target_);
                if (!s.target_alias_.empty()) {
                    if (!output_.pg() && s.kind_ == db_query_kind::delete_value) {
                        output_.unsupported("DELETE target alias");
                    }
                    output_.sql_ += " AS ";
                    output_.identifier(s.target_alias_);
                }
                if (s.kind_ == db_query_kind::update) {
                    output_.sql_ += " SET ";
                    assignments(s, s.assignments_, depth + 1);
                }
                if (s.source_) {
                    output_.require_pg("UPDATE FROM / DELETE USING");
                    output_.sql_ += s.kind_ == db_query_kind::update ? " FROM " : " USING ";
                    sources(s, depth + 1);
                }
                break;
        }
        if (s.predicate_ != no_db_node) {
            output_.sql_ += " WHERE ";
            expr(s, s.predicate_, depth + 1);
        }
        if (!s.groups_.empty()) {
            output_.sql_ += " GROUP BY ";
            expressions(s, s.groups_, depth + 1);
        }
        if (s.having_ != no_db_node) {
            output_.sql_ += " HAVING ";
            expr(s, s.having_, depth + 1);
        }
    }

    grouped_bound_values grouped_;
};
void append_db_expression(std::string& output, db_expression expression, db_driver driver) {
    if (expression.empty()) {
        throw std::invalid_argument("empty database expression");
    }
    db_query_compiler compiler(driver, db_parameter_mode::literal);
    const auto initial_size = output.size();
    compiler.output_.sql_ = std::move(output);
    try {
        if (compiler.output_.sql_.capacity() - initial_size < 32U) {
            compiler.output_.sql_.reserve(initial_size + 64U);
        }
        compiler.expression(expression);
        output = std::move(compiler.output_.sql_);
    } catch (...) {
        compiler.output_.sql_.resize(initial_size);
        output = std::move(compiler.output_.sql_);
        throw;
    }
}

}  // namespace ruvia::detail

namespace ruvia {

db_statement db_query::compile(db_driver driver, db_parameter_mode mode) const {
    detail::db_query_compiler compiler(driver, mode);
    compiler.query(storage());
    return db_statement(std::move(compiler.output_.sql_), std::move(compiler.output_.params_), returns_rows());
}

std::string db_query::render_expression(db_expression expression, db_driver driver) {
    std::string output;
    detail::append_db_expression(output, expression, driver);
    return output;
}

}  // namespace ruvia
