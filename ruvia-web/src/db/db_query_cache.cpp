#include "db/db_query_cache.h"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_utils.h"
#include "ruvia/web/detail/db/db_value_access.h"

#include "db/db_config_storage.h"
#include "util/pmr_owned_storage.h"

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
[[nodiscard]] std::array<unsigned char, SHA256_DIGEST_LENGTH> cache_digest(std::string_view bytes_value) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> hash{};
    if (!SHA256(reinterpret_cast<const unsigned char*>(bytes_value.data()), bytes_value.size(), hash.data())) {
        throw std::runtime_error("database cache key digest failed");
    }
    return hash;
}
void append_digest(std::pmr::string& output, std::string_view bytes_value) {
    const auto hash = cache_digest(bytes_value);
    http_append_lower_hex(output, hash);
}
constexpr std::string_view cache_prefix = "ruvia:qc:v3:";
constexpr std::size_t digest_text_bytes = 2 * SHA256_DIGEST_LENGTH;
constexpr std::size_t full_prefix_bytes = cache_prefix.size() + digest_text_bytes + 1;

[[nodiscard]] std::pmr::string make_id_key(std::string_view prefix, db_cache_shape shape,
    const std::array<unsigned char, SHA256_DIGEST_LENGTH>& hash, std::pmr::memory_resource* resource) {
    const auto shape_tag = db_cache_shape_tag(shape);
    std::pmr::string result(resource);
    result.reserve(prefix.size() + 3 + shape_tag.size() + 1 + digest_text_bytes);
    result.append(prefix).append("id:").append(shape_tag).push_back(':');
    http_append_lower_hex(result, hash);
    return result;
}

void release_cache_storage(void* storage) noexcept {
    auto* const bytes = static_cast<std::pmr::string*>(storage);
    auto* const resource = bytes->get_allocator().resource();
    destroy_pmr_object(bytes, resource);
}

// SQL and parameters can be large. Feed the existing byte encoding directly
// into SHA-256 instead of copying the entire input into a temporary string.
class cache_key_hasher final {
public:
    cache_key_hasher()
        : context_(EVP_MD_CTX_new(), &EVP_MD_CTX_free) {
        if (!context_ || EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
            throw std::runtime_error("database cache key digest failed");
        }
    }
    void bytes(std::string_view value) {
        if (!value.empty() && EVP_DigestUpdate(context_.get(), value.data(), value.size()) != 1) {
            throw std::runtime_error("database cache key digest failed");
        }
    }
    void number(std::uint64_t value) {
        std::array<char, 8> encoded;
        for (unsigned index = 0; index < encoded.size(); ++index) {
            encoded[index] = static_cast<char>((value >> (index * 8)) & 255);
        }
        bytes({encoded.data(), encoded.size()});
    }
    void text(std::string_view value) {
        number(value.size());
        bytes(value);
    }
    void type(db_value_type value) {
        const auto byte = static_cast<char>(value);
        bytes({&byte, 1});
    }
    void append_result(std::pmr::string& output) {
        std::array<unsigned char, SHA256_DIGEST_LENGTH> hash{};
        unsigned length = 0;
        if (EVP_DigestFinal_ex(context_.get(), hash.data(), &length) != 1 || length != hash.size()) {
            throw std::runtime_error("database cache key digest failed");
        }
        http_append_lower_hex(output, hash);
    }

private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
};

}  // namespace

task<db_rows> db_cache_query::operator()(operation_options options) && {
    return query_db_backend(backend_, std::move(sql_), std::move(params_), options);
}

// Format RUVIAQC2: row count, column count, the column names once, then for
// every row a row marker followed, for every field, by a presence byte and its
// text when present. All rows of one result share a single schema, so the
// names are not repeated per row. The marker gives every row, including a
// zero-column one, a serialized cost, so a decoder bounds the row count by the
// entry size before it allocates.
constexpr char db_cache_row_marker = '\2';

std::pmr::string encode_db_cache_rows(const db_rows& rows, std::pmr::memory_resource* resource) {
    const auto names = rows.empty() ? std::span<const std::pmr::string>{}
                                    : db_result_access::column_names(rows.front());
    std::size_t total_bytes = 8U + 8U + 8U;
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
    out.append("RUVIAQC2");
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
    if (!bytes_value.starts_with("RUVIAQC2")) {
        invalid_cache();
    }
    reader reader_value{std::string_view(bytes_value).substr(8)};
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
std::pmr::string db_cache_scope(std::string_view name_space, std::string_view alias,
    const db_config_storage& config, std::pmr::memory_resource* resource) {
    const auto server_name = config.tls_.server_name_ ? std::string_view(*config.tls_.server_name_) : std::string_view{};
    std::pmr::string identity(resource);
    identity.reserve(8U * 8U + name_space.size() + alias.size() + config.host_.size() +
                     config.database_.size() + config.username_.size() + server_name.size());
    text(identity, name_space);
    text(identity, alias);
    number(identity, static_cast<std::uint64_t>(config.driver_));
    text(identity, config.host_);
    number(identity, config.port_);
    text(identity, config.database_);
    text(identity, config.username_);
    text(identity, server_name);
    std::pmr::string result(resource);
    result.reserve(digest_text_bytes);
    append_digest(result, identity);
    return result;
}
std::pmr::string db_cache_prefix(std::string_view name_space, std::pmr::memory_resource* resource) {
    std::pmr::string result_value(resource);
    result_value.reserve(full_prefix_bytes);
    result_value.append(cache_prefix);
    append_digest(result_value, name_space);
    result_value.push_back(':');
    return result_value;
}
void append_db_cache_id_keys(
    std::pmr::vector<std::pmr::string>& keys, std::string_view prefix, std::string_view id) {
    const auto hash = cache_digest(id);
    auto* const resource = keys.get_allocator().resource();
    for (const auto shape : db_cache_shapes) {
        keys.push_back(make_id_key(prefix, shape, hash, resource));
    }
}
std::pmr::string db_cache_key(std::string_view prefix, std::string_view id, db_cache_shape shape,
    std::string_view sql, std::span<const db_value> params, db_driver driver, std::pmr::memory_resource* resource) {
    if (!id.empty()) {
        return make_id_key(prefix, shape, cache_digest(id), resource);
    }
    std::pmr::string result_value(resource);
    result_value.reserve(prefix.size() + 4 + digest_text_bytes);
    result_value.append(prefix);
    cache_key_hasher input;
    input.number(static_cast<std::uint64_t>(driver));
    input.text(sql);
    input.number(params.size());
    for (const auto& value : params) {
        const auto type = db_value_access::type(value);
        input.type(type);
        switch (type) {
            case db_value_type::null:
                break;
            case db_value_type::string:
                input.text(db_value_access::text(value));
                break;
            case db_value_type::signed_value:
                input.number(static_cast<std::uint64_t>(db_value_access::signed_value(value)));
                break;
            case db_value_type::unsigned_value:
                input.number(db_value_access::unsigned_value(value));
                break;
            case db_value_type::double_value:
                input.number(std::bit_cast<std::uint64_t>(db_value_access::get_double_value(value)));
                break;
            case db_value_type::bool_value:
                input.number(db_value_access::get_bool_value(value));
                break;
        }
    }
    result_value.append("sql:");
    input.append_result(result_value);
    return result_value;
}
}  // namespace ruvia::detail
