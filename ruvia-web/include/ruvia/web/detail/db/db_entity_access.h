#pragma once

#include "ruvia/web/db/db_entity.h"

namespace ruvia::detail {

template <typename e_type>
struct db_entity_access final {
    static auto& storage(e_type& entity) noexcept {
        return entity.sql_storage();
    }
    template <fixed_string name>
    static auto& emplace_relation(e_type& entity) {
        auto& owner_value = storage(entity);
        using owner_type = std::remove_reference_t<decltype(owner_value)>;
        static_assert(owner_type::template is_relation<name>());
        auto& slot = owner_value.template relation_slot<name>();
        using relation_type = std::tuple_element_t<owner_type::template relation_index<name>(), typename e_type::relations_type>;
        using target_type = typename relation_type::target_entity_type;
        static_assert(!relation_type::is_collection, "emplace_relation is only valid for to-one relations");
        std::pmr::polymorphic_allocator<target_type> allocator(entity.resource());
        auto* value = allocator.template new_object<target_type>(entity.resource());
        slot.value_.reset(value);
        slot.state_ = decltype(slot.state_)::value;
        return *slot.value_;
    }

    template <fixed_string name>
    static auto& ensure_relation_collection(e_type& entity) {
        auto& owner_value = storage(entity);
        using owner_type = std::remove_reference_t<decltype(owner_value)>;
        static_assert(owner_type::template is_relation<name>());
        auto& slot = owner_value.template relation_slot<name>();
        using relation_type = std::tuple_element_t<owner_type::template relation_index<name>(), typename e_type::relations_type>;
        static_assert(relation_type::is_collection, "ensure_relation_collection requires a collection relation");
        if (slot.state_ != decltype(slot.state_)::value) {
            using stored_type = typename std::remove_reference_t<decltype(slot)>::stored_type;
            std::pmr::polymorphic_allocator<stored_type> allocator(entity.resource());
            slot.value_.reset(allocator.template new_object<stored_type>(entity.resource()));
            slot.state_ = decltype(slot.state_)::value;
        }
        return *slot.value_;
    }

    template <fixed_string name>
    static void set_relation_null(e_type& entity) {
        auto& owner_value = storage(entity);
        using owner_type = std::remove_reference_t<decltype(owner_value)>;
        static_assert(owner_type::template is_relation<name>());
        using relation_type = std::tuple_element_t<owner_type::template relation_index<name>(), typename e_type::relations_type>;
        static_assert(!relation_type::is_collection, "a loaded collection can be empty but cannot be NULL");
        auto& slot = owner_value.template relation_slot<name>();
        slot.reset();
        slot.state_ = decltype(slot.state_)::null;
    }
};

}  // namespace ruvia::detail
