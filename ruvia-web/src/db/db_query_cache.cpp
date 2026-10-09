#include "db/db_query_cache.h"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include <openssl/sha.h>

#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_utils.h"
#include "ruvia/web/detail/db/db_value_access.h"

#include "db/db_config_storage.h"
#include "db/db_query_cache_state.h"
#include "db/db_registry.h"

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
    constexpr char hex[] = "0123456789abcdef";
    std::pmr::string result(resource);
    result.reserve(hash.size() * 2);
    for (auto byte : hash) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}
}  // namespace

db_query_plan db_query_plan::prepare(const db_query& query, const db_query* count,
    db_driver driver, std::pmr::memory_resource* resource, db_query_cache_state* cache) {
    const auto prepare = [&](const db_query& input) {
        auto statement = input.compile(driver, resource);
        if (!statement.returns_rows()) {
            throw std::invalid_argument(count
                                            ? "query and count require statements that return rows"
                                            : "query requires a statement that returns rows");
        }
        auto key = cache ? cache->key(input, statement, driver) : std::nullopt;
        return db_query_step{std::move(statement.sql_), std::move(statement.params_),
            std::move(key), input.cache_duration()};
    };
    db_query_plan plan{prepare(query), std::nullopt};
    if (count != nullptr) {
        plan.second_.emplace(prepare(*count));
    }
    return plan;
}

task<db_rows> db_query_backend::operator()(db_query_step step,
    operation_options options, const operation_timeout& deadline_value) const {
    db_cache_query database_value(pool_, slot_, std::move(step.sql_), std::move(step.params_),
        resource_, backend_failed_);
    if (cache_ != nullptr && step.cache_key_.has_value()) {
        return cache_->wrap(step.cache_duration_, std::move(step.cache_key_),
            std::move(database_value), std::move(options), deadline_value);
    }
    return std::move(database_value)(std::move(options));
}

task<db_rows> db_cache_query::operator()(operation_options options) && {
    return execute_owned(std::move(pool_), std::move(slot_), std::move(sql_), std::move(params_),
        resource_, backend_failed_, std::move(options));
}

task<db_rows> db_cache_query::execute_owned(db_pool_ref_type pool, std::optional<std::size_t> slot,
    std::pmr::string sql, std::pmr::vector<db_value> params,
    std::pmr::memory_resource* resource, bool* backend_failed,
    operation_options options) {
    if (slot.has_value()) {
        auto backend = visit_db_pool(pool, [&](auto& client) {
            return query_on_db_transaction_slot(client, *slot, std::move(sql), std::move(params),
                resource, options);
        });
        try {
            co_return co_await std::move(backend);
        } catch (...) {
            if (backend_failed != nullptr) {
                *backend_failed = true;
            }
            throw;
        }
    }

    auto backend = visit_db_pool(pool, [&](auto& client) {
        return execute_db_query(client, std::move(sql), std::move(params), resource,
            std::move(options));
    });
    try {
        co_return co_await std::move(backend);
    } catch (...) {
        if (backend_failed != nullptr) {
            *backend_failed = true;
        }
        throw;
    }
}

std::pmr::string encode_db_cache_rows(const db_rows& rows, std::pmr::memory_resource* resource) {
    std::pmr::string out("RUVIAQC1", resource);
    number(out, rows.size());
    for (const auto& row : rows) {
        number(out, row.size());
        const auto names = db_result_access::column_names(row);
        if (names.size() != row.size()) {
            invalid_cache();
        }
        for (std::size_t index = 0; index < row.size(); ++index) {
            text(out, names[index]);
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
    if (!bytes_value.starts_with("RUVIAQC1")) {
        invalid_cache();
    }
    reader reader_value{bytes_value.substr(8)};
    const auto count = reader_value.number();
    if (count > reader_value.bytes_.size() / 8) {
        invalid_cache();
    }
    auto result_value = db_result_access::make_result(resource);
    auto& rows = db_result_access::rows(result_value);
    rows.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t row_index = 0; row_index < count; ++row_index) {
        const auto columns = reader_value.number();
        if (columns > reader_value.bytes_.size() / 9) {
            invalid_cache();
        }
        auto row = db_result_access::owned_row(resource);
        auto& fields_value = db_result_access::owned_fields(row);
        auto& names = db_result_access::owned_column_names(row);
        fields_value.reserve(static_cast<std::size_t>(columns));
        names.reserve(static_cast<std::size_t>(columns));
        for (std::uint64_t index = 0; index < columns; ++index) {
            names.emplace_back(reader_value.text());
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
        rows.push_back(std::move(row));
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
    text(identity, config.tls_.server_name_);
    return digest(identity, resource);
}
std::pmr::string db_cache_prefix(std::string_view name_space, std::pmr::memory_resource* resource) {
    std::pmr::string result_value("ruvia:qc:v2:", resource);
    result_value.append(digest(name_space, resource)).push_back(':');
    return result_value;
}
std::pmr::string db_cache_key(std::string_view name_space, std::string_view id, std::string_view sql,
    std::span<const db_value> params, db_driver driver, std::pmr::memory_resource* resource) {
    auto result_value = db_cache_prefix(name_space, resource);
    if (!id.empty()) {
        result_value.append("id:").append(digest(id, resource));
        return result_value;
    }
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
