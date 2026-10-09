#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ruvia/web/db/db_query.h"
#include "ruvia/web/detail/db/db_entity_access.h"
#include "ruvia/web/detail/db/db_entity_codec.h"
#include "ruvia/web/detail/db/db_relation_metadata.h"

namespace ruvia::detail {

// This plan belongs to a builder or a single cold operation. It owns every
// selected path and result-column name; it never retains an AST or row view.
class db_relation_plan final {
public:
    static constexpr std::size_t root = std::numeric_limits<std::size_t>::max();
    struct node_type final {
        explicit node_type(std::pmr::memory_resource* resource)
            : path_(resource),
              alias_(resource),
              columns_(resource) {}
        node_type(const node_type& other, std::pmr::memory_resource* resource)
            : parent_(other.parent_),
              relation_(other.relation_),
              offset_(other.offset_),
              join_(other.join_),
              path_(other.path_, resource),
              alias_(other.alias_, resource),
              columns_(other.columns_, resource) {}
        std::size_t parent_{root};
        std::size_t relation_{0};
        std::size_t offset_{0};
        db_join_type join_{db_join_type::left};
        std::pmr::string path_;
        std::pmr::string alias_;
        std::pmr::vector<std::pmr::string> columns_;
    };

    explicit db_relation_plan(std::pmr::memory_resource* resource)
        : resource_(pmr_resource_or_default(resource)),
          nodes_(resource_) {}
    db_relation_plan(const db_relation_plan&) = delete;
    db_relation_plan& operator=(const db_relation_plan&) = delete;
    db_relation_plan(db_relation_plan&&) noexcept = default;
    db_relation_plan& operator=(db_relation_plan&&) = delete;

    [[nodiscard]] db_relation_plan clone() const {
        db_relation_plan result(resource_);
        result.column_count_ = column_count_;
        result.next_alias_ = next_alias_;
        result.next_projection_ = next_projection_;
        for (const auto& node : nodes_) {
            result.nodes_.emplace_back(node, resource_);
        }
        return result;
    }
    [[nodiscard]] bool empty() const noexcept {
        return nodes_.empty();
    }
    [[nodiscard]] std::size_t column_count() const noexcept {
        return column_count_;
    }
    [[nodiscard]] std::span<const node_type> nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] std::size_t find(std::size_t parent_value, std::size_t relation) const noexcept {
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].parent_ == parent_value && nodes_[i].relation_ == relation) {
                return i;
            }
        }
        return root;
    }

    template <typename entity_type>
    void add(db_query& query, std::string_view root_alias, std::string_view path,
        std::string_view alias = {}, db_join_type join = db_join_type::left) {
        if constexpr (db_primary_key_count<entity_type>() == 0) {
            throw std::invalid_argument("relation loading requires a primary key on the root entity");
        } else {
            if (alias.find('.') != std::string_view::npos || alias.find('\0') != std::string_view::npos) {
                throw std::invalid_argument("a relation join alias must be one identifier");
            }
            auto normalized = normalize(root_alias, path);
            validate_path<entity_type>(normalized, 0);
            if (!alias.empty() && query.uses_source_name(alias)) {
                bool same = false;
                for (const auto& node : nodes_) {
                    same |= node.path_ == normalized && node.alias_ == alias && node.join_ == join;
                }
                if (!same) {
                    throw std::invalid_argument("relation join alias is already in use");
                }
            }
            if (nodes_.empty()) {
                column_count_ = std::tuple_size_v<typename entity_type::columns_type>;
            }
            add_path<entity_type>(query, root_alias, normalized, root, alias, join);
        }
    }

    template <typename entity_type>
    [[nodiscard]] std::optional<db_query> prepare(const db_query& query,
        std::string_view root_alias, db_driver driver) const {
        constexpr auto keys = db_primary_key_columns<entity_type>();
        return query.prepare_entity_read(keys, root_alias, driver);
    }

private:
    std::pmr::string normalize(std::string_view root_alias, std::string_view path) const {
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

    template <typename entity_type>
    static void validate_path(std::string_view path, std::size_t depth) {
        if (path.empty() || depth >= 256) {
            throw std::invalid_argument("relation path is empty or exceeds query nesting depth");
        }
        const auto dot = path.find('.');
        const auto name = path.substr(0, dot);
        const auto rest = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
        if (name.empty() || (dot != std::string_view::npos && rest.empty())) {
            throw std::invalid_argument("relation path contains an empty component");
        }
        bool found = false;
        for_each_db_descriptor<typename entity_type::relations_type>([&]<typename relation_type, std::size_t> {
            if (name != relation_type::name.view()) {
                return;
            }
            found = true;
            validate_db_relation<entity_type, relation_type>();
            using target_type = typename relation_type::target_entity_type;
            if constexpr (db_primary_key_count<target_type>() == 0) {
                throw std::invalid_argument("relation loading requires a primary key on every selected target");
            } else if (!rest.empty()) {
                validate_path<target_type>(rest, depth + 1);
            }
        });
        if (!found) {
            throw std::invalid_argument("unknown database entity relation");
        }
    }

    std::pmr::string unique_alias(const db_query& query, std::string_view reserved) {
        for (;;) {
            std::pmr::string alias("__ruvia_relation_", resource_);
            append_db_number(alias, static_cast<std::uint64_t>(next_alias_++));
            if (alias != reserved && !query.uses_source_name(alias)) {
                return alias;
            }
        }
    }
    std::pmr::string unique_projection(const db_query& query) {
        for (;;) {
            std::pmr::string name("__ruvia_column_", resource_);
            append_db_number(name, static_cast<std::uint64_t>(next_projection_++));
            if (!query.uses_projection_name(name)) {
                return name;
            }
        }
    }
    static void conjunction(db_query& query, db_expression& predicate, db_expression term) {
        predicate = predicate.empty() ? term : query.binary(predicate, db_binary_operator::and_value, term);
    }

    template <typename entity_type, typename relation_type>
    void append_join(db_query& query, std::string_view source_alias, node_type& node_value,
        std::string_view reserved_alias) {
        using mapping_type = db_relation_mapping<entity_type, relation_type>;
        using target_type = typename relation_type::target_entity_type;
        db_expression predicate;
        if constexpr (mapping_type::through_join_table) {
            auto junction = unique_alias(query, node_value.alias_);
            // A terminal user alias is reserved even while constructing an
            // ancestor's automatic junction alias.
            if (junction == reserved_alias) {
                junction = unique_alias(query, reserved_alias);
            }
            for_each_db_descriptor<typename mapping_type::source_columns_type>([&]<typename column_type, std::size_t> {
                conjunction(query, predicate, query.binary(query.column(column_type::referenced.view(), source_alias), db_binary_operator::equal, query.column(column_type::local.view(), junction)));
            });
            query.join(node_value.join_, mapping_type::table_name(), predicate, junction);
            predicate = {};
            for_each_db_descriptor<typename mapping_type::target_columns_type>([&]<typename column_type, std::size_t> {
                conjunction(query, predicate, query.binary(query.column(column_type::local.view(), junction), db_binary_operator::equal, query.column(column_type::referenced.view(), node_value.alias_)));
            });
        } else {
            for_each_db_descriptor<typename mapping_type::join_columns_type>([&]<typename column_type, std::size_t> {
                conjunction(query, predicate, query.binary(query.column(column_type::local.view(), source_alias), db_binary_operator::equal, query.column(column_type::referenced.view(), node_value.alias_)));
            });
        }
        query.join(node_value.join_, target_type::table_name(), predicate, node_value.alias_);
        for_each_db_descriptor<typename target_type::columns_type>([&]<typename column_type, std::size_t> {
            auto name = unique_projection(query);
            query.add_select(query.alias(query.column(column_type::name.view(), node_value.alias_), name));
            node_value.columns_.push_back(std::move(name));
        });
    }

    template <typename entity_type>
    void add_path(db_query& query, std::string_view root_alias, std::string_view path,
        std::size_t parent_value, std::string_view requested_alias, db_join_type requested_join) {
        const auto dot = path.find('.');
        const auto name = path.substr(0, dot);
        const auto rest = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
        for_each_db_descriptor<typename entity_type::relations_type>([&]<typename relation_type, std::size_t i> {
            if (name != relation_type::name.view()) {
                return;
            }
            auto index = find(parent_value, i);
            if (index == root) {
                node_type node(resource_);
                node.parent_ = parent_value;
                node.relation_ = i;
                node.offset_ = column_count_;
                node.join_ = rest.empty() ? requested_join : db_join_type::left;
                if (parent_value != root) {
                    node.path_ = nodes_[parent_value].path_;
                    node.path_.push_back('.');
                }
                node.path_.append(name);
                node.alias_ = rest.empty() && !requested_alias.empty()
                                  ? std::pmr::string(requested_alias, resource_)
                                  : unique_alias(query, requested_alias);
                const auto source_alias = parent_value == root ? root_alias : std::string_view(nodes_[parent_value].alias_);
                append_join<entity_type, relation_type>(query, source_alias, node, requested_alias);
                column_count_ += node.columns_.size();
                index = nodes_.size();
                nodes_.push_back(std::move(node));
            } else if (rest.empty() && ((!requested_alias.empty() && nodes_[index].alias_ != requested_alias) || nodes_[index].join_ != requested_join)) {
                throw std::invalid_argument("relation path was already loaded with another alias or join kind");
            }
            if (!rest.empty()) {
                add_path<typename relation_type::target_entity_type>(query, root_alias, rest, index, requested_alias, requested_join);
            }
        });
    }

    std::pmr::memory_resource* resource_;
    std::pmr::vector<node_type> nodes_;
    std::size_t column_count_{0};
    std::size_t next_alias_{0};
    std::size_t next_projection_{0};
};

struct db_graph_identity final {
    std::size_t index_{0};
    std::size_t scope_{0};
};

// Indices, rather than pointers into result vectors, remain valid when a
// collection grows. The identity table is temporary operation storage; the
// resulting entities own their nested values without retaining this table.
class db_relation_decoder final {
public:
    db_relation_decoder(const db_relation_plan& plan, std::pmr::memory_resource* resource)
        : plan_(plan),
          resource_(pmr_resource_or_default(resource)),
          columns_(resource_),
          identities_(resource_),
          key_(resource_) {}

    template <typename entity_type>
    entity_rows<entity_type> decode(const db_rows& rows) {
        entity_rows<entity_type> result(resource_);
        if (rows.empty()) {
            return result;
        }
        resolve_columns<entity_type>(rows.front());
        for (const auto& row : rows) {
            if (!make_key<entity_type>(row, 0, 0, db_relation_plan::root)) {
                throw std::invalid_argument("NULL root primary key in a relation query");
            }
            auto position = identities_.find(key_);
            db_graph_identity identity;
            if (position == identities_.end()) {
                identity = {result.size(), next_scope_++};
                entity_type entity(resource_);
                decode_columns(entity, row, 0);
                result.push_back(std::move(entity));
                identities_.emplace(key_, identity);
            } else {
                identity = position->second;
            }
            decode_relations(result[identity.index_], row, db_relation_plan::root, identity.scope_);
        }
        return result;
    }

private:
    template <typename entity_type>
    void resolve_columns(const db_row& row) {
        columns_.resize(plan_.column_count());
        const auto names = db_result_access::column_names(row);
        const auto resolve = [&](std::string_view name) {
            std::size_t found = names.size();
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (names[i] == name) {
                    if (found != names.size()) {
                        throw std::invalid_argument("duplicate entity projection name in a relation query");
                    }
                    found = i;
                }
            }
            if (found == names.size() || found >= row.size()) {
                throw std::invalid_argument("relation query must select every mapped entity column");
            }
            return found;
        };
        for_each_db_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t i> {
            columns_[i] = resolve(column_type::name.view());
        });
        for (const auto& node : plan_.nodes()) {
            for (std::size_t i = 0; i < node.columns_.size(); ++i) {
                columns_[node.offset_ + i] = resolve(node.columns_[i]);
            }
        }
    }

    template <typename entity_type>
    void decode_columns(entity_type& entity, const db_row& row, std::size_t offset) const {
        for_each_db_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t i> {
            decode_entity_field<entity_type, column_type>(entity, row[columns_[offset + i]], resource_);
        });
    }

    template <typename entity_type>
    bool make_key(const db_row& row, std::size_t offset, std::size_t owner_value, std::size_t relation) {
        key_.clear();
        append_db_number(key_, static_cast<std::uint64_t>(owner_value));
        key_.push_back('/');
        append_db_number(key_, static_cast<std::uint64_t>(relation));
        key_.push_back('/');
        std::size_t nulls = 0;
        for_each_db_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t i> {
            if constexpr (column_type::options.primary_key_) {
                const auto value = row[columns_[offset + i]].value();
                if (!value) {
                    ++nulls;
                    return;
                }
                append_db_number(key_, static_cast<std::uint64_t>(value->size()));
                key_.push_back(':');
                key_.append(*value);
            }
        });
        if (nulls != 0 && nulls != db_primary_key_count<entity_type>()) {
            throw std::invalid_argument("partially NULL composite primary key in a relation query");
        }
        return nulls == 0;
    }

    template <typename entity_type>
    void decode_relations(entity_type& entity, const db_row& row, std::size_t parent_value, std::size_t scope) {
        for_each_db_descriptor<typename entity_type::relations_type>([&]<typename relation_type, std::size_t i> {
            const auto node_index = plan_.find(parent_value, i);
            if (node_index == db_relation_plan::root) {
                return;
            }
            const auto& node_value = plan_.nodes()[node_index];
            using target_type = typename relation_type::target_entity_type;
            const bool present = make_key<target_type>(row, node_value.offset_, scope, node_index);
            if constexpr (relation_type::is_collection) {
                auto& collection_value = db_entity_access<entity_type>::template ensure_relation_collection<relation_type::name>(entity);
                if (!present) {
                    return;
                }
                auto position = identities_.find(key_);
                db_graph_identity identity;
                if (position == identities_.end()) {
                    identity = {collection_value.size(), next_scope_++};
                    target_type child_value(resource_);
                    decode_columns(child_value, row, node_value.offset_);
                    collection_value.push_back(std::move(child_value));
                    identities_.emplace(key_, identity);
                } else {
                    identity = position->second;
                }
                decode_relations(collection_value[identity.index_], row, node_index, identity.scope_);
            } else {
                if (!present) {
                    if (entity.template is_set<relation_type::name>() && !entity.template is_null<relation_type::name>()) {
                        throw std::invalid_argument("inconsistent to-one rows in a relation query");
                    }
                    db_entity_access<entity_type>::template set_relation_null<relation_type::name>(entity);
                    return;
                }
                auto position = identities_.find(key_);
                db_graph_identity identity;
                if (position == identities_.end()) {
                    if (entity.template is_set<relation_type::name>()) {
                        throw std::invalid_argument("to-one relation returned more than one target");
                    }
                    identity = {0, next_scope_++};
                    auto& child_value = db_entity_access<entity_type>::template emplace_relation<relation_type::name>(entity);
                    decode_columns(child_value, row, node_value.offset_);
                    identities_.emplace(key_, identity);
                } else {
                    identity = position->second;
                }
                decode_relations(entity.template get<relation_type::name>(), row, node_index, identity.scope_);
            }
        });
    }

    const db_relation_plan& plan_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<std::size_t> columns_;
    std::pmr::unordered_map<std::pmr::string, db_graph_identity> identities_;
    std::pmr::string key_;
    std::size_t next_scope_{1};
};

template <typename entity_type>
struct db_map_related_entities final {
    db_relation_plan plan_;
    entity_rows<entity_type> operator()(db_rows&& rows, std::pmr::memory_resource* resource) {
        return db_relation_decoder(plan_, resource).template decode<entity_type>(rows);
    }
};

template <typename entity_type>
struct db_map_one_related_entity final {
    db_relation_plan plan_;
    std::optional<entity_type> operator()(db_rows&& rows, std::pmr::memory_resource* resource) {
        auto entities = db_relation_decoder(plan_, resource).template decode<entity_type>(rows);
        if (entities.empty()) {
            return std::nullopt;
        }
        if (entities.size() != 1) {
            throw std::invalid_argument("single-entity relation query returned multiple roots");
        }
        return std::optional<entity_type>(std::in_place, std::move(entities[0]));
    }
};

}  // namespace ruvia::detail
