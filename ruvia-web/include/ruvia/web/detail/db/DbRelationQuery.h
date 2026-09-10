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

#include "ruvia/web/db/DbQuery.h"
#include "ruvia/web/detail/db/DbEntityAccess.h"
#include "ruvia/web/detail/db/DbEntityCodec.h"
#include "ruvia/web/detail/db/DbRelationMetadata.h"

namespace ruvia::detail {

// This plan belongs to a builder or a single cold operation. It owns every
// selected path and result-column name; it never retains an AST or row view.
class DbRelationPlan final {
public:
    static constexpr std::size_t root = std::numeric_limits<std::size_t>::max();
    struct Node final {
        explicit Node(std::pmr::memory_resource* resource)
            : path(resource),
              alias(resource),
              columns(resource) {}
        Node(const Node& other, std::pmr::memory_resource* resource)
            : parent(other.parent),
              relation(other.relation),
              offset(other.offset),
              join(other.join),
              path(other.path, resource),
              alias(other.alias, resource),
              columns(other.columns, resource) {}
        std::size_t parent{root};
        std::size_t relation{0};
        std::size_t offset{0};
        DbJoinType join{DbJoinType::kLeft};
        std::pmr::string path;
        std::pmr::string alias;
        std::pmr::vector<std::pmr::string> columns;
    };

    explicit DbRelationPlan(std::pmr::memory_resource* resource)
        : resource_(pmrResourceOrDefault(resource)),
          nodes_(resource_) {}
    DbRelationPlan(const DbRelationPlan&) = delete;
    DbRelationPlan& operator=(const DbRelationPlan&) = delete;
    DbRelationPlan(DbRelationPlan&&) noexcept = default;
    DbRelationPlan& operator=(DbRelationPlan&&) = delete;

    [[nodiscard]] DbRelationPlan clone() const {
        DbRelationPlan result(resource_);
        result.columnCount_ = columnCount_;
        result.nextAlias_ = nextAlias_;
        result.nextProjection_ = nextProjection_;
        for (const auto& node : nodes_) {
            result.nodes_.emplace_back(node, resource_);
        }
        return result;
    }
    [[nodiscard]] bool empty() const noexcept {
        return nodes_.empty();
    }
    [[nodiscard]] std::size_t columnCount() const noexcept {
        return columnCount_;
    }
    [[nodiscard]] std::span<const Node> nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] std::size_t find(std::size_t parent, std::size_t relation) const noexcept {
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].parent == parent && nodes_[i].relation == relation) {
                return i;
            }
        }
        return root;
    }

    template <typename Entity>
    void add(DbQuery& query, std::string_view rootAlias, std::string_view path,
        std::string_view alias = {}, DbJoinType join = DbJoinType::kLeft) {
        if constexpr (dbPrimaryKeyCount<Entity>() == 0) {
            throw std::invalid_argument("relation loading requires a primary key on the root entity");
        }
        if (alias.find('.') != std::string_view::npos || alias.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("a relation join alias must be one identifier");
        }
        auto normalized = normalize(rootAlias, path);
        validatePath<Entity>(normalized, 0);
        if (!alias.empty() && query.usesSourceName(alias)) {
            bool same = false;
            for (const auto& node : nodes_) {
                same |= node.path == normalized && node.alias == alias && node.join == join;
            }
            if (!same) {
                throw std::invalid_argument("relation join alias is already in use");
            }
        }
        if (nodes_.empty()) {
            columnCount_ = std::tuple_size_v<typename Entity::Columns>;
        }
        addPath<Entity>(query, rootAlias, normalized, root, alias, join);
    }

    template <typename Entity>
    [[nodiscard]] std::optional<DbQuery> prepare(const DbQuery& query,
        std::string_view rootAlias, DbDriver driver) const {
        constexpr auto keys = dbPrimaryKeyColumns<Entity>();
        return query.prepareEntityRead(keys, rootAlias, driver);
    }

private:
    std::pmr::string normalize(std::string_view rootAlias, std::string_view path) const {
        if (path.size() > rootAlias.size() && path.starts_with(rootAlias) && path[rootAlias.size()] == '.') {
            path.remove_prefix(rootAlias.size() + 1);
        } else if (const auto dot = path.find('.'); dot != std::string_view::npos) {
            for (const auto& node : nodes_) {
                if (path.substr(0, dot) == node.alias) {
                    std::pmr::string result(node.path, resource_);
                    result.append(path.substr(dot));
                    return result;
                }
            }
        }
        return std::pmr::string(path, resource_);
    }

    template <typename Entity>
    static void validatePath(std::string_view path, std::size_t depth) {
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
        forEachDbDescriptor<typename Entity::Relations>([&]<typename Relation, std::size_t> {
            if (name != Relation::name.view()) {
                return;
            }
            found = true;
            validateDbRelation<Entity, Relation>();
            using Target = typename Relation::TargetEntity;
            if constexpr (dbPrimaryKeyCount<Target>() == 0) {
                throw std::invalid_argument("relation loading requires a primary key on every selected target");
            }
            if (!rest.empty()) {
                validatePath<Target>(rest, depth + 1);
            }
        });
        if (!found) {
            throw std::invalid_argument("unknown database entity relation");
        }
    }

    std::pmr::string uniqueAlias(const DbQuery& query, std::string_view reserved) {
        for (;;) {
            std::pmr::string alias("__ruvia_relation_", resource_);
            appendDbNumber(alias, static_cast<std::uint64_t>(nextAlias_++));
            if (alias != reserved && !query.usesSourceName(alias)) {
                return alias;
            }
        }
    }
    std::pmr::string uniqueProjection(const DbQuery& query) {
        for (;;) {
            std::pmr::string name("__ruvia_column_", resource_);
            appendDbNumber(name, static_cast<std::uint64_t>(nextProjection_++));
            if (!query.usesProjectionName(name)) {
                return name;
            }
        }
    }
    static void conjunction(DbQuery& query, DbExpression& predicate, DbExpression term) {
        predicate = predicate.empty() ? term : query.binary(predicate, DbBinaryOperator::kAnd, term);
    }

    template <typename Entity, typename Relation>
    void appendJoin(DbQuery& query, std::string_view sourceAlias, Node& node,
        std::string_view reservedAlias) {
        using Mapping = DbRelationMapping<Entity, Relation>;
        using Target = typename Relation::TargetEntity;
        DbExpression predicate;
        if constexpr (Mapping::throughJoinTable) {
            auto junction = uniqueAlias(query, node.alias);
            // A terminal user alias is reserved even while constructing an
            // ancestor's automatic junction alias.
            if (junction == reservedAlias) {
                junction = uniqueAlias(query, reservedAlias);
            }
            forEachDbDescriptor<typename Mapping::SourceColumns>([&]<typename Column, std::size_t> {
                conjunction(query, predicate, query.binary(query.column(Column::referenced.view(), sourceAlias), DbBinaryOperator::kEqual, query.column(Column::local.view(), junction)));
            });
            query.join(node.join, Mapping::tableName(), predicate, junction);
            predicate = {};
            forEachDbDescriptor<typename Mapping::TargetColumns>([&]<typename Column, std::size_t> {
                conjunction(query, predicate, query.binary(query.column(Column::local.view(), junction), DbBinaryOperator::kEqual, query.column(Column::referenced.view(), node.alias)));
            });
        } else {
            forEachDbDescriptor<typename Mapping::JoinColumns>([&]<typename Column, std::size_t> {
                conjunction(query, predicate, query.binary(query.column(Column::local.view(), sourceAlias), DbBinaryOperator::kEqual, query.column(Column::referenced.view(), node.alias)));
            });
        }
        query.join(node.join, Target::tableName(), predicate, node.alias);
        forEachDbDescriptor<typename Target::Columns>([&]<typename Column, std::size_t> {
            auto name = uniqueProjection(query);
            query.addSelect(query.alias(query.column(Column::name.view(), node.alias), name));
            node.columns.push_back(std::move(name));
        });
    }

    template <typename Entity>
    void addPath(DbQuery& query, std::string_view rootAlias, std::string_view path,
        std::size_t parent, std::string_view requestedAlias, DbJoinType requestedJoin) {
        const auto dot = path.find('.');
        const auto name = path.substr(0, dot);
        const auto rest = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
        forEachDbDescriptor<typename Entity::Relations>([&]<typename Relation, std::size_t I> {
            if (name != Relation::name.view()) {
                return;
            }
            auto index = find(parent, I);
            if (index == root) {
                Node node(resource_);
                node.parent = parent;
                node.relation = I;
                node.offset = columnCount_;
                node.join = rest.empty() ? requestedJoin : DbJoinType::kLeft;
                if (parent != root) {
                    node.path = nodes_[parent].path;
                    node.path.push_back('.');
                }
                node.path.append(name);
                node.alias = rest.empty() && !requestedAlias.empty()
                                 ? std::pmr::string(requestedAlias, resource_)
                                 : uniqueAlias(query, requestedAlias);
                const auto sourceAlias = parent == root ? rootAlias : std::string_view(nodes_[parent].alias);
                appendJoin<Entity, Relation>(query, sourceAlias, node, requestedAlias);
                columnCount_ += node.columns.size();
                index = nodes_.size();
                nodes_.push_back(std::move(node));
            } else if (rest.empty() && ((!requestedAlias.empty() && nodes_[index].alias != requestedAlias) || nodes_[index].join != requestedJoin)) {
                throw std::invalid_argument("relation path was already loaded with another alias or join kind");
            }
            if (!rest.empty()) {
                addPath<typename Relation::TargetEntity>(query, rootAlias, rest, index, requestedAlias, requestedJoin);
            }
        });
    }

    std::pmr::memory_resource* resource_;
    std::pmr::vector<Node> nodes_;
    std::size_t columnCount_{0};
    std::size_t nextAlias_{0};
    std::size_t nextProjection_{0};
};

struct DbGraphIdentity final {
    std::size_t index{0};
    std::size_t scope{0};
};

// Indices, rather than pointers into result vectors, remain valid when a
// collection grows. The identity table is temporary operation storage; the
// resulting entities own their nested values without retaining this table.
class DbRelationDecoder final {
public:
    DbRelationDecoder(const DbRelationPlan& plan, std::pmr::memory_resource* resource)
        : plan_(plan),
          resource_(pmrResourceOrDefault(resource)),
          columns_(resource_),
          identities_(resource_),
          key_(resource_) {}

    template <typename Entity>
    DbEntityRows<Entity> decode(const DbRows& rows) {
        DbEntityRows<Entity> result(resource_);
        if (rows.empty()) {
            return result;
        }
        resolveColumns<Entity>(rows.front());
        for (const auto& row : rows) {
            if (!makeKey<Entity>(row, 0, 0, DbRelationPlan::root)) {
                throw std::invalid_argument("NULL root primary key in a relation query");
            }
            auto position = identities_.find(key_);
            DbGraphIdentity identity;
            if (position == identities_.end()) {
                identity = {result.size(), nextScope_++};
                Entity entity(resource_);
                decodeColumns(entity, row, 0);
                result.push_back(std::move(entity));
                identities_.emplace(key_, identity);
            } else {
                identity = position->second;
            }
            decodeRelations(result[identity.index], row, DbRelationPlan::root, identity.scope);
        }
        return result;
    }

private:
    template <typename Entity>
    void resolveColumns(const DbRow& row) {
        columns_.resize(plan_.columnCount());
        const auto names = DbResultAccess::columnNames(row);
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
        forEachDbDescriptor<typename Entity::Columns>([&]<typename Column, std::size_t I> {
            columns_[I] = resolve(Column::name.view());
        });
        for (const auto& node : plan_.nodes()) {
            for (std::size_t i = 0; i < node.columns.size(); ++i) {
                columns_[node.offset + i] = resolve(node.columns[i]);
            }
        }
    }

    template <typename Entity>
    void decodeColumns(Entity& entity, const DbRow& row, std::size_t offset) const {
        forEachDbDescriptor<typename Entity::Columns>([&]<typename Column, std::size_t I> {
            decodeEntityField<Entity, Column>(entity, row[columns_[offset + I]], resource_);
        });
    }

    template <typename Entity>
    bool makeKey(const DbRow& row, std::size_t offset, std::size_t owner, std::size_t relation) {
        key_.clear();
        appendDbNumber(key_, static_cast<std::uint64_t>(owner));
        key_.push_back('/');
        appendDbNumber(key_, static_cast<std::uint64_t>(relation));
        key_.push_back('/');
        std::size_t nulls = 0;
        forEachDbDescriptor<typename Entity::Columns>([&]<typename Column, std::size_t I> {
            if constexpr (Column::options.primaryKey) {
                const auto value = row[columns_[offset + I]].value();
                if (!value) {
                    ++nulls;
                    return;
                }
                appendDbNumber(key_, static_cast<std::uint64_t>(value->size()));
                key_.push_back(':');
                key_.append(*value);
            }
        });
        if (nulls != 0 && nulls != dbPrimaryKeyCount<Entity>()) {
            throw std::invalid_argument("partially NULL composite primary key in a relation query");
        }
        return nulls == 0;
    }

    template <typename Entity>
    void decodeRelations(Entity& entity, const DbRow& row, std::size_t parent, std::size_t scope) {
        forEachDbDescriptor<typename Entity::Relations>([&]<typename Relation, std::size_t I> {
            const auto nodeIndex = plan_.find(parent, I);
            if (nodeIndex == DbRelationPlan::root) {
                return;
            }
            const auto& node = plan_.nodes()[nodeIndex];
            using Target = typename Relation::TargetEntity;
            const bool present = makeKey<Target>(row, node.offset, scope, nodeIndex);
            if constexpr (Relation::isCollection) {
                auto& collection = DbEntityAccess<Entity>::template ensureRelationCollection<Relation::name>(entity);
                if (!present) {
                    return;
                }
                auto position = identities_.find(key_);
                DbGraphIdentity identity;
                if (position == identities_.end()) {
                    identity = {collection.size(), nextScope_++};
                    Target child(resource_);
                    decodeColumns(child, row, node.offset);
                    collection.push_back(std::move(child));
                    identities_.emplace(key_, identity);
                } else {
                    identity = position->second;
                }
                decodeRelations(collection[identity.index], row, nodeIndex, identity.scope);
            } else {
                if (!present) {
                    if (entity.template isSet<Relation::name>() && !entity.template isNull<Relation::name>()) {
                        throw std::invalid_argument("inconsistent to-one rows in a relation query");
                    }
                    DbEntityAccess<Entity>::template setRelationNull<Relation::name>(entity);
                    return;
                }
                auto position = identities_.find(key_);
                DbGraphIdentity identity;
                if (position == identities_.end()) {
                    if (entity.template isSet<Relation::name>()) {
                        throw std::invalid_argument("to-one relation returned more than one target");
                    }
                    identity = {0, nextScope_++};
                    auto& child = DbEntityAccess<Entity>::template emplaceRelation<Relation::name>(entity);
                    decodeColumns(child, row, node.offset);
                    identities_.emplace(key_, identity);
                } else {
                    identity = position->second;
                }
                decodeRelations(entity.template get<Relation::name>(), row, nodeIndex, identity.scope);
            }
        });
    }

    const DbRelationPlan& plan_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<std::size_t> columns_;
    std::pmr::unordered_map<std::pmr::string, DbGraphIdentity> identities_;
    std::pmr::string key_;
    std::size_t nextScope_{1};
};

template <typename Entity>
struct DbMapRelatedEntities final {
    DbRelationPlan plan;
    DbEntityRows<Entity> operator()(DbRows&& rows, std::pmr::memory_resource* resource) {
        return DbRelationDecoder(plan, resource).template decode<Entity>(rows);
    }
};

template <typename Entity>
struct DbMapOneRelatedEntity final {
    DbRelationPlan plan;
    std::optional<Entity> operator()(DbRows&& rows, std::pmr::memory_resource* resource) {
        auto entities = DbRelationDecoder(plan, resource).template decode<Entity>(rows);
        if (entities.empty()) {
            return std::nullopt;
        }
        if (entities.size() != 1) {
            throw std::invalid_argument("single-entity relation query returned multiple roots");
        }
        return std::optional<Entity>(std::in_place, std::move(entities[0]));
    }
};

}  // namespace ruvia::detail
