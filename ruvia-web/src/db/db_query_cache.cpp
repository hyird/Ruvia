#include "db/db_query_cache.h"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

#include <openssl/sha.h>

#include "ruvia/http/http_hex.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_utils.h"
#include "ruvia/web/detail/db/db_value_access.h"

#include "db/db_cache_shape.h"
#include "db/db_config_storage.h"

namespace ruvia::detail {
namespace {
void number(std::pmr::string& out, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
        out.push_back(static_cast<char>((value >> (index * 8)) & 255));
    }
}
void text(std::pmr::string& out, std::string_view value) {
    number(out, value.size());
    out.append(value);
}
[[noreturn]] void invalid_cache() {
    throw db_conversion_error(db_conversion_error::code_type::invalid_format, "invalid database query cache entry");
}
struct reader {
    std::string_view bytes_;
    std::uint64_t number() {
        if (bytes_.size() < 8) {
            invalid_cache();
        }
        std::uint64_t value = 0;
        for (unsigned index = 0; index < 8; ++index) {
            value |= std::uint64_t(static_cast<unsigned char>(bytes_[index])) << (index * 8);
        }
        bytes_.remove_prefix(8);
        return value;
    }
    std::string_view text() {
        const auto size = number();
        if (size > bytes_.size()) {
            invalid_cache();
        }
        auto result_value = bytes_.substr(0, static_cast<std::size_t>(size));
        bytes_.remove_prefix(static_cast<std::size_t>(size));
        return result_value;
    }
};
std::pmr::string digest(std::string_view bytes_value, std::pmr::memory_resource* resource) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> hash{};
    if (!SHA256(reinterpret_cast<const unsigned char*>(bytes_value.data()), bytes_value.size(), hash.data())) {
        throw std::runtime_error("database cache key digest failed");
    }
    std::pmr::string result(resource);
    http_append_lower_hex(result, hash);
    return result;
}
}  // namespace

task<db_rows> db_cache_query::operator()(operation_options options) && {
    return execute_owned(backend_, std::move(sql_), std::move(params_), std::move(options));
}

task<db_rows> db_cache_query::execute_owned(db_query_backend backend, std::pmr::string sql,
    std::pmr::vector<db_value> params, operation_options options) {
    co_return co_await query_db_backend(backend, std::move(sql), std::move(params), options);
}

// Format RUVIAQC2: row count, column count, the column names once, then for
// every row a row marker followed, for every field, by a presence byte and its
// text when present. All rows of one result share a single schema, so the
// names are not repeated per row. The marker gives every row, including a
// zero-column one, a serialized cost, so a decoder bounds the row count by the
// entry size before it allocates.
constexpr char db_cache_row_marker = '\2';

std::pmr::string encode_db_cache_rows(const db_rows& rows, std::pmr::memory_resource* resource) {
    std::pmr::string out("RUVIAQC2", resource);
    number(out, rows.size());
    const auto names = rows.empty() ? std::span<const std::pmr::string>{}
                                    : db_result_access::column_names(rows.front());
    number(out, names.size());
    for (const auto& name : names) {
        text(out, name);
    }
    for (const auto& row : rows) {
        if (row.size() != names.size()) {
            invalid_cache();
        }
        out.push_back(db_cache_row_marker);
        for (std::size_t index = 0; index < row.size(); ++index) {
            const auto value = row[index].value();
            out.push_back(value ? '\1' : '\0');
            if (value) {
                text(out, *value);
            }
        }
    }
    return out;
}
db_rows decode_db_cache_rows(std::string_view bytes_value, std::pmr::memory_resource* resource) {
    if (!bytes_value.starts_with("RUVIAQC2")) {
        invalid_cache();
    }
    reader reader_value{bytes_value.substr(8)};
    const auto count = reader_value.number();
    const auto columns = reader_value.number();
    if (columns > reader_value.bytes_.size() / 8) {
        invalid_cache();
    }
    auto result_value = db_result_access::make_result(resource);
    auto& names = db_result_access::column_names(result_value);
    names.reserve(static_cast<std::size_t>(columns));
    for (std::uint64_t index = 0; index < columns; ++index) {
        names.emplace_back(reader_value.text());
    }
    // Every row takes at least its marker and one presence byte per field,
    // which also bounds count * columns by the remaining entry size.
    if (count > reader_value.bytes_.size() / (columns + 1)) {
        invalid_cache();
    }
    auto& fields_value = db_result_access::fields(result_value);
    auto& rows = db_result_access::rows(result_value);
    fields_value.reserve(static_cast<std::size_t>(count * columns));
    rows.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t row_index = 0; row_index < count; ++row_index) {
        if (reader_value.bytes_.empty() || reader_value.bytes_.front() != db_cache_row_marker) {
            invalid_cache();
        }
        reader_value.bytes_.remove_prefix(1);
        const auto row_start = fields_value.size();
        for (std::uint64_t index = 0; index < columns; ++index) {
            if (reader_value.bytes_.empty()) {
                invalid_cache();
            }
            const char present = reader_value.bytes_.front();
            reader_value.bytes_.remove_prefix(1);
            if (present == '\0') {
                fields_value.push_back(db_result_access::null_field(resource));
            } else if (present == '\1') {
                fields_value.push_back(db_result_access::owned_field(reader_value.text(), resource));
            } else {
                invalid_cache();
            }
        }
        // The field storage was reserved up front, so borrowed rows stay valid.
        rows.push_back(db_result_access::borrowed_row(fields_value.data() + row_start,
            static_cast<std::size_t>(columns), names.data(), names.size(), resource));
    }
    if (!reader_value.bytes_.empty()) {
        invalid_cache();
    }
    return result_value;
}
std::pmr::string db_cache_scope(std::string_view name_space, std::string_view alias,
    const db_config_storage& config, std::pmr::memory_resource* resource) {
    std::pmr::string identity(resource);
    text(identity, name_space);
    text(identity, alias);
    number(identity, static_cast<std::uint64_t>(config.driver_));
    text(identity, config.host_);
    number(identity, config.port_);
    text(identity, config.database_);
    text(identity, config.username_);
    text(identity, config.tls_.server_name_ ? std::string_view(*config.tls_.server_name_) : std::string_view{});
    return digest(identity, resource);
}
std::pmr::string db_cache_prefix(std::string_view name_space, std::pmr::memory_resource* resource) {
    std::pmr::string result_value("ruvia:qc:v3:", resource);
    result_value.append(digest(name_space, resource)).push_back(':');
    return result_value;
}
std::pmr::string db_cache_id_key(std::string_view name_space, std::string_view id, db_cache_shape shape,
    std::pmr::memory_resource* resource) {
    auto result_value = db_cache_prefix(name_space, resource);
    result_value.append("id:").append(db_cache_shape_tag(shape)).push_back(':');
    result_value.append(digest(id, resource));
    return result_value;
}
std::pmr::string db_cache_key(std::string_view name_space, std::string_view id, db_cache_shape shape,
    std::string_view sql, std::span<const db_value> params, db_driver driver, std::pmr::memory_resource* resource) {
    if (!id.empty()) {
        return db_cache_id_key(name_space, id, shape, resource);
    }
    auto result_value = db_cache_prefix(name_space, resource);
    std::pmr::string input(resource);
    number(input, static_cast<std::uint64_t>(driver));
    text(input, sql);
    number(input, params.size());
    for (const auto& value : params) {
        const auto type = db_value_access::type(value);
        input.push_back(static_cast<char>(type));
        switch (type) {
            case db_value_type::null:
                break;
            case db_value_type::string:
                text(input, db_value_access::text(value));
                break;
            case db_value_type::signed_value:
                number(input, static_cast<std::uint64_t>(db_value_access::signed_value(value)));
                break;
            case db_value_type::unsigned_value:
                number(input, db_value_access::unsigned_value(value));
                break;
            case db_value_type::double_value:
                number(input, std::bit_cast<std::uint64_t>(db_value_access::get_double_value(value)));
                break;
            case db_value_type::bool_value:
                number(input, db_value_access::get_bool_value(value));
                break;
        }
    }
    result_value.append("sql:").append(digest(input, resource));
    return result_value;
}
}  // namespace ruvia::detail
