#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/web/db/DbEntity.h"

namespace ruvia::detail {

template <typename Tuple, typename Function>
constexpr void forEachDbDescriptor(Function&& function) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (function.template operator()<std::tuple_element_t<I, Tuple>, I>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

template <typename Entity, FixedString Name>
consteval std::size_t dbRelationIndex() {
    std::size_t result = std::tuple_size_v<typename Entity::Relations>;
    forEachDbDescriptor<typename Entity::Relations>([&]<typename Relation, std::size_t I> {
        if constexpr (Relation::name == Name) {
            result = I;
        }
    });
    return result;
}

template <typename Entity, FixedString Name>
struct DbRelationNamed final {
    static constexpr auto index = dbRelationIndex<Entity, Name>();
    static_assert(index < std::tuple_size_v<typename Entity::Relations>, "unknown inverse database relation");
    using Type = std::tuple_element_t<index, typename Entity::Relations>;
};

template <typename Tuple>
struct ReverseDbJoinColumns;
template <typename... Columns>
struct ReverseDbJoinColumns<std::tuple<Columns...>> final {
    using Type = std::tuple<DbJoinColumn<Columns::referenced, Columns::local>...>;
};

// One normalized mapping is consumed by schema generation and query planning.
// Target metadata is deliberately inspected only when a mapping is consumed,
// so mutually recursive and self-referential entity declarations stay valid.
template <typename Source, typename Relation,
    DbRelationKind Kind = Relation::kind, bool Owning = Relation::isOwning>
struct DbRelationMapping;

template <typename Source, typename Relation>
struct DbRelationMapping<Source, Relation, DbRelationKind::kManyToOne, true> {
    static constexpr bool throughJoinTable = false;
    using JoinColumns = typename Relation::JoinColumns;
};

template <typename Source, typename Relation>
struct DbRelationMapping<Source, Relation, DbRelationKind::kOneToOne, true>
    : DbRelationMapping<Source, Relation, DbRelationKind::kManyToOne, true> {};

template <typename Source, typename Relation>
struct DbRelationMapping<Source, Relation, DbRelationKind::kOneToMany, false> {
    using Inverse = typename DbRelationNamed<typename Relation::TargetEntity, Relation::inverseName>::Type;
    static_assert(Inverse::kind == DbRelationKind::kManyToOne && Inverse::isOwning,
        "one-to-many must refer to an owning many-to-one relation");
    static_assert(std::is_same_v<typename Inverse::TargetEntity, Source>,
        "inverse database relation must target its source entity");
    static constexpr bool throughJoinTable = false;
    using JoinColumns = typename ReverseDbJoinColumns<typename Inverse::JoinColumns>::Type;
};

template <typename Source, typename Relation>
struct DbRelationMapping<Source, Relation, DbRelationKind::kOneToOne, false> {
    using Inverse = typename DbRelationNamed<typename Relation::TargetEntity, Relation::inverseName>::Type;
    static_assert(Inverse::kind == DbRelationKind::kOneToOne && Inverse::isOwning,
        "inverse one-to-one must refer to an owning one-to-one relation");
    static_assert(std::is_same_v<typename Inverse::TargetEntity, Source>,
        "inverse database relation must target its source entity");
    static constexpr bool throughJoinTable = false;
    using JoinColumns = typename ReverseDbJoinColumns<typename Inverse::JoinColumns>::Type;
};

template <typename Source, typename Relation>
struct DbRelationMapping<Source, Relation, DbRelationKind::kManyToMany, true> {
    static constexpr bool throughJoinTable = true;
    using Table = typename Relation::JoinTable;
    using SourceColumns = typename Table::OwnerColumns;
    using TargetColumns = typename Table::InverseColumns;
    static constexpr std::string_view tableName() noexcept {
        return Table::name.view();
    }
};

template <typename Source, typename Relation>
struct DbRelationMapping<Source, Relation, DbRelationKind::kManyToMany, false> {
    using Inverse = typename DbRelationNamed<typename Relation::TargetEntity, Relation::inverseName>::Type;
    static_assert(Inverse::kind == DbRelationKind::kManyToMany && Inverse::isOwning,
        "inverse many-to-many must refer to an owning many-to-many relation");
    static_assert(std::is_same_v<typename Inverse::TargetEntity, Source>,
        "inverse database relation must target its source entity");
    static constexpr bool throughJoinTable = true;
    using Table = typename Inverse::JoinTable;
    using SourceColumns = typename Table::InverseColumns;
    using TargetColumns = typename Table::OwnerColumns;
    static constexpr std::string_view tableName() noexcept {
        return Table::name.view();
    }
};

template <typename Columns>
consteval bool uniqueDbJoinColumns() {
    constexpr auto count = std::tuple_size_v<Columns>;
    std::array<std::string_view, count> local{}, referenced{};
    forEachDbDescriptor<Columns>([&]<typename Column, std::size_t I> {
        local[I] = Column::local.view();
        referenced[I] = Column::referenced.view();
    });
    for (std::size_t i = 0; i < count; ++i) {
        if (local[i].empty() || referenced[i].empty()) {
            return false;
        }
        for (std::size_t j = i + 1; j < count; ++j) {
            if (local[i] == local[j] || referenced[i] == referenced[j]) {
                return false;
            }
        }
    }
    return true;
}

template <typename Source, typename Relation>
constexpr void validateDbRelation() {
    using Target = typename Relation::TargetEntity;
    using Mapping = DbRelationMapping<Source, Relation>;
    if constexpr (Mapping::throughJoinTable) {
        using Owner = typename Mapping::SourceColumns;
        using Other = typename Mapping::TargetColumns;
        static_assert(std::tuple_size_v<Owner> > 0 && std::tuple_size_v<Other> > 0,
            "a join table requires both owner and inverse columns");
        static_assert(uniqueDbJoinColumns<Owner>() && uniqueDbJoinColumns<Other>(),
            "join table column mappings must be nonempty and unique");
        static_assert([] {
            bool unique = true;
            forEachDbDescriptor<Owner>([&]<typename Left, std::size_t> {
                forEachDbDescriptor<Other>([&]<typename Right, std::size_t> {
                    unique &= Left::local.view() != Right::local.view();
                });
            });
            return unique;
        }(),
            "join table owner and inverse columns must have distinct names");
        forEachDbDescriptor<Owner>([]<typename Column, std::size_t> {
            (void)Source::template columnIndex<Column::referenced>();
        });
        forEachDbDescriptor<Other>([]<typename Column, std::size_t> {
            (void)Target::template columnIndex<Column::referenced>();
        });
    } else {
        using Columns = typename Mapping::JoinColumns;
        static_assert(std::tuple_size_v<Columns> > 0, "a database relation requires join columns");
        static_assert(uniqueDbJoinColumns<Columns>(), "database relation column mappings must be nonempty and unique");
        forEachDbDescriptor<Columns>([]<typename Column, std::size_t> {
            (void)Source::template columnIndex<Column::local>();
            (void)Target::template columnIndex<Column::referenced>();
        });
    }
}

template <typename Entity>
consteval std::size_t dbPrimaryKeyCount() {
    std::size_t count = 0;
    forEachDbDescriptor<typename Entity::Columns>([&]<typename Column, std::size_t> {
        count += Column::options.primaryKey ? 1 : 0;
    });
    return count;
}

template <typename Entity>
consteval auto dbPrimaryKeyColumns() {
    std::array<std::string_view, dbPrimaryKeyCount<Entity>()> result{};
    std::size_t i = 0;
    forEachDbDescriptor<typename Entity::Columns>([&]<typename Column, std::size_t> {
        if constexpr (Column::options.primaryKey) {
            result[i++] = Column::name.view();
        }
    });
    return result;
}

}  // namespace ruvia::detail
