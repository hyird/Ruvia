#pragma once

#include "ruvia/web/db/DbEntity.h"

namespace ruvia::detail {

template <typename E>
struct DbEntityAccess final {
    static auto& storage(E& entity) noexcept {
        return entity.sql_storage();
    }
    template <FixedString Name>
    static auto& emplaceRelation(E& entity) {
        auto& owner = storage(entity);
        using owner_type = std::remove_reference_t<decltype(owner)>;
        static_assert(owner_type::template isRelation<Name>());
        auto& slot = owner.template relationSlot<Name>();
        using Relation = std::tuple_element_t<owner_type::template relationIndex<Name>(), typename E::Relations>;
        using Target = typename Relation::TargetEntity;
        static_assert(!Relation::isCollection, "emplaceRelation is only valid for to-one relations");
        std::pmr::polymorphic_allocator<Target> allocator(entity.resource());
        auto* value = allocator.template new_object<Target>(entity.resource());
        slot.value.reset(value);
        slot.state = decltype(slot.state)::value;
        return *slot.value;
    }

    template <FixedString Name>
    static auto& ensureRelationCollection(E& entity) {
        auto& owner = storage(entity);
        using owner_type = std::remove_reference_t<decltype(owner)>;
        static_assert(owner_type::template isRelation<Name>());
        auto& slot = owner.template relationSlot<Name>();
        using Relation = std::tuple_element_t<owner_type::template relationIndex<Name>(), typename E::Relations>;
        static_assert(Relation::isCollection, "ensureRelationCollection requires a collection relation");
        if (slot.state != decltype(slot.state)::value) {
            using Stored = typename std::remove_reference_t<decltype(slot)>::Stored;
            std::pmr::polymorphic_allocator<Stored> allocator(entity.resource());
            slot.value.reset(allocator.template new_object<Stored>(entity.resource()));
            slot.state = decltype(slot.state)::value;
        }
        return *slot.value;
    }

    template <FixedString Name>
    static void setRelationNull(E& entity) {
        auto& owner = storage(entity);
        using owner_type = std::remove_reference_t<decltype(owner)>;
        static_assert(owner_type::template isRelation<Name>());
        using Relation = std::tuple_element_t<owner_type::template relationIndex<Name>(), typename E::Relations>;
        static_assert(!Relation::isCollection, "a loaded collection can be empty but cannot be NULL");
        auto& slot = owner.template relationSlot<Name>();
        slot.reset();
        slot.state = decltype(slot.state)::null;
    }
};

}  // namespace ruvia::detail
