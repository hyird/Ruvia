#include "db/db_query_cache.h"

#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_value_access.h"

#include "db/db_config_storage.h"
#include "db/db_digest.h"
#include "util/pmr_owned_storage.h"

namespace ruvia::detail {
namespace {
void number(std::pmr::string& out, std::uint64_t value) {
    std::array<char, 8> encoded;
    for (unsigned index = 0; index < encoded.size(); ++index) {
        encoded[index] = static_cast<char>((value >> (index * 8)) & 255);
    }
    out.append(encoded.data(), encoded.size());
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

// Leads every entry, so a value another writer put under a cache key is
// refused before it is decoded.
constexpr std::string_view db_cache_entry_magic = "RUVIAQC";
constexpr std::string_view cache_prefix = "ruvia:qc:";
constexpr std::size_t digest_text_bytes = 2 * db_sha256::digest_size;

[[nodiscard]] std::pmr::string make_id_key_from_hex(std::string_view prefix, db_cache_shape shape,
    std::string_view digest_hex, std::pmr::memory_resource* resource) {
    const auto shape_tag = db_cache_shape_tag(shape);
    std::pmr::string result(resource);
    result.reserve(prefix.size() + 3 + shape_tag.size() + 1 + digest_hex.size());
    result.append(prefix).append("id:").append(shape_tag).push_back(':');
    result.append(digest_hex);
    return result;
}

[[nodiscard]] std::pmr::string make_id_key(std::string_view prefix, db_cache_shape shape,
    const db_sha256::digest_type& hash, std::pmr::memory_resource* resource) {
    std::array<char, digest_text_bytes> hex{};
    http_encode_lower_hex(hex.data(), hash);
    return make_id_key_from_hex(prefix, shape, std::string_view(hex.data(), hex.size()), resource);
}

void release_cache_storage(void* storage) noexcept {
    auto* const bytes = static_cast<std::pmr::string*>(storage);
    auto* const resource = bytes->get_allocator().resource();
    destroy_pmr_object(bytes, resource);
}

// SQL and parameters can be large, so their byte encoding streams into the
// digest instead of being copied into a temporary string first.
void digest_text(db_sha256& digest, std::string_view value) {
    digest.update_u64_le(value.size());
    digest.update(value);
}

}  // namespace

task<db_rows> db_cache_query::operator()(operation_options options) && {
    return query_db_backend(backend_, std::move(sql_), std::move(params_), options);
}

// Format: the entry magic, row count, column count, the column names once, then for
// every row a row marker followed, for every field, by a presence byte and its
// text when present. All rows of one result share a single schema, so the
// names are not repeated per row. The marker gives every row, including a
// zero-column one, a serialized cost, so a decoder bounds the row count by the
// entry size before it allocates.
constexpr char db_cache_row_marker = '\2';

std::pmr::string encode_db_cache_rows(const db_rows& rows, std::pmr::memory_resource* resource) {
    const auto names = rows.empty() ? std::span<const std::pmr::string>{}
                                    : db_result_access::column_names(rows.front());
    std::size_t total_bytes = db_cache_entry_magic.size() + 8U + 8U;
    for (const auto& name : names) {
        total_bytes += 8U + name.size();
    }
    for (const auto& row : rows) {
        if (row.size() != names.size()) {
            invalid_cache();
        }
        total_bytes += 1U + row.size();
        for (std::size_t index = 0; index < row.size(); ++index) {
            if (const auto value = row[index].value()) {
                total_bytes += 8U + value->size();
            }
        }
    }
    std::pmr::string out(resource);
    out.reserve(total_bytes);
    out.append(db_cache_entry_magic);
    number(out, rows.size());
    number(out, names.size());
    for (const auto& name : names) {
        text(out, name);
    }
    for (const auto& row : rows) {
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
db_rows decode_db_cache_rows(std::pmr::string bytes_value, std::pmr::memory_resource* resource) {
    if (!bytes_value.starts_with(db_cache_entry_magic)) {
        invalid_cache();
    }
    reader reader_value{std::string_view(bytes_value).substr(db_cache_entry_magic.size())};
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
    // Only fields can borrow the encoded storage. Empty results and
    // zero-column rows need no backing allocation after decoding.
    if (count != 0 && columns != 0) {
        // Rebind to the result's resource when Redis and the database use
        // different worker pools.
        const auto consumed = bytes_value.size() - reader_value.bytes_.size();
        auto* const result_resource = pmr_resource_or_default(resource);
        auto storage = make_pmr_object<std::pmr::string>(
            result_resource, rebind_owned_storage(bytes_value, result_resource));
        reader_value.bytes_ = std::string_view(*storage).substr(consumed);
        db_result_access::own_raw_result(result_value, storage.release(), &release_cache_storage);
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
                fields_value.push_back(db_result_access::borrowed_field(reader_value.text(), resource));
            } else {
                invalid_cache();
            }
        }
        // The field storage was reserved up front, so borrowed rows stay valid.
        rows.push_back(db_result_access::borrowed_row(
            fields_value.data() + row_start, static_cast<std::size_t>(columns), names.data(), names.size()));
    }
    if (!reader_value.bytes_.empty()) {
        invalid_cache();
    }
    return result_value;
}
std::pmr::string db_cache_prefix(std::string_view name_space, std::string_view alias,
    const db_config_storage& config, std::pmr::memory_resource* resource) {
    const auto server_name = config.tls_.server_name_ ? std::string_view(*config.tls_.server_name_) : std::string_view{};
    db_sha256 identity;
    digest_text(identity, name_space);
    digest_text(identity, alias);
    identity.update_u64_le(static_cast<std::uint64_t>(config.driver_));
    digest_text(identity, config.host_);
    identity.update_u64_le(config.port_);
    digest_text(identity, config.database_);
    digest_text(identity, config.username_);
    digest_text(identity, server_name);
    std::pmr::string result_value(resource);
    result_value.reserve(cache_prefix.size() + digest_text_bytes + 1);
    result_value.append(cache_prefix);
    http_append_lower_hex(result_value, identity.finish());
    result_value.push_back(':');
    return result_value;
}
void append_db_cache_id_keys(
    std::pmr::vector<std::pmr::string>& keys, std::string_view prefix, std::string_view id) {
    const auto hash = db_sha256_of(id);
    std::array<char, digest_text_bytes> hex{};
    http_encode_lower_hex(hex.data(), hash);
    const std::string_view hex_view(hex.data(), hex.size());
    auto* const resource = keys.get_allocator().resource();
    for (const auto shape : db_cache_shapes) {
        keys.push_back(make_id_key_from_hex(prefix, shape, hex_view, resource));
    }
}
std::pmr::string db_cache_key(std::string_view prefix, std::string_view id, db_cache_shape shape,
    std::string_view sql, std::span<const db_value> params, db_driver driver, std::pmr::memory_resource* resource) {
    if (!id.empty()) {
        return make_id_key(prefix, shape, db_sha256_of(id), resource);
    }
    std::pmr::string result_value(resource);
    result_value.reserve(prefix.size() + 4 + digest_text_bytes);
    result_value.append(prefix);
    db_sha256 input;
    input.update_u64_le(static_cast<std::uint64_t>(driver));
    digest_text(input, sql);
    input.update_u64_le(params.size());
    for (const auto& value : params) {
        const auto type = db_value_access::type(value);
        const auto type_byte = static_cast<char>(type);
        input.update({&type_byte, 1});
        switch (type) {
            case db_value_type::null:
                break;
            case db_value_type::string:
                digest_text(input, db_value_access::text(value));
                break;
            case db_value_type::signed_value:
                input.update_u64_le(static_cast<std::uint64_t>(db_value_access::signed_value(value)));
                break;
            case db_value_type::unsigned_value:
                input.update_u64_le(db_value_access::unsigned_value(value));
                break;
            case db_value_type::double_value:
                input.update_u64_le(std::bit_cast<std::uint64_t>(db_value_access::get_double_value(value)));
                break;
            case db_value_type::bool_value:
                input.update_u64_le(db_value_access::get_bool_value(value));
                break;
        }
    }
    result_value.append("sql:");
    http_append_lower_hex(result_value, input.finish());
    return result_value;
}
}  // namespace ruvia::detail
