#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/web/detail/entity/value_storage.h"
#include "ruvia/web/detail/text_scalar.h"
#include "ruvia/web/entity_rows.h"
#include "ruvia/web/model.h"
#include "ruvia/web/redis/redis_types.h"

namespace ruvia {
enum class redis_index_kind : std::uint8_t;
class redis_predicate;
struct redis_find_options;
struct redis_repository_config;
struct redis_write_options;
class redis_write_result;
}  // namespace ruvia

namespace ruvia::detail {

class redis_pool;
struct request_io_metrics;

// Typed reply accessors that enforce the redis_value kind. A reply's type is
// chosen by the (untrusted) server, so a mismatch throws
// redis_error(protocol_error) rather than the raw accessors' std::logic_error,
// and an error reply throws redis_error(command_error).
[[nodiscard]] std::string_view redis_value_string(const redis_value& value);
[[nodiscard]] std::int64_t redis_value_integer(const redis_value& value);
[[nodiscard]] std::span<const redis_value> redis_value_array(const redis_value& value);
void throw_if_redis_error(const redis_value& value);

// Redis hash fields support owning scalar values and one string/integer key.
// Character types are text units, not numbers, so they are not Redis numbers.
template <typename t_type>
inline constexpr bool is_redis_entity_native_integer =
    is_model_integer<std::remove_cvref_t<t_type>>;

template <typename t_type>
inline constexpr bool is_redis_entity_native_float =
    std::is_same_v<std::remove_cvref_t<t_type>, float> ||
    std::is_same_v<std::remove_cvref_t<t_type>, double>;

template <typename t_type>
inline constexpr bool is_redis_entity_native_number =
    is_redis_entity_native_integer<t_type> || is_redis_entity_native_float<t_type>;

// The one classification of Redis column value types: entity validation,
// index validation, query compilation and writes all derive from it.
enum class redis_column_kind : std::uint8_t {
    unsupported,
    text,
    bool_value,
    signed_value,
    unsigned_value,
    float32,
    float64,
};

template <typename t_type>
consteval redis_column_kind redis_number_kind() {
    if constexpr (std::is_same_v<t_type, float>) {
        return redis_column_kind::float32;
    } else if constexpr (std::is_same_v<t_type, double>) {
        return redis_column_kind::float64;
    } else if constexpr (std::is_signed_v<t_type>) {
        return redis_column_kind::signed_value;
    } else {
        return redis_column_kind::unsigned_value;
    }
}

template <typename t_type>
consteval redis_column_kind redis_column_value_kind() {
    using value_type = std::remove_cvref_t<t_type>;
    if constexpr (std::is_same_v<value_type, string> || std::is_same_v<value_type, std::string>) {
        return redis_column_kind::text;
    } else if constexpr (std::is_same_v<value_type, bool>) {
        return redis_column_kind::bool_value;
    } else if constexpr (is_redis_entity_native_number<value_type>) {
        return redis_number_kind<value_type>();
    } else {
        return redis_column_kind::unsupported;
    }
}

template <typename t_type>
inline constexpr bool is_redis_entity_scalar = redis_column_value_kind<t_type>() != redis_column_kind::unsupported;

template <typename t_type>
inline constexpr bool is_redis_entity_primary_key_scalar =
    redis_column_value_kind<t_type>() == redis_column_kind::text ||
    redis_column_value_kind<t_type>() == redis_column_kind::signed_value ||
    redis_column_value_kind<t_type>() == redis_column_kind::unsigned_value;

[[noreturn]] inline void throw_invalid_redis_scalar(std::string_view message) {
    throw redis_error(redis_error::code_type::protocol_error, message);
}

inline constexpr std::size_t redis_scalar_scratch_size = 128;
using redis_scalar_scratch_type = std::array<char, redis_scalar_scratch_size>;

template <typename t_type>
[[nodiscard]] std::string_view encode_redis_number(
    t_type value, std::span<char, redis_scalar_scratch_size> scratch) {
    if constexpr (std::is_floating_point_v<t_type>) {
        if (!std::isfinite(value)) {
            throw_invalid_redis_scalar("non-finite Redis scalar");
        }
        const auto result_value = std::to_chars(scratch.data(), scratch.data() + scratch.size(), value,
            std::chars_format::general, std::numeric_limits<t_type>::max_digits10);
        if (result_value.ec != std::errc{}) {
            throw_invalid_redis_scalar("failed to encode Redis scalar");
        }
        return {scratch.data(), static_cast<std::size_t>(result_value.ptr - scratch.data())};
    } else if constexpr (std::is_unsigned_v<t_type>) {
        return http_format_unsigned(
            std::span<char, http_max_unsigned_decimal_size>(scratch.data(), http_max_unsigned_decimal_size), value);
    } else {
        return http_format_signed(
            std::span<char, http_max_signed_decimal_size>(scratch.data(), http_max_signed_decimal_size), value);
    }
}

template <typename t_type>
[[nodiscard]] std::string_view encode_redis_scalar(
    const t_type& value, std::span<char, redis_scalar_scratch_size> scratch) {
    using scalar_type = std::remove_cvref_t<t_type>;
    if constexpr (std::is_same_v<scalar_type, string>) {
        return value.view();
    } else if constexpr (std::is_same_v<scalar_type, std::string> ||
                         std::is_same_v<scalar_type, std::string> ||
                         std::is_same_v<scalar_type, std::string_view>) {
        return value;
    } else if constexpr (std::is_same_v<scalar_type, bool>) {
        return value ? std::string_view("1") : std::string_view("0");
    } else if constexpr (is_redis_entity_native_number<scalar_type>) {
        return encode_redis_number(value, scratch);
    } else {
        static_assert(always_false<scalar_type>, "unsupported Redis scalar type");
    }
}

template <typename t_type>
[[nodiscard]] t_type decode_redis_scalar(
    std::string_view value) {
    using scalar_type = std::remove_cvref_t<t_type>;
    if constexpr (std::is_same_v<scalar_type, string>) {
        string result_value;
        result_value.assign_owned(value);
        return result_value;
    } else if constexpr (std::is_same_v<scalar_type, std::string>) {
        return std::string(value.data(), value.size());
    } else if constexpr (std::is_same_v<scalar_type, bool>) {
        if (value == "0") {
            return false;
        }
        if (value == "1") {
            return true;
        }
        throw_invalid_redis_scalar("malformed Redis boolean scalar");
    } else if constexpr (is_redis_entity_native_number<scalar_type>) {
        const auto parsed = parse_text_scalar<scalar_type>(value);
        if (std::holds_alternative<text_scalar_parse_error>(parsed)) {
            throw_invalid_redis_scalar("malformed Redis scalar");
        }
        return std::get<scalar_type>(parsed);
    } else {
        static_assert(always_false<scalar_type>, "unsupported Redis scalar type");
    }
}

// Every ORM entity key is `redis_entity_key_root + lower-hex(prefix) + ':' + id`.
inline constexpr std::string_view redis_entity_key_root = "ruvia:orm:";

[[nodiscard]] inline std::string redis_entity_storage_prefix(
    std::string_view prefix) {
    std::string result;
    result.reserve(redis_entity_key_root.size() + prefix.size() * 2U + 1U);
    result.append(redis_entity_key_root);
    http_append_lower_hex(result, prefix);
    result.push_back(':');
    return result;
}

[[nodiscard]] inline std::string redis_entity_key(
    std::string_view storage_prefix, std::string_view id) {
    if (id.empty()) {
        throw std::invalid_argument("Redis entity ID must not be empty");
    }
    std::string key;
    key.reserve(storage_prefix.size() + id.size());
    key.append(storage_prefix);
    key.append(id);
    return key;
}

[[nodiscard]] inline bool redis_entity_key_matches(
    std::string_view storage_prefix, std::string_view id, std::string_view key) noexcept {
    return !id.empty() && key.size() == storage_prefix.size() + id.size() &&
           key.starts_with(storage_prefix) && key.substr(storage_prefix.size()) == id;
}

template <typename entity_type>
[[nodiscard]] std::optional<std::string_view> try_redis_entity_id(
    const entity_type& entity, std::span<char, redis_scalar_scratch_size> scratch) {
    std::optional<std::string_view> id;
    for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
        if constexpr (column_type::options.primary_key_) {
            if (entity.template is_set<column_type::name>() && !entity.template is_null<column_type::name>()) {
                const auto encoded = encode_redis_scalar(entity.template get<column_type::name>(), scratch);
                if (!encoded.empty()) {
                    id = encoded;
                }
            }
        }
    });
    return id;
}

template <typename entity_type>
[[nodiscard]] std::string_view redis_entity_id(
    const entity_type& entity, std::span<char, redis_scalar_scratch_size> scratch) {
    const auto id = try_redis_entity_id(entity, scratch);
    if (!id) {
        throw std::invalid_argument("Redis entity requires an ID");
    }
    return *id;
}

enum class redis_binary_operator : std::uint8_t { equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
    logical_and,
    logical_or,
    in,
    not_in };
enum class redis_literal_kind : std::uint8_t { null,
    string,
    signed_integer,
    unsigned_integer,
    float32,
    real,
    boolean };

// Literals own their text immediately. Only Redis scalar values are accepted;
// SQL expressions, statements and backend parameter types cannot enter this AST.
class redis_literal final {
    using storage_type = std::variant<std::monostate, std::string, std::int64_t, std::uint64_t, float, double, bool>;

public:
    redis_literal() noexcept = default;
    explicit redis_literal(std::nullptr_t) noexcept {}
    explicit redis_literal(std::string_view value)
        : storage_(std::in_place_type<std::string>, value) {}
    explicit redis_literal(const char* value)
        : redis_literal(value ? std::string_view(value) : throw std::invalid_argument("Redis predicate text must not be null")) {}
    template <typename value_type>
        requires std::is_same_v<value_type, bool>
    explicit redis_literal(value_type value)
        : storage_(value) {}
    template <typename value_type>
        requires is_model_integer<value_type>
    explicit redis_literal(value_type value) {
        if constexpr (std::is_signed_v<value_type>) {
            storage_.emplace<std::int64_t>(value);
        } else {
            storage_.emplace<std::uint64_t>(value);
        }
    }
    template <typename value_type>
        requires std::is_floating_point_v<value_type>
    explicit redis_literal(value_type value) {
        if constexpr (std::is_same_v<value_type, float>) {
            storage_.emplace<float>(value);
        } else {
            storage_.emplace<double>(checked_model_float<double>(value));
        }
    }
    explicit redis_literal(const string& value)
        : redis_literal(value.view()) {}
    redis_literal(redis_literal&&) noexcept = default;
    redis_literal& operator=(redis_literal&&) = delete;
    redis_literal(const redis_literal&) = default;
    redis_literal& operator=(const redis_literal&) = delete;

    redis_literal_kind kind() const noexcept {
        return static_cast<redis_literal_kind>(storage_.index());
    }
    std::string_view text() const noexcept {
        return std::get<std::string>(storage_);
    }
    std::int64_t signed_value() const noexcept {
        return std::get<std::int64_t>(storage_);
    }
    std::uint64_t unsigned_value() const noexcept {
        return std::get<std::uint64_t>(storage_);
    }
    float float32_value() const noexcept {
        return std::get<float>(storage_);
    }
    double real_value() const noexcept {
        return std::get<double>(storage_);
    }
    bool boolean_value() const noexcept {
        return std::get<bool>(storage_);
    }

private:
    storage_type storage_;
};

template <typename value_type>
concept redis_predicate_argument = requires(value_type&& value) {
    redis_literal(std::forward<value_type>(value));
};

// Only redis_field_reference creates leaves, and only redis_predicate combines
// them, so the tree has exactly these shapes: a comparison or IN is a binary
// node {field, literal} or {field, list}, BETWEEN is {field, lower, upper}, a
// list holds literals, and logical AND/OR is a binary node of two predicates.
struct redis_predicate_node final {
    enum class kind_type : std::uint8_t { field,
        literal,
        binary,
        between,
        list,
        logical_not };

    explicit redis_predicate_node(kind_type kind) noexcept
        : kind_(kind) {}
    explicit redis_predicate_node(redis_literal&& value) noexcept
        : kind_(kind_type::literal),
          literal_(std::move(value)) {}
    redis_predicate_node(const redis_predicate_node& other, std::size_t offset = 0)
        : kind_(other.kind_),
          binary_(other.binary_),
          field_(other.field_),
          entity_prefix_(other.entity_prefix_),
          literal_(other.literal_),
          operands_(other.operands_) {
        for (auto& operand : operands_) {
            operand += offset;
        }
    }
    redis_predicate_node(redis_predicate_node&&) noexcept = default;
    redis_predicate_node& operator=(redis_predicate_node&&) = delete;
    redis_predicate_node& operator=(const redis_predicate_node&) = delete;
    kind_type kind_;
    redis_binary_operator binary_{redis_binary_operator::equal};
    std::string_view field_{};
    std::string_view entity_prefix_{};
    redis_literal literal_;
    std::vector<std::size_t> operands_;
};

struct redis_predicate_storage final {
    explicit redis_predicate_storage(std::size_t initial_capacity = 0) {
        if (initial_capacity != 0) {
            nodes_.reserve(initial_capacity);
        }
    }
    redis_predicate_storage(const redis_predicate_storage&) = default;
    std::vector<redis_predicate_node> nodes_;
    std::size_t root_{};
    std::size_t add(redis_predicate_node::kind_type kind) {
        nodes_.emplace_back(kind);
        return nodes_.size() - 1;
    }
    template <redis_predicate_argument value_type>
    std::size_t add_value(value_type&& value) {
        redis_literal literal(std::forward<value_type>(value));
        nodes_.emplace_back(std::move(literal));
        return nodes_.size() - 1;
    }
};

using redis_orm_arguments_type = std::vector<std::string>;

// An ORM Lua script and the lowercase hex SHA1 Redis caches it under.
struct redis_orm_script final {
    std::string_view body_;
    std::string_view sha1_;
};

[[nodiscard]] const redis_orm_script& redis_orm_write_script();
[[nodiscard]] const redis_orm_script& redis_orm_delete_script();

// Both argument lists start with EVALSHA and the script digest.
[[nodiscard]] redis_orm_arguments_type redis_orm_write_arguments(std::string_view storage_prefix, std::string_view id,
    std::string_view mode, redis_write_options options, std::size_t extra_arguments);
[[nodiscard]] redis_orm_arguments_type redis_orm_delete_arguments(
    std::string_view storage_prefix, std::string_view id);
// Runs an EVALSHA argument list for `script`; a NOSCRIPT reply loads the body
// through EVAL once within the same operation. `io_metrics` is the request's
// counter block, or null outside a request.
[[nodiscard]] task<redis_value> execute_redis_orm_script(redis_pool& pool, const redis_orm_script& script,
    redis_orm_arguments_type args, request_io_metrics* io_metrics, operation_options options);
[[nodiscard]] redis_write_result redis_orm_exec_result(redis_value&& reply);
[[nodiscard]] redis_write_result redis_orm_delete_result(redis_value&& reply);
void redis_orm_status_result(redis_value&& reply);
[[nodiscard]] std::uint64_t redis_orm_count(const redis_value& reply);
[[nodiscard]] std::uint64_t redis_orm_count_result(redis_value&& reply);
[[nodiscard]] bool redis_orm_exists_result(redis_value&& reply);

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
    std::string column_;
    redis_index_kind kind_{};
    bool sortable_{false};
};

// A repository's validated storage and index layout. Only
// normalize_redis_mapping builds one, so every index column exists, is a safe
// identifier outside the reserved __ruvia_ namespace, is unique, and has an
// index kind that fits its value kind. `columns_` views the entity's static
// schema; `entity_prefix_` owns the declared prefix predicates are bound to,
// `prefix_` names the index, and `storage_prefix_` is the precomputed
// `"ruvia:orm:<hex(prefix_)>:"` hash-key prefix.
// `column_index_kinds_[i]` is the index kind of `columns_[i]` (none when the
// column is not indexed), so writes address it by descriptor index.
struct redis_mapping final {
    std::span<const redis_column_info> columns_;
    std::string entity_prefix_;
    std::string prefix_;
    std::string storage_prefix_;
    std::vector<redis_index_definition> indexes_;
    std::vector<redis_index_kind> column_index_kinds_;

    [[nodiscard]] redis_index_kind index_kind(std::string_view column) const noexcept {
        for (const auto& index : indexes_) {
            if (index.column_ == column) {
                return index.kind_;
            }
        }
        return redis_index_kind{};
    }
    [[nodiscard]] bool sortable(std::string_view column) const noexcept {
        for (const auto& index : indexes_) {
            if (index.column_ == column) {
                return index.sortable_;
            }
        }
        return false;
    }
};

// `columns` must be the entity's static descriptor table.
[[nodiscard]] redis_mapping normalize_redis_mapping(std::span<const redis_column_info> columns,
    std::string_view entity_prefix, const redis_repository_config& config);

// A TAG value is kept in a shadow hash field. The leading x makes an empty
// value distinct from a missing field, and hexadecimal encoding keeps every
// byte outside RediSearch's TAG grammar.
[[nodiscard]] std::string encode_redis_tag(std::string_view value);

// The canonical ID when the predicate is exactly `primary key == literal`.
[[nodiscard]] std::optional<std::string_view> redis_primary_key(
    const redis_mapping& mapping, const redis_predicate& predicate,
    std::span<char, redis_scalar_scratch_size> scratch);

// Appends the write-script triples for one entity field: the value (or its
// deletion when absent), its presence marker and, for TAG-encodable columns,
// its TAG shadow. A value in a NUMERIC index must be exactly representable.
void append_redis_entity_field(redis_orm_arguments_type& args, std::string_view field,
    std::optional<std::string_view> value, bool numeric_index, bool tag_shadow);

template <typename entity_type>
entity_type decode_redis_entity(std::span<const redis_value> fields_value, std::string_view key, std::string_view storage_prefix) {
    if (fields_value.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid entity hash reply");
    }
    std::array<std::optional<std::string_view>, std::tuple_size_v<typename entity_type::columns_type>> values{};
    bool marker = false;
    for (std::size_t i = 0; i < fields_value.size(); i += 2) {
        const auto name = redis_value_string(fields_value[i]);
        const auto value = redis_value_string(fields_value[i + 1]);
        if (name.starts_with("__ruvia_")) {
            if (name == "__ruvia_entity") {
                if (marker || value != "1") {
                    throw redis_error(redis_error::code_type::protocol_error, "invalid entity storage marker");
                }
                marker = true;
            }
            continue;
        }
        bool matched = false;
        for_each_descriptor<typename entity_type::columns_type>([&]<typename field_type, std::size_t index> {
            if (!matched && name == field_type::name.view()) {
                if (values[index].has_value()) {
                    throw redis_error(redis_error::code_type::protocol_error, "duplicate entity field");
                }
                values[index] = value;
                matched = true;
            }
        });
    }
    if (!marker) {
        throw redis_error(redis_error::code_type::protocol_error, "entity storage marker missing");
    }
    entity_type entity;
    for_each_descriptor<typename entity_type::columns_type>([&]<typename field_type, std::size_t index> {
        if (values[index].has_value()) {
            entity.template set<field_type::name>(
                decode_redis_scalar<typename field_type::value_type>(*values[index]));
        } else if constexpr (field_type::options.nullable_) {
            entity.template set_null<field_type::name>();
        } else {
            throw redis_error(redis_error::code_type::protocol_error, "required entity field missing from hash");
        }
    });
    redis_scalar_scratch_type id_scratch;
    const auto id = try_redis_entity_id(entity, id_scratch);
    if (!id) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid stored entity ID");
    }
    if (!redis_entity_key_matches(storage_prefix, *id, key)) {
        throw redis_error(redis_error::code_type::protocol_error, "entity ID does not match its Redis key");
    }
    return entity;
}

template <typename entity_type>
entity_type decode_redis_entity(const redis_value& reply, std::string_view key, std::string_view storage_prefix) {
    return decode_redis_entity<entity_type>(redis_value_array(reply), key, storage_prefix);
}

template <typename entity_type>
struct redis_map_one final {
    std::string key_;
    std::size_t storage_prefix_size_{0};
    std::optional<entity_type> operator()(redis_value&& reply) const {
        const auto fields_value = redis_value_array(reply);
        if (fields_value.empty()) {
            return std::nullopt;
        }
        return decode_redis_entity<entity_type>(
            fields_value, key_, std::string_view(key_).substr(0, storage_prefix_size_));
    }
};

template <typename entity_type>
struct redis_map_search final {
    std::string_view storage_prefix_;
    std::pair<entity_rows<entity_type>, std::uint64_t> decode(
        std::span<const redis_value> values) const {
        if (values.empty() || values.size() % 2 != 1) {
            throw redis_error(redis_error::code_type::protocol_error, "invalid entity search reply");
        }
        const auto count = redis_orm_count(values.front());
        const auto documents = (values.size() - 1) / 2;
        if (count < documents) {
            throw redis_error(redis_error::code_type::protocol_error, "invalid entity search count");
        }
        entity_rows<entity_type> rows;
        rows.reserve(documents);
        for (std::size_t i = 1; i < values.size(); i += 2) {
            const auto key = redis_value_string(values[i]);
            // A document may expire between index matching and field loading.
            if (values[i + 1].null()) {
                continue;
            }
            rows.push_back(decode_redis_entity<entity_type>(values[i + 1], key, storage_prefix_));
        }
        return {std::move(rows), count};
    }
    std::pair<entity_rows<entity_type>, std::uint64_t> operator()(
        redis_value&& reply) const {
        return decode(redis_value_array(reply));
    }
};

template <typename entity_type>
struct redis_map_many final {
    redis_map_search<entity_type> search_;
    // Set when the caller left `take_` unset: the reply must then hold every
    // match after this skip, or find() would silently truncate.
    std::optional<std::uint64_t> unpaged_skip_;
    entity_rows<entity_type> operator()(
        redis_value&& reply) const {
        const auto values = redis_value_array(reply);
        const auto documents = values.size() / 2;
        auto [rows, count] = search_.decode(values);
        if (unpaged_skip_.has_value() && count > *unpaged_skip_ && count - *unpaged_skip_ > documents) {
            throw redis_error(redis_error::code_type::command_error,
                "redis find() matched more entities than one default page; set take_ to page the results");
        }
        return std::move(rows);
    }
};

template <typename entity_type>
struct redis_map_first final {
    redis_map_search<entity_type> search_;
    std::optional<entity_type> operator()(redis_value&& reply) const {
        auto result_value = search_(std::move(reply));
        if (result_value.first.empty()) {
            return std::nullopt;
        }
        if (result_value.first.size() != 1) {
            throw redis_error(redis_error::code_type::protocol_error, "multiple entities in find_one reply");
        }
        return std::move(result_value.first[0]);
    }
};

}  // namespace ruvia::detail
