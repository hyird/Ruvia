#pragma once

#include "ruvia/web/db/DbEntity.h"

namespace ruvia::detail {

template <typename E>
struct DbEntityAccess final {
    template <FixedString Name>
    static auto& emplaceRelation(E& entity) {
        static_assert(E::template isRelation<Name>());
        auto& slot = entity.template relationSlot<Name>();
        using Relation = std::tuple_element_t<E::template relationIndex<Name>(), typename E::Relations>;
        using Target = typename Relation::TargetEntity;
        static_assert(!Relation::isCollection, "emplaceRelation is only valid for to-one relations");
        slot.reset();
        std::pmr::polymorphic_allocator<Target> allocator(entity.resource());
        slot.value.reset(allocator.template new_object<Target>(entity.resource()));
        slot.state = decltype(slot.state)::kValue;
        return *slot.value;
    }

    template <FixedString Name>
    static auto& ensureRelationCollection(E& entity) {
        static_assert(E::template isRelation<Name>());
        auto& slot = entity.template relationSlot<Name>();
        using Relation = std::tuple_element_t<E::template relationIndex<Name>(), typename E::Relations>;
        static_assert(Relation::isCollection, "ensureRelationCollection requires a collection relation");
        if (slot.state != decltype(slot.state)::kValue) {
            using Stored = typename std::remove_reference_t<decltype(slot)>::Stored;
            std::pmr::polymorphic_allocator<Stored> allocator(entity.resource());
            slot.value.reset(allocator.template new_object<Stored>(entity.resource()));
            slot.state = decltype(slot.state)::kValue;
        }
        return *slot.value;
    }

    template <FixedString Name>
    static void setRelationNull(E& entity) {
        static_assert(E::template isRelation<Name>());
        using Relation = std::tuple_element_t<E::template relationIndex<Name>(), typename E::Relations>;
        static_assert(!Relation::isCollection, "a loaded collection can be empty but cannot be NULL");
        auto& slot = entity.template relationSlot<Name>();
        slot.reset();
        slot.state = decltype(slot.state)::kNull;
    }
};

}  // namespace ruvia::detail
