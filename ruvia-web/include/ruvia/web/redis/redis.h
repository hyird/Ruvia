#pragma once

#include "ruvia/web/redis/redis_session.h"
#ifdef RUVIA_ENABLE_REDIS
#endif

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
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

#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/error.h"
#include "ruvia/web/model.h"

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

namespace ruvia {

template <typename entity_type, fixed_string name>
class redis_field_reference;

namespace detail {
struct redis_predicate_access;
}

// An owning Redis filter. Literal text is copied into the process resource when
// the filter is built, so the filter may outlive the strings used to build it.
// Compilation copies every argument into the schema mapping's memory resource.
// NUMERIC predicates retain literal precision: float operands keep float's
// stored encoding for float32 columns and widen for other numeric columns;
// double operands never narrow. Cast explicitly when float rounding is intended.
class redis_predicate final {
public:
    redis_predicate() noexcept = default;
    redis_predicate(const redis_predicate& other) {
        if (other.storage_) {
            storage_ = std::make_unique<detail::redis_predicate_storage>(*other.storage_);
        }
    }
    redis_predicate& operator=(const redis_predicate& other) {
        if (this != &other) {
            *this = redis_predicate(other);
        }
        return *this;
    }
    redis_predicate(redis_predicate&&) noexcept = default;
    redis_predicate& operator=(redis_predicate&&) noexcept = default;
    [[nodiscard]] bool empty() const noexcept {
        return storage_ == nullptr;
    }
    redis_predicate& and_where(const redis_predicate& other) {
        return combine(detail::redis_binary_operator::logical_and, other);
    }
    redis_predicate& or_where(const redis_predicate& other) {
        return combine(detail::redis_binary_operator::logical_or, other);
    }
    friend redis_predicate operator&&(redis_predicate left, const redis_predicate& right) {
        left.append(detail::redis_binary_operator::logical_and, right);
        return left;
    }
    friend redis_predicate operator||(redis_predicate left, const redis_predicate& right) {
        left.append(detail::redis_binary_operator::logical_or, right);
        return left;
    }
    [[nodiscard]] redis_predicate operator!() const {
        if (empty()) {
            throw std::invalid_argument("cannot negate an empty Redis predicate");
        }
        redis_predicate result(*this);
        auto& storage = *result.storage_;
        const auto operand = storage.root_;
        storage.root_ = storage.add(detail::redis_predicate_node::kind_type::logical_not);
        storage.nodes_[storage.root_].operands_ = {operand};
        return result;
    }

private:
    template <typename, fixed_string>
    friend class redis_field_reference;
    friend struct detail::redis_predicate_access;
    static redis_predicate create(std::size_t initial_capacity = 0) {
        redis_predicate result;
        result.storage_ = std::make_unique<detail::redis_predicate_storage>(initial_capacity);
        return result;
    }
    redis_predicate& combine(detail::redis_binary_operator operation, const redis_predicate& other) {
        if (other.empty()) {
            throw std::invalid_argument("cannot combine empty Redis predicates");
        }
        if (empty()) {
            *this = other;
        } else {
            append(operation, other);
        }
        return *this;
    }
    void append(detail::redis_binary_operator operation, const redis_predicate& right) {
        if (empty() || right.empty()) {
            throw std::invalid_argument("cannot combine empty Redis predicates");
        }
        auto& storage = *storage_;
        const auto left_root = storage.root_;
        const auto offset = storage.nodes_.size();
        const auto right_root = right.storage_->root_ + offset;
        const auto count = right.storage_->nodes_.size();
        detail::redis_predicate_node root_node(detail::redis_predicate_node::kind_type::binary);
        root_node.binary_ = operation;
        root_node.operands_ = {left_root, right_root};
        storage.nodes_.reserve(offset + count + 1);
        try {
            for (std::size_t index = 0; index < count; ++index) {
                storage.nodes_.emplace_back(right.storage_->nodes_[index], offset);
            }
            storage.nodes_.push_back(std::move(root_node));
        } catch (...) {
            while (storage.nodes_.size() > offset) {
                storage.nodes_.pop_back();
            }
            throw;
        }
        storage.root_ = storage.nodes_.size() - 1;
    }
    std::unique_ptr<detail::redis_predicate_storage> storage_;
};

struct redis_column_options final {
    bool primary_key_{false};
    bool nullable_{false};
};

template <fixed_string field_name, typename t_type, redis_column_options options_type = {}>
struct redis_column final {
    static_assert(detail::is_redis_entity_scalar<t_type>, "Redis columns require owning scalar values");
    static_assert(!options_type.primary_key_ || (detail::is_redis_entity_primary_key_scalar<t_type> && !options_type.nullable_),
        "Redis primary keys must be non-nullable strings or integers");
    static constexpr auto name = field_name;
    using value_type = t_type;
    static constexpr auto options = options_type;
};

namespace detail {
template <typename t_type>
struct is_redis_column : std::false_type {};
template <fixed_string name, typename t_type, redis_column_options options_type>
struct is_redis_column<redis_column<name, t_type, options_type>> : std::true_type {};

template <typename columns>
inline constexpr bool redis_columns = []<std::size_t... indices>(std::index_sequence<indices...>) {
    return (is_redis_column<std::tuple_element_t<indices, columns>>::value && ...);
}(std::make_index_sequence<std::tuple_size_v<columns>>{});

// Column names are unique through entity_value_storage.
template <fixed_string key_prefix, typename... columns>
struct redis_entity_storage final {
    static_assert((is_redis_column<columns>::value && ...),
        "Redis entities require RUVIA_REDIS_COLUMN descriptors");
    static_assert((std::size_t{columns::options.primary_key_} + ... + std::size_t{0}) == 1,
        "Redis entities require exactly one primary key");
    static constexpr auto prefix_value = key_prefix;
    using type = entity_value_storage<columns...>;
};

template <typename entity>
concept redis_entity_schema = requires(const entity& value) {
    typename entity::redis_entity_type;
    typename entity::columns_type;
    requires std::same_as<entity, typename entity::redis_entity_type>;
    requires redis_columns<typename entity::columns_type>;
    { entity::prefix() } -> std::convertible_to<std::string_view>;
};
}  // namespace detail

namespace detail {
struct redis_predicate_access final {
    // Null for an empty predicate.
    static const redis_predicate_storage* storage(const redis_predicate& predicate) noexcept {
        return predicate.storage_.get();
    }
};
}  // namespace detail

template <typename entity_type, fixed_string name>
class redis_field_reference final {
    static_assert(detail::redis_entity_schema<entity_type>, "Redis field references require a Redis entity");
    static_assert(entity_type::template column_index<name>() < std::tuple_size_v<typename entity_type::columns_type>);
    using operation = detail::redis_binary_operator;
    using kind = detail::redis_predicate_node::kind_type;

public:
    template <detail::redis_predicate_argument value_type>
    [[nodiscard]] redis_predicate operator==(value_type&& value) const {
        return compare(operation::equal, std::forward<value_type>(value));
    }
    template <detail::redis_predicate_argument value_type>
    [[nodiscard]] redis_predicate operator!=(value_type&& value) const {
        return compare(operation::not_equal, std::forward<value_type>(value));
    }
    template <detail::redis_predicate_argument value_type>
    [[nodiscard]] redis_predicate operator<(value_type&& value) const {
        return compare(operation::less, std::forward<value_type>(value));
    }
    template <detail::redis_predicate_argument value_type>
    [[nodiscard]] redis_predicate operator<=(value_type&& value) const {
        return compare(operation::less_equal, std::forward<value_type>(value));
    }
    template <detail::redis_predicate_argument value_type>
    [[nodiscard]] redis_predicate operator>(value_type&& value) const {
        return compare(operation::greater, std::forward<value_type>(value));
    }
    template <detail::redis_predicate_argument value_type>
    [[nodiscard]] redis_predicate operator>=(value_type&& value) const {
        return compare(operation::greater_equal, std::forward<value_type>(value));
    }
    [[nodiscard]] redis_predicate is_null() const {
        return compare(operation::equal, nullptr);
    }
    [[nodiscard]] redis_predicate is_not_null() const {
        return compare(operation::not_equal, nullptr);
    }
    template <detail::redis_predicate_argument lower_type, detail::redis_predicate_argument upper_type>
    [[nodiscard]] redis_predicate between(lower_type&& lower, upper_type&& upper) const {
        auto result_value = create_field(4);
        auto& storage = *result_value.storage_;
        const auto lower_node = storage.add_value(std::forward<lower_type>(lower));
        const auto upper_node = storage.add_value(std::forward<upper_type>(upper));
        storage.root_ = storage.add(kind::between);
        storage.nodes_[storage.root_].operands_ = {0, lower_node, upper_node};
        return result_value;
    }
    template <typename value_type>
        requires detail::redis_predicate_argument<const value_type&>
    [[nodiscard]] redis_predicate in(std::span<const value_type> values) const {
        return list(values, operation::in);
    }
    template <typename value_type>
        requires detail::redis_predicate_argument<const value_type&>
    [[nodiscard]] redis_predicate in(std::initializer_list<value_type> values) const {
        return in(std::span<const value_type>(values.begin(), values.size()));
    }
    template <typename value_type>
        requires detail::redis_predicate_argument<const value_type&>
    [[nodiscard]] redis_predicate not_in(std::span<const value_type> values) const {
        return list(values, operation::not_in);
    }
    template <typename value_type>
        requires detail::redis_predicate_argument<const value_type&>
    [[nodiscard]] redis_predicate not_in(std::initializer_list<value_type> values) const {
        return not_in(std::span<const value_type>(values.begin(), values.size()));
    }

private:
    static redis_predicate create_field(std::size_t initial_capacity) {
        auto result_value = redis_predicate::create(initial_capacity);
        auto& storage = *result_value.storage_;
        const auto index = storage.add(kind::field);
        storage.nodes_[index].field_ = name.view();
        storage.nodes_[index].entity_prefix_ = entity_type::prefix();
        return result_value;
    }
    template <detail::redis_predicate_argument value_type>
    static redis_predicate compare(operation op, value_type&& value) {
        auto result_value = create_field(3);
        auto& storage = *result_value.storage_;
        const auto literal = storage.add_value(std::forward<value_type>(value));
        storage.root_ = storage.add(kind::binary);
        storage.nodes_[storage.root_].binary_ = op;
        storage.nodes_[storage.root_].operands_ = {0, literal};
        return result_value;
    }
    template <typename value_type>
    static redis_predicate list(std::span<const value_type> values, operation op) {
        auto result_value = create_field(values.size() + 3);
        auto& storage = *result_value.storage_;
        std::vector<std::size_t> operands;
        operands.reserve(values.size());
        for (const auto& value : values) {
            operands.push_back(storage.add_value(value));
        }
        const auto list_node = storage.add(kind::list);
        storage.nodes_[list_node].operands_ = std::move(operands);
        storage.root_ = storage.add(kind::binary);
        storage.nodes_[storage.root_].binary_ = op;
        storage.nodes_[storage.root_].operands_ = {0, list_node};
        return result_value;
    }
};

}  // namespace ruvia

namespace ruvia {

#define RUVIA_REDIS_COLUMN(name, value_type, ...) ::ruvia::redis_column<::ruvia::fixed_string{#name}, value_type __VA_OPT__(, ) __VA_ARGS__>

// set<field>() 检查整数范围与有限浮点溢出；整数列不隐式接受浮点输入。
// 失败保留原值和 unset/null 状态；经可变 get() 或显式转换执行的赋值由调用方负责。
#define RUVIA_REDIS_ENTITY(name, key_prefix, ...)                                                                      \
    struct name final {                                                                                                \
    private:                                                                                                           \
        using descriptor_type = ::ruvia::detail::redis_entity_storage<::ruvia::fixed_string{key_prefix}, __VA_ARGS__>; \
        using storage_type = typename descriptor_type::type;                                                           \
        storage_type entity_;                                                                                          \
        RUVIA_DETAIL_ENTITY_OWNER_API(name, storage_type, entity_)                                                     \
        using redis_entity_type = name;                                                                                \
        using columns_type = typename storage_type::columns_type;                                                      \
        static constexpr std::string_view prefix() noexcept {                                                          \
            return descriptor_type::prefix_value.view();                                                               \
        }                                                                                                              \
        template <::ruvia::fixed_string field_name>                                                                    \
        static constexpr ::ruvia::redis_field_reference<name, field_name> column() noexcept {                          \
            return {};                                                                                                 \
        }                                                                                                              \
        RUVIA_DETAIL_ENTITY_VALUE_API(entity_)                                                                         \
    };

}  // namespace ruvia

namespace ruvia {

enum class redis_order_direction : std::uint8_t {
    asc,
    desc,
};

struct redis_find_order final {
    borrowed_text column_{};
    redis_order_direction direction_{redis_order_direction::asc};
};

// RediSearch always pages results; this page size applies when `take_` is unset.
inline constexpr std::uint64_t redis_find_default_take = 100;

// Redis filters and pagination have no SQL relation, lock or cache options.
// With `take_` unset, find() returns every match or throws redis_error
// (command_error) when more than redis_find_default_take entities match after
// `skip_`; it never truncates silently. find_and_count() returns one page of
// that size together with the total count.
struct redis_find_options final {
    redis_predicate where_{};
    std::optional<redis_find_order> order_{};
    std::optional<query_row_count> skip_{};
    std::optional<query_row_count> take_{};
};

enum class redis_index_kind : std::uint8_t {
    none,
    tag,
    text,
    numeric,
};

struct redis_index_config final {
    borrowed_text column_{};
    redis_index_kind kind_{redis_index_kind::tag};
    bool sortable_{false};
};

struct redis_repository_config final {
    borrowed_text prefix_{};
    std::vector<redis_index_config> indexes_;
};

enum class redis_search_mode : std::uint8_t {
    rows,
    first,
    count,
};

// Validated, owning schema configuration; no connection or worker is required.
// Only immutable entity descriptors have static lifetime. The supplied memory
// resource must outlive the mapping and commands compiled from it.
class redis_search_mapping final {
public:
    template <typename entity_type>
        requires detail::redis_entity_schema<entity_type>
    [[nodiscard]] static redis_search_mapping create(
        const redis_repository_config& config = {}) {
        return redis_search_mapping(detail::normalize_redis_mapping(
            detail::redis_query_columns<entity_type>, entity_type::prefix(), config));
    }

    redis_search_mapping(const redis_search_mapping& other);
    redis_search_mapping(redis_search_mapping&&) noexcept = default;
    redis_search_mapping& operator=(const redis_search_mapping& other);
    redis_search_mapping& operator=(redis_search_mapping&& other);

    // rows uses take_ or redis_find_default_take; first caps that page at one.
    // count validates all options but counts all matches, ignoring pagination.
    // Commands own all arguments in this mapping's resource.
    [[nodiscard]] redis_compiled_command compile_find(
        const redis_find_options& options = {}, redis_search_mode mode = redis_search_mode::rows) const;
    [[nodiscard]] redis_compiled_command compile_create_index() const;
    // Drops only the index, never its documents.
    [[nodiscard]] redis_compiled_command compile_drop_index() const;

private:
    template <typename>
    friend class redis_repository;
    explicit redis_search_mapping(detail::redis_mapping&& mapping) noexcept
        : mapping_(std::move(mapping)) {}
    void swap(redis_search_mapping& other) noexcept;
    void require_valid() const;

    detail::redis_mapping mapping_;
};

// Omitted expiration preserves an existing TTL; new entities are persistent.
struct redis_write_options final {
    std::optional<std::chrono::milliseconds> ttl_{};
    bool persist_{false};
};

class redis_write_result final {
public:
    explicit constexpr redis_write_result(std::uint64_t affected_entities) noexcept
        : affected_entities_(affected_entities) {}
    [[nodiscard]] constexpr std::uint64_t affected_entities() const noexcept {
        return affected_entities_;
    }

private:
    std::uint64_t affected_entities_{};
};

template <typename entity_type>
class redis_repository final {
    static_assert(detail::redis_entity_schema<entity_type>, "Redis repositories require a RUVIA_REDIS_ENTITY declaration");

public:
    redis_repository(const redis_repository&) = delete;
    redis_repository& operator=(const redis_repository&) = delete;

    // A repository is tied to its worker operation scope. Moving it transfers
    // its value registration and PMR mapping; copying would create a second
    // owner with ambiguous mapping lifetime.
    redis_repository(redis_repository&& other) noexcept
        : session_(other.session_),
          mapping_(std::move(other.mapping_)),
          registration_(std::move(other.registration_), this) {
        // The source no longer has a registration. Destroy moved-from
        // PMR containers now: implementations may retain allocator-owned state.
        other.mapping_.reset();
    }
    redis_repository& operator=(redis_repository&&) = delete;

    [[nodiscard]] task<redis_write_result> insert(
        const entity_type& entity, redis_write_options options = {}) const {
        detail::redis_scalar_scratch_type id_scratch;
        return write(write_mode::insert, detail::redis_entity_id(entity, id_scratch), entity, options);
    }

    [[nodiscard]] task<redis_write_result> upsert(
        const entity_type& entity, redis_write_options options = {}) const {
        detail::redis_scalar_scratch_type id_scratch;
        return write(write_mode::upsert, detail::redis_entity_id(entity, id_scratch), entity, options);
    }

    [[nodiscard]] task<redis_write_result> update(
        const redis_predicate& predicate, const entity_type& changes,
        redis_write_options options = {}) const {
        detail::redis_scalar_scratch_type id_scratch;
        const auto id = require_primary_key(predicate, id_scratch);
        return write(write_mode::update, id, changes, options);
    }

    [[nodiscard]] task<entity_rows<entity_type>> find(
        const redis_find_options& options = {}) const {
        auto args = mapping_->compile_find(options).take_arguments();
        detail::redis_map_many<entity_type> mapper{{mapping().storage_prefix_},
            options.take_.has_value()
                ? std::nullopt
                : std::optional<std::uint64_t>(options.skip_ ? options.skip_->value() : 0U)};
        return session_.template command_mapped<entity_rows<entity_type>>(std::move(args), std::move(mapper));
    }
    template <std::same_as<redis_predicate> predicate_type>
    [[nodiscard]] task<entity_rows<entity_type>> find(const predicate_type& where) const {
        return find(redis_find_options{.where_ = where});
    }

    [[nodiscard]] task<std::optional<entity_type>> find_one(
        const redis_find_options& options = {}) const {
        const bool plain = !options.order_.has_value() && !options.skip_ && !options.take_;
        if (plain) {
            detail::redis_scalar_scratch_type id_scratch;
            if (const auto id = detail::redis_primary_key(mapping(), options.where_, id_scratch)) {
                auto key = detail::redis_entity_key(mapping().storage_prefix_, *id);
                detail::redis_orm_arguments_type args;
                args.reserve(2);
                args.emplace_back("HGETALL");
                args.emplace_back(key);
                detail::redis_map_one<entity_type> mapper{
                    std::move(key), mapping().storage_prefix_.size()};
                return session_.template command_mapped<std::optional<entity_type>>(std::move(args), std::move(mapper));
            }
        }
        auto args = mapping_->compile_find(options, redis_search_mode::first).take_arguments();
        detail::redis_map_first<entity_type> mapper{{mapping().storage_prefix_}};
        return session_.template command_mapped<std::optional<entity_type>>(std::move(args), std::move(mapper));
    }

    template <std::same_as<redis_predicate> predicate_type>
    [[nodiscard]] task<std::optional<entity_type>> find_one(const predicate_type& where) const {
        return find_one(redis_find_options{.where_ = where});
    }
    [[nodiscard]] task<std::pair<entity_rows<entity_type>, std::uint64_t>> find_and_count(
        const redis_find_options& options = {}) const {
        auto args = mapping_->compile_find(options).take_arguments();
        detail::redis_map_search<entity_type> mapper{mapping().storage_prefix_};
        return session_.template command_mapped<std::pair<entity_rows<entity_type>, std::uint64_t>>(
            std::move(args), std::move(mapper));
    }

    [[nodiscard]] task<std::uint64_t> count(
        const redis_find_options& options = {}) const {
        return session_.template command_mapped<std::uint64_t>(
            mapping_->compile_find(options, redis_search_mode::count).take_arguments(), detail::redis_orm_count_result);
    }

    template <std::same_as<redis_predicate> predicate_type>
    [[nodiscard]] task<std::uint64_t> count(const predicate_type& where) const {
        return count(redis_find_options{.where_ = where});
    }

    [[nodiscard]] task<bool> exists(
        const redis_find_options& options = {}) const {
        if (!options.order_.has_value() && !options.skip_ && !options.take_) {
            detail::redis_scalar_scratch_type id_scratch;
            if (const auto id = detail::redis_primary_key(mapping(), options.where_, id_scratch)) {
                const auto key = detail::redis_entity_key(mapping().storage_prefix_, *id);
                return session_.exists(key);
            }
        }
        return session_.template command_mapped<bool>(
            mapping_->compile_find(options, redis_search_mode::count).take_arguments(),
            detail::redis_orm_exists_result);
    }

    template <std::same_as<redis_predicate> predicate_type>
    [[nodiscard]] task<bool> exists(const predicate_type& where) const {
        return exists(redis_find_options{.where_ = where});
    }

    [[nodiscard]] task<redis_write_result> delete_by(
        const redis_predicate& predicate) const {
        detail::redis_scalar_scratch_type id_scratch;
        const auto id = require_primary_key(predicate, id_scratch);
        return delete_by_id(id);
    }

    [[nodiscard]] task<redis_write_result> remove(const entity_type& entity) const {
        detail::redis_scalar_scratch_type id_scratch;
        return delete_by_id(detail::redis_entity_id(entity, id_scratch));
    }

    [[nodiscard]] task<bool> expire(
        const redis_predicate& predicate, std::chrono::seconds ttl) const {
        detail::redis_scalar_scratch_type id_scratch;
        const auto id = require_primary_key(predicate, id_scratch);
        const auto key = detail::redis_entity_key(mapping().storage_prefix_, id);
        return session_.expire(key, ttl);
    }

    [[nodiscard]] task<redis_ttl> ttl(const redis_predicate& predicate) const {
        detail::redis_scalar_scratch_type id_scratch;
        const auto id = require_primary_key(predicate, id_scratch);
        const auto key = detail::redis_entity_key(mapping().storage_prefix_, id);
        return session_.pttl(key);
    }

    // Explicit schema management. Repository acquisition never creates an
    // index, and an existing index is reported by Redis.
    [[nodiscard]] task<void> create_index() const {
        return session_.template command_mapped<void>(
            mapping_->compile_create_index().take_arguments(), detail::redis_orm_status_result);
    }

    [[nodiscard]] task<void> drop_index() const {
        return session_.template command_mapped<void>(
            mapping_->compile_drop_index().take_arguments(), detail::redis_orm_status_result);
    }

private:
    friend class redis_session;

    explicit redis_repository(const redis_session& session_value, const redis_repository_config& config)
        : session_(session_value),
          mapping_(redis_search_mapping::create<entity_type>(config)),
          registration_(session_value.registration_.scope(), this, &redis_repository::expire_capability) {}

    [[nodiscard]] const detail::redis_mapping& mapping() const {
        if (!mapping_) {
            throw std::logic_error("Redis repository mapping has expired");
        }
        return mapping_->mapping_;
    }

    static void expire_capability(void* target) noexcept {
        auto& repository = *static_cast<redis_repository*>(target);
        repository.mapping_.reset();
    }

    [[nodiscard]] std::string_view require_primary_key(
        const redis_predicate& predicate, std::span<char, detail::redis_scalar_scratch_size> scratch) const {
        const auto result_value = detail::redis_primary_key(mapping(), predicate, scratch);
        if (!result_value) {
            throw std::invalid_argument(
                "Redis repository operation requires an exact primary-key predicate");
        }
        return *result_value;
    }

    [[nodiscard]] task<redis_write_result> delete_by_id(
        std::string_view id) const {
        return session_.template script_mapped<redis_write_result>(detail::redis_orm_delete_script(),
            detail::redis_orm_delete_arguments(mapping().storage_prefix_, id), detail::redis_orm_delete_result);
    }

    enum class write_mode : std::uint8_t {
        insert,
        upsert,
        update,
    };

    // The write script's ARGV mode token.
    [[nodiscard]] static constexpr std::string_view write_mode_token(write_mode mode) {
        switch (mode) {
            case write_mode::insert:
                return "insert";
            case write_mode::upsert:
                return "upsert";
            case write_mode::update:
                return "update";
        }
        throw std::logic_error("invalid Redis write mode");
    }

    [[nodiscard]] task<redis_write_result> write(
        write_mode mode, std::string_view id, const entity_type& entity,
        redis_write_options options) const {
        static constexpr std::size_t required = [] {
            std::size_t count = 0;
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
                if constexpr (!column_type::options.nullable_) {
                    ++count;
                }
            });
            return count;
        }();
        static constexpr std::size_t max_mutation_arguments = [] {
            std::size_t count = 0;
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
                constexpr auto value_kind = detail::redis_column_value_kind<typename column_type::value_type>();
                constexpr bool tag_shadow =
                    value_kind == detail::redis_column_kind::text || value_kind == detail::redis_column_kind::bool_value;
                count += tag_shadow ? 9U : 6U;
            });
            return count;
        }();
        auto args = detail::redis_orm_write_arguments(
            mapping().storage_prefix_, id, write_mode_token(mode), options, 1 + required + max_mutation_arguments);
        detail::redis_scalar_scratch_type scalar_scratch;
        args.emplace_back(detail::encode_redis_scalar(required, scalar_scratch));
        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
            if constexpr (!column_type::options.nullable_) {
                args.emplace_back(column_type::name.view());
            }
        });

        const auto& column_index_kinds = mapping().column_index_kinds_;
        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t index> {
            constexpr auto value_kind = detail::redis_column_value_kind<typename column_type::value_type>();
            constexpr bool tag_shadow =
                value_kind == detail::redis_column_kind::text || value_kind == detail::redis_column_kind::bool_value;
            const auto field = column_type::name.view();
            const bool numeric_index = !tag_shadow && column_index_kinds[index] == redis_index_kind::numeric;
            if constexpr (column_type::options.primary_key_) {
                if (mode == write_mode::update && entity.template is_set<column_type::name>() &&
                    detail::encode_redis_scalar(entity.template get<column_type::name>(), scalar_scratch) != id) {
                    throw std::invalid_argument("an entity update cannot change its ID");
                }
                detail::append_redis_entity_field(args, field, id, numeric_index, tag_shadow);
            } else if (entity.template is_set<column_type::name>()) {
                if (entity.template is_null<column_type::name>()) {
                    detail::append_redis_entity_field(args, field, std::nullopt, false, tag_shadow);
                } else {
                    const auto value = detail::encode_redis_scalar(entity.template get<column_type::name>(), scalar_scratch);
                    detail::append_redis_entity_field(args, field, value, numeric_index, tag_shadow);
                }
            }
        });

        return session_.template script_mapped<redis_write_result>(
            detail::redis_orm_write_script(), std::move(args), detail::redis_orm_exec_result);
    }

    redis_session session_;
    std::optional<redis_search_mapping> mapping_;
    scoped_capability_registration registration_;
};

template <typename entity_type>
redis_repository<entity_type> redis_session::repository() const {
    return repository<entity_type>(redis_repository_config{});
}

template <typename entity_type>
redis_repository<entity_type> redis_session::repository(const redis_repository_config& config) const {
    registration_.require_active();
    return redis_repository<entity_type>(*this, config);
}

}  // namespace ruvia
