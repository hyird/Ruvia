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
#include <vector>

#include "ruvia/web/detail/entity/value_storage.h"
#include "ruvia/web/detail/redis/redis_entity_traits.h"
#include "ruvia/web/detail/redis/redis_repository_commands.h"

namespace ruvia {
enum class redis_index_kind : std::uint8_t;
class redis_predicate;
struct redis_find_options;
struct redis_repository_config;
}  // namespace ruvia

namespace ruvia::detail {

// What the RediSearch compiler needs to know about one entity column.
struct redis_column_info final {
    std::string_view name_;
    redis_column_kind kind_{redis_column_kind::unsupported};
    bool primary_key_{false};
};

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

struct redis_index_definition final {
    std::pmr::string field_;
    redis_index_kind kind_{};
    bool sortable_{false};
};

// A repository's validated storage and index layout. Only
// normalize_redis_mapping builds one, so every index column exists, is a safe
// identifier outside the reserved __ruvia_ namespace, is unique, and has an
// index kind that fits its value kind. `columns_` and `entity_prefix_` view the
// entity's static schema: `entity_prefix_` is the declared prefix predicates
// are bound to, `prefix_` names the index and storage keys.
struct redis_mapping final {
    std::span<const redis_column_info> columns_;
    std::string_view entity_prefix_;
    std::pmr::string prefix_;
    std::pmr::vector<redis_index_definition> indexes_;

    [[nodiscard]] redis_index_kind index_kind(std::string_view column) const noexcept {
        for (const auto& index : indexes_) {
            if (index.field_ == column) {
                return index.kind_;
            }
        }
        return redis_index_kind{};
    }
    [[nodiscard]] bool sortable(std::string_view column) const noexcept {
        for (const auto& index : indexes_) {
            if (index.field_ == column) {
                return index.sortable_;
            }
        }
        return false;
    }
};

// `columns` and `entity_prefix` must outlive the mapping.
[[nodiscard]] redis_mapping normalize_redis_mapping(std::span<const redis_column_info> columns,
    std::string_view entity_prefix, const redis_repository_config& config, std::pmr::memory_resource* resource);

// A TAG value is kept in a shadow hash field. The leading x makes an empty
// value distinct from a missing field, and hexadecimal encoding keeps every
// byte outside RediSearch's TAG grammar.
[[nodiscard]] std::pmr::string encode_redis_tag(std::string_view value, std::pmr::memory_resource* resource);

[[nodiscard]] std::pmr::vector<std::pmr::string> compile_redis_find(const redis_mapping& mapping,
    const redis_find_options& options, std::pmr::memory_resource* resource, bool count_only = false,
    std::optional<std::uint64_t> take_override = {});

[[nodiscard]] std::pmr::vector<std::pmr::string> compile_redis_index(
    const redis_mapping& mapping, std::pmr::memory_resource* resource);

// The canonical ID when the predicate is exactly `primary key == literal`.
[[nodiscard]] std::optional<std::pmr::string> redis_primary_key(
    const redis_mapping& mapping, const redis_predicate& predicate, std::pmr::memory_resource* resource);

// Appends the write-script triples for one entity field: the value (or its
// deletion when absent), its presence marker and, for TAG-encodable columns,
// its TAG shadow. A value in a NUMERIC index must be exactly representable.
void append_redis_entity_field(redis_orm_arguments_type& args, std::string_view field,
    std::optional<std::string_view> value, bool numeric_index, bool tag_shadow, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
