#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include "ruvia/web/detail/entity/value_storage.h"
#include "ruvia/web/detail/model/traits.h"
#include "ruvia/web/detail/redis/redis_predicate_storage.h"
#include "ruvia/web/detail/redis/redis_repository_commands.h"
#include "ruvia/web/detail/redis/redis_repository_config.h"
#include "ruvia/web/redis/redis_find_options.h"

namespace ruvia::detail {

enum class redis_column_kind : std::uint8_t {
    unsupported,
    text,
    bool_value,
    signed_value,
    unsigned_value,
    float32,
    float64,
};

// What the RediSearch compiler needs to know about one entity column.
struct redis_column_info final {
    std::string_view name_;
    redis_column_kind kind_{redis_column_kind::unsupported};
    bool primary_key_{false};
};

template <typename t_type>
consteval redis_column_kind redis_column_value_kind() {
    using value_type = std::remove_cvref_t<t_type>;
    if constexpr (std::is_same_v<value_type, string> ||
                  std::is_same_v<value_type, std::pmr::string> ||
                  std::is_same_v<value_type, std::string>) {
        return redis_column_kind::text;
    } else if constexpr (std::is_same_v<value_type, bool_value> || std::is_same_v<value_type, bool>) {
        return redis_column_kind::bool_value;
    } else if constexpr (is_ruvia_scalar<value_type>) {
        return redis_column_value_kind<model_scalar_value_t_type<value_type>>();
    } else if constexpr (std::is_integral_v<value_type>) {
        if constexpr (std::is_signed_v<value_type>) {
            return redis_column_kind::signed_value;
        } else {
            return redis_column_kind::unsigned_value;
        }
    } else if constexpr (std::is_same_v<value_type, float>) {
        return redis_column_kind::float32;
    } else if constexpr (std::is_same_v<value_type, double>) {
        return redis_column_kind::float64;
    } else {
        return redis_column_kind::unsupported;
    }
}

// The per-entity column table every compiler entry point below consumes.
template <typename entity_type>
inline constexpr auto redis_query_columns = [] {
    std::array<redis_column_info, std::tuple_size_v<typename entity_type::columns_type>> columns{};
    for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t i> {
        columns[i] = {column_type::name.view(), redis_column_value_kind<typename column_type::value_type>(),
            column_type::options.primary_key_};
    });
    return columns;
}();

// A TAG value is kept in a shadow hash field. The leading x makes an empty
// value distinct from a missing field, and hexadecimal encoding keeps every
// byte outside RediSearch's TAG grammar.
[[nodiscard]] std::pmr::string encode_redis_tag(std::string_view value, std::pmr::memory_resource* resource);

// `entity_prefix` is the declared entity prefix that predicates are bound to;
// the mapping prefix names the index and storage keys.
[[nodiscard]] std::pmr::vector<std::pmr::string> compile_redis_find(std::span<const redis_column_info> columns,
    std::string_view entity_prefix, const redis_find_options& options, const redis_mapping& mapping,
    std::pmr::memory_resource* resource, bool count_only = false, std::optional<std::uint64_t> take_override = {});

[[nodiscard]] std::pmr::vector<std::pmr::string> compile_redis_index(std::span<const redis_column_info> columns,
    const redis_mapping& mapping, std::pmr::memory_resource* resource);

// The canonical ID when the predicate is exactly `primary key == literal`.
[[nodiscard]] std::optional<std::pmr::string> redis_primary_key(std::span<const redis_column_info> columns,
    std::string_view entity_prefix, const redis_predicate& predicate, std::pmr::memory_resource* resource);

// Appends the write-script triples for one entity field: the value (or its
// deletion when absent), its presence marker and, for TAG-encodable columns,
// its TAG shadow. A value in a NUMERIC index must be exactly representable.
void append_redis_entity_field(redis_orm_arguments_type& args, std::string_view field,
    std::optional<std::string_view> value, bool numeric_index, bool tag_shadow, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
