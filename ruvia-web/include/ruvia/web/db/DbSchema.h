#pragma once

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/web/db/DbEntity.h"
#include "ruvia/web/db/DbMigration.h"
#include "ruvia/web/db/DbProcedure.h"
#include "ruvia/web/db/DbQuery.h"
#include "ruvia/web/detail/db/DbRelationMetadata.h"

namespace ruvia {

enum class DbIdentity : std::uint8_t { kNone,
    kByDefault,
    kAlways };
enum class DbReferentialAction : std::uint8_t { kNoAction,
    kRestrict,
    kCascade,
    kSetNull,
    kSetDefault };
enum class DbConstraintKind : std::uint8_t { kPrimaryKey,
    kUnique,
    kForeignKey,
    kCheck };
enum class DbDropBehavior : std::uint8_t { kRestrict,
    kCascade };
enum class DbIndexMethod : std::uint8_t { kBtree,
    kHash,
    kGin,
    kGist,
    kSpGist,
    kBrin };
enum class DbTriggerTiming : std::uint8_t { kBefore,
    kAfter,
    kInsteadOf };
enum class DbTriggerEvent : std::uint8_t { kInsert,
    kUpdate,
    kDelete,
    kTruncate };

struct DbTriggerDefinition final {
    std::string name{};
    std::string table{};
    std::string function{};
    DbTriggerTiming timing{DbTriggerTiming::kBefore};
    std::vector<DbTriggerEvent> events{};
    bool forEachRow{true};
    std::vector<std::string> updateColumns{};
    DbExpression when{};
    std::vector<std::string> arguments{};
};

struct DbSchemaColumn final {
    std::string name{};
    DbTypeDefinition type{};
    bool nullable{false};
    bool primaryKey{false};
    bool unique{false};
    DbIdentity identity{DbIdentity::kNone};
    DbQuery::Expr defaultValue{};
    DbGeneratedType generatedType{DbGeneratedType::kNone};
    DbExpression asExpression{};
};

struct DbSchemaConstraint final {
    std::string name{};
    DbConstraintKind kind{DbConstraintKind::kCheck};
    std::vector<std::string> columns{};
    std::string referencedTable{};
    std::vector<std::string> referencedColumns{};
    DbReferentialAction onDelete{DbReferentialAction::kNoAction};
    DbReferentialAction onUpdate{DbReferentialAction::kNoAction};
    bool deferrable{false};
    bool initiallyDeferred{false};
    bool notValid{false};
    DbQuery::Expr check{};
};

struct DbTableDefinition final {
    std::string name{};
    std::vector<DbSchemaColumn> columns{};
    std::vector<DbSchemaConstraint> constraints{};
    bool ifNotExists{false};
    bool temporary{false};
    bool unlogged{false};
};

struct DbIndexKey final {
    std::string column{};
    DbQuery::Expr expression{};
    DbOrderDirection order{DbOrderDirection::kAsc};
    DbNullsOrder nulls{DbNullsOrder::kDefault};
    std::string operatorClass{};
};

struct DbIndexDefinition final {
    std::string name{};
    std::string table{};
    std::vector<DbIndexKey> keys{};
    bool unique{false};
    bool ifNotExists{false};
    bool concurrently{false};
    DbIndexMethod method{DbIndexMethod::kBtree};
    std::vector<std::string> include{};
    DbQuery::Expr where{};
};

struct DbSchemaOptions final {
    DbDriver driver{DbDriver::kUnspecified};
    std::pmr::memory_resource* resource{nullptr};
};

struct DbColumnDefault final {
    std::string column{};
    DbExpression value{};
};
struct DbColumnGenerated final {
    std::string column{};
    DbExpression expression{};
};
struct DbEntityTableOptions final {
    std::vector<DbColumnDefault> defaults{};
    std::vector<DbColumnGenerated> generatedColumns{};
    std::vector<DbSchemaConstraint> constraints{};
    bool ifNotExists{false};
    bool temporary{false};
    bool unlogged{false};
};
struct DbTableOption final {
    std::string name{};
    std::variant<bool, std::int64_t, std::string> value{false};
};
struct DbHypertableOptions final {
    std::string table{};
    std::string timeColumn{};
    DbExpression chunkInterval{};
    bool createDefaultIndexes{true};
    bool ifNotExists{false};
    bool migrateData{false};
};
struct DbCompressionOrder final {
    std::string column{};
    DbOrderDirection direction{DbOrderDirection::kAsc};
    DbNullsOrder nulls{DbNullsOrder::kDefault};
};
struct DbCompressionOptions final {
    bool enabled{true};
    std::vector<std::string> segmentBy{};
    std::vector<DbCompressionOrder> orderBy{};
};

// Explicit, versioned schema changes. Each operation consumes its borrowed
// expressions synchronously and stores only generated, owning SQL. Nothing
// here connects to a database or synchronizes a running application's schema.
class DbSchema final {
public:
    explicit DbSchema(DbSchemaOptions options);
    DbSchema(const DbSchema&) = delete;
    DbSchema& operator=(const DbSchema&) = delete;
    DbSchema(DbSchema&&) noexcept = default;
    DbSchema& operator=(DbSchema&&) = delete;

    void createSchema(std::string_view name, bool ifNotExists = false);
    void dropSchema(std::string_view name, DbDropBehavior behavior = DbDropBehavior::kRestrict, bool ifExists = false);
    void createTable(const DbTableDefinition& table);
    template <typename Entity>
    void createTable(const DbEntityTableOptions& options = {});
    template <typename Entity>
    void createRelationTables();
    void dropTable(std::string_view table, DbDropBehavior behavior = DbDropBehavior::kRestrict, bool ifExists = false);
    void renameTable(std::string_view table, std::string_view name);
    void addColumn(std::string_view table, const DbSchemaColumn& column, bool ifNotExists = false);
    void dropColumn(std::string_view table, std::string_view column, DbDropBehavior behavior = DbDropBehavior::kRestrict, bool ifExists = false);
    void renameColumn(std::string_view table, std::string_view column, std::string_view name);
    void alterColumnType(std::string_view table, std::string_view column, const DbTypeDefinition& type, DbQuery::Expr usingValue = {});
    void setColumnNullable(std::string_view table, std::string_view column, bool nullable);
    void setColumnDefault(std::string_view table, std::string_view column, DbQuery::Expr expression);
    void dropColumnDefault(std::string_view table, std::string_view column);
    void addConstraint(std::string_view table, const DbSchemaConstraint& constraint);
    void dropConstraint(std::string_view table, std::string_view name, DbDropBehavior behavior = DbDropBehavior::kRestrict, bool ifExists = false);
    void validateConstraint(std::string_view table, std::string_view name);
    void createIndex(const DbIndexDefinition& index);
    void dropIndex(std::string_view name, bool concurrently = false, bool ifExists = false);
    void createEnum(std::string_view name, std::span<const std::string_view> values);
    void addEnumValue(std::string_view name, std::string_view value, bool ifNotExists = false);
    void dropEnum(std::string_view name, DbDropBehavior behavior = DbDropBehavior::kRestrict, bool ifExists = false);
    void createExtension(std::string_view name, bool ifNotExists = true);
    void createView(std::string_view name, const DbQuery& query, bool replace = false, bool materialized = false);
    void dropView(std::string_view name, bool materialized = false, DbDropBehavior behavior = DbDropBehavior::kRestrict, bool ifExists = false);
    void execute(const DbQuery& query);
    void perform(DbQuery::Expr expression);
    void run(const DbProcedure& procedure);
    void createTriggerFunction(std::string_view name, const DbProcedure& procedure, bool replace = false);
    void dropTriggerFunction(std::string_view name, DbDropBehavior behavior = DbDropBehavior::kRestrict, bool ifExists = false);
    void createTrigger(const DbTriggerDefinition& trigger);
    void dropTrigger(std::string_view table, std::string_view name, bool ifExists = false);
    void setTableOptions(std::string_view table, std::span<const DbTableOption> options);
    void setDatabaseOption(std::string_view database, std::string_view name, std::string_view value);
    void createHypertable(const DbHypertableOptions& options);
    void setChunkTimeInterval(std::string_view table, DbExpression interval);
    void setCompression(std::string_view table, const DbCompressionOptions& options);
    void addCompressionPolicy(std::string_view table, DbExpression after, bool ifNotExists = false);
    void removeCompressionPolicy(std::string_view table, bool ifExists = false);
    void addRetentionPolicy(std::string_view table, DbExpression after, bool ifNotExists = false);
    void removeRetentionPolicy(std::string_view table, bool ifExists = false);

    // PostgreSQL batches are one atomic DO statement. A nontransactional
    // operation must be the batch's only operation. MariaDB DDL has implicit
    // commits, so each generated statement receives a stable numbered id.
    [[nodiscard]] std::pmr::vector<DbMigration> compile(std::string_view id) const;
    [[nodiscard]] DbDriver driver() const noexcept {
        return driver_;
    }

private:
    friend class DbProcedure;
    struct Statement final {
        std::pmr::string sql;
        DbMigrationAtomicity atomicity{DbMigrationAtomicity::kTransactional};
        bool procedural{false};
    };
    void append(std::pmr::string sql, DbMigrationAtomicity atomicity = DbMigrationAtomicity::kTransactional, bool procedural = false);
    void requirePostgreSql() const;
    [[nodiscard]] std::pmr::string tablePrefix(std::string_view table) const;
    void appendType(std::pmr::string& sql, const DbTypeDefinition& type) const;
    void appendColumn(std::pmr::string& sql, const DbSchemaColumn& column) const;
    void appendConstraint(std::pmr::string& sql, const DbSchemaConstraint& constraint) const;
    void appendExpression(std::pmr::string& sql, DbQuery::Expr expression) const;
    template <typename Column>
    static DbTypeDefinition entityColumnType() {
        DbTypeDefinition type{.dataType = Column::dataType, .length = Column::options.length, .precision = Column::options.precision, .scale = Column::options.scale};
        if constexpr (detail::IsPmrVector<typename Column::value_type>::value) {
            using Item = typename detail::IsPmrVector<typename Column::value_type>::value_type;
            if constexpr (Column::options.dataType == DbDataType::kInferred || Column::options.dataType == DbDataType::kArray) {
                type.dataType = detail::DbEntityTypeTraits<Item>::dataType;
            }
            type.array = true;
        }
        return type;
    }

    DbDriver driver_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<Statement> statements_;
};

template <typename Entity>
void DbSchema::createTable(const DbEntityTableOptions& options) {
    DbTableDefinition table{.name = std::string(Entity::tableName()), .constraints = options.constraints, .ifNotExists = options.ifNotExists, .temporary = options.temporary, .unlogged = options.unlogged};
    std::vector<std::string> primary;
    bool conflictingGeneration = false;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        const auto add = [&]<typename C> {
            conflictingGeneration |= C::options.generated && C::options.generatedType != DbGeneratedType::kNone;
            DbSchemaColumn column{.name = std::string(C::name.view()),
                .type = entityColumnType<C>(),
                .nullable = C::options.nullable,
                .generatedType = C::options.generatedType};
            if constexpr (C::options.primaryKey) {
                primary.push_back(column.name);
            }
            if constexpr (C::options.generated && (C::dataType == DbDataType::kSmallInt || C::dataType == DbDataType::kInteger || C::dataType == DbDataType::kBigInt)) {
                column.identity = DbIdentity::kByDefault;
            }
            for (const auto& value : options.generatedColumns) {
                if (value.column == column.name) {
                    if (!column.asExpression.empty()) {
                        throw std::invalid_argument("duplicate entity generated column");
                    }
                    column.asExpression = value.expression;
                }
            }
            for (const auto& value : options.defaults) {
                if (value.column == column.name) {
                    if (!column.defaultValue.empty()) {
                        throw std::invalid_argument("duplicate entity column default");
                    }
                    column.defaultValue = value.value;
                }
            }
            table.columns.push_back(std::move(column));
        };
        (add.template operator()<std::tuple_element_t<I, typename Entity::Columns>>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<typename Entity::Columns>>{});
    if (conflictingGeneration) {
        throw std::invalid_argument("computed columns cannot also request generated identity values");
    }
    for (const auto& value : options.generatedColumns) {
        bool found = false;
        for (const auto& column : table.columns) {
            if (column.name == value.column) {
                found = true;
                if (value.expression.empty()) {
                    throw std::invalid_argument("entity generated column requires an expression");
                }
                if (column.generatedType == DbGeneratedType::kNone) {
                    throw std::invalid_argument("entity generated column requires generated metadata");
                }
            }
        }
        if (!found) {
            throw std::invalid_argument("entity generated column requires a known column");
        }
    }
    for (auto& column : table.columns) {
        if (column.generatedType != DbGeneratedType::kNone) {
            if (column.asExpression.empty()) {
                throw std::invalid_argument("generated column requires an expression");
            }
            if (column.identity != DbIdentity::kNone || !column.defaultValue.empty()) {
                throw std::invalid_argument("generated column conflicts with identity or default");
            }
        }
    }
    for (const auto& value : options.defaults) {
        bool found = false;
        for (const auto& column : table.columns) {
            found |= column.name == value.column;
        }
        if (!found || value.value.empty()) {
            throw std::invalid_argument("entity default requires a known column and an expression");
        }
    }
    if (!primary.empty()) {
        const auto dot = Entity::tableName().rfind('.');
        const auto local = Entity::tableName().substr(dot == std::string_view::npos ? 0 : dot + 1);
        table.constraints.push_back({.name = std::string(local) + "_pkey", .kind = DbConstraintKind::kPrimaryKey, .columns = std::move(primary)});
    }
    detail::forEachDbDescriptor<typename Entity::Relations>([&]<typename R, std::size_t> {
        if constexpr (!R::isCollection && R::isOwning) {
            detail::validateDbRelation<Entity, R>();
            using Mapping = detail::DbRelationMapping<Entity, R>;
            using Target = typename R::TargetEntity;
            const auto localTable = Entity::tableName().substr(Entity::tableName().rfind('.') == std::string_view::npos ? 0 : Entity::tableName().rfind('.') + 1);
            const auto prefix = std::string(localTable) + "_" + std::string(R::name.view());
            DbSchemaConstraint fk{.name = prefix + "_fkey", .kind = DbConstraintKind::kForeignKey, .referencedTable = std::string(Target::tableName())};
            detail::forEachDbDescriptor<typename Mapping::JoinColumns>([&]<typename J, std::size_t> { fk.columns.emplace_back(J::local.view()); fk.referencedColumns.emplace_back(J::referenced.view()); });
            table.constraints.push_back(std::move(fk));
            if constexpr (R::kind == DbRelationKind::kOneToOne) {
                table.constraints.push_back({.name = prefix + "_key", .kind = DbConstraintKind::kUnique, .columns = table.constraints.back().columns});
            }
        }
    });
    createTable(table);
}

template <typename Entity>
void DbSchema::createRelationTables() {
    detail::forEachDbDescriptor<typename Entity::Relations>([&]<typename R, std::size_t> {
        if constexpr (R::kind == DbRelationKind::kManyToMany && R::isOwning) {
            detail::validateDbRelation<Entity, R>();
            using M = detail::DbRelationMapping<Entity, R>;
            using Target = typename R::TargetEntity;
            DbTableDefinition table{.name = std::string(M::tableName())};
            const auto dot = M::tableName().rfind('.');
            const auto localTable = std::string(M::tableName().substr(dot == std::string_view::npos ? 0 : dot + 1));
            const auto addSide = [&]<typename ReferencedEntity, typename Columns>(std::string_view side) {
                DbSchemaConstraint fk{.name = localTable + "_" + std::string(side) + "_fkey",
                    .kind = DbConstraintKind::kForeignKey,
                    .referencedTable = std::string(ReferencedEntity::tableName())};
                detail::forEachDbDescriptor<Columns>([&]<typename J, std::size_t> {
                    using C = std::tuple_element_t<ReferencedEntity::template columnIndex<J::referenced>(), typename ReferencedEntity::Columns>;
                    table.columns.push_back({.name = std::string(J::local.view()), .type = entityColumnType<C>(), .nullable = false});
                    fk.columns.emplace_back(J::local.view());
                    fk.referencedColumns.emplace_back(J::referenced.view());
                });
                table.constraints.push_back(std::move(fk));
            };
            addSide.template operator()<Entity, typename M::SourceColumns>("owner");
            addSide.template operator()<Target, typename M::TargetColumns>("inverse");
            std::vector<std::string> primary;
            for (const auto& column : table.columns) {
                primary.push_back(column.name);
            }
            table.constraints.push_back({.name = localTable + "_pkey", .kind = DbConstraintKind::kPrimaryKey, .columns = std::move(primary)});
            createTable(table);
        }
    });
}

}  // namespace ruvia
