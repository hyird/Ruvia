#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/web/db/db_entity.h"

namespace ruvia::detail {

template <typename tuple_type, typename function_type>
constexpr void for_each_db_descriptor(function_type&& function) {
    [&]<std::size_t... i>(std::index_sequence<i...>) {
        (function.template operator()<std::tuple_element_t<i, tuple_type>, i>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<tuple_type>>{});
}

template <typename entity_type, fixed_string name>
consteval std::size_t db_relation_index() {
    std::size_t result_value = std::tuple_size_v<typename entity_type::relations_type>;
    for_each_db_descriptor<typename entity_type::relations_type>([&]<typename relation_type, std::size_t i> {
        if constexpr (relation_type::name == name) {
            result_value = i;
        }
    });
    return result_value;
}

template <typename entity_type, fixed_string name>
struct db_relation_named final {
    static constexpr auto index = db_relation_index<entity_type, name>();
    static_assert(index < std::tuple_size_v<typename entity_type::relations_type>, "unknown inverse database relation");
    using type_type = std::tuple_element_t<index, typename entity_type::relations_type>;
};

template <typename tuple_type>
struct reverse_db_join_columns;
template <typename... columns_type>
struct reverse_db_join_columns<std::tuple<columns_type...>> final {
    using type_type = std::tuple<db_join_column<columns_type::referenced, columns_type::local>...>;
};

// One normalized mapping is consumed by schema generation and query planning.
// Target metadata is deliberately inspected only when a mapping is consumed,
// so mutually recursive and self-referential entity declarations stay valid.
template <typename source, typename relation_type,
    db_relation_kind kind = relation_type::kind, bool owning = relation_type::is_owning>
struct db_relation_mapping;

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::many_to_one, true> {
    static constexpr bool through_join_table = false;
    using join_columns_type = typename relation_type::join_columns_type;
};

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::one_to_one, true>
    : db_relation_mapping<source, relation_type, db_relation_kind::many_to_one, true> {};

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::one_to_many, false> {
    using inverse_type = typename db_relation_named<typename relation_type::target_entity_type, relation_type::inverse_name>::type_type;
    static_assert(inverse_type::kind == db_relation_kind::many_to_one && inverse_type::is_owning,
        "one-to-many must refer to an owning many-to-one relation");
    static_assert(std::is_same_v<typename inverse_type::target_entity_type, source>,
        "inverse database relation must target its source entity");
    static constexpr bool through_join_table = false;
    using join_columns_type = typename reverse_db_join_columns<typename inverse_type::join_columns_type>::type_type;
};

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::one_to_one, false> {
    using inverse_type = typename db_relation_named<typename relation_type::target_entity_type, relation_type::inverse_name>::type_type;
    static_assert(inverse_type::kind == db_relation_kind::one_to_one && inverse_type::is_owning,
        "inverse one-to-one must refer to an owning one-to-one relation");
    static_assert(std::is_same_v<typename inverse_type::target_entity_type, source>,
        "inverse database relation must target its source entity");
    static constexpr bool through_join_table = false;
    using join_columns_type = typename reverse_db_join_columns<typename inverse_type::join_columns_type>::type_type;
};

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::many_to_many, true> {
    static constexpr bool through_join_table = true;
    using table = typename relation_type::join_table_type;
    using source_columns_type = typename table::owner_columns_type;
    using target_columns_type = typename table::inverse_columns_type;
    static constexpr std::string_view table_name() noexcept {
        return table::name.view();
    }
};

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::many_to_many, false> {
    using inverse_type = typename db_relation_named<typename relation_type::target_entity_type, relation_type::inverse_name>::type_type;
    static_assert(inverse_type::kind == db_relation_kind::many_to_many && inverse_type::is_owning,
        "inverse many-to-many must refer to an owning many-to-many relation");
    static_assert(std::is_same_v<typename inverse_type::target_entity_type, source>,
        "inverse database relation must target its source entity");
    static constexpr bool through_join_table = true;
    using table = typename inverse_type::join_table_type;
    using source_columns_type = typename table::inverse_columns_type;
    using target_columns_type = typename table::owner_columns_type;
    static constexpr std::string_view table_name() noexcept {
        return table::name.view();
    }
};

template <typename columns_type>
consteval bool unique_db_join_columns() {
    constexpr auto count = std::tuple_size_v<columns_type>;
    std::array<std::string_view, count> local{}, referenced{};
    for_each_db_descriptor<columns_type>([&]<typename column_type, std::size_t i> {
        local[i] = column_type::local.view();
        referenced[i] = column_type::referenced.view();
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

template <typename source, typename relation_type>
constexpr void validate_db_relation() {
    using target_type = typename relation_type::target_entity_type;
    using mapping_type = db_relation_mapping<source, relation_type>;
    if constexpr (mapping_type::through_join_table) {
        using owner = typename mapping_type::source_columns_type;
        using other_type = typename mapping_type::target_columns_type;
        static_assert(std::tuple_size_v<owner> > 0 && std::tuple_size_v<other_type> > 0,
            "a join table requires both owner and inverse columns");
        static_assert(unique_db_join_columns<owner>() && unique_db_join_columns<other_type>(),
            "join table column mappings must be nonempty and unique");
        static_assert([] {
            bool unique = true;
            for_each_db_descriptor<owner>([&]<typename left_type, std::size_t> {
                for_each_db_descriptor<other_type>([&]<typename right_type, std::size_t> {
                    unique &= left_type::local.view() != right_type::local.view();
                });
            });
            return unique;
        }(),
            "join table owner and inverse columns must have distinct names");
        for_each_db_descriptor<owner>([]<typename column_type, std::size_t> {
            (void)source::template column_index<column_type::referenced>();
        });
        for_each_db_descriptor<other_type>([]<typename column_type, std::size_t> {
            (void)target_type::template column_index<column_type::referenced>();
        });
    } else {
        using columns_type = typename mapping_type::join_columns_type;
        static_assert(std::tuple_size_v<columns_type> > 0, "a database relation requires join columns");
        static_assert(unique_db_join_columns<columns_type>(), "database relation column mappings must be nonempty and unique");
        for_each_db_descriptor<columns_type>([]<typename column_type, std::size_t> {
            (void)source::template column_index<column_type::local>();
            (void)target_type::template column_index<column_type::referenced>();
        });
    }
}

template <typename entity_type>
consteval std::size_t db_primary_key_count() {
    std::size_t count = 0;
    for_each_db_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
        count += column_type::options.primary_key_ ? 1 : 0;
    });
    return count;
}

template <typename entity_type>
consteval auto db_primary_key_columns() {
    std::array<std::string_view, db_primary_key_count<entity_type>()> result_value{};
    std::size_t i = 0;
    for_each_db_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
        if constexpr (column_type::options.primary_key_) {
            result_value[i++] = column_type::name.view();
        }
    });
    return result_value;
}

}  // namespace ruvia::detail
