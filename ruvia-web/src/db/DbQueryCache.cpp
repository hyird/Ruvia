#include "ruvia/web/detail/db/DbQueryCache.h"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>

#include <openssl/sha.h>

#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#include "ruvia/web/detail/db/DbUtils.h"
#include "ruvia/web/detail/db/DbValueAccess.h"

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
[[noreturn]] void invalidCache() {
    throw DbConversionError(DbConversionError::Code::kInvalidFormat, "invalid database query cache entry");
}
struct Reader {
    std::string_view bytes;
    std::uint64_t number() {
        if (bytes.size() < 8) {
            invalidCache();
        }
        std::uint64_t value = 0;
        for (unsigned index = 0; index < 8; ++index) {
            value |= std::uint64_t(static_cast<unsigned char>(bytes[index])) << (index * 8);
        }
        bytes.remove_prefix(8);
        return value;
    }
    std::string_view text() {
        const auto size = number();
        if (size > bytes.size()) {
            invalidCache();
        }
        auto result = bytes.substr(0, static_cast<std::size_t>(size));
        bytes.remove_prefix(static_cast<std::size_t>(size));
        return result;
    }
};
std::pmr::string digest(std::string_view bytes, std::pmr::memory_resource* resource) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> hash{};
    if (!SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), hash.data())) {
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

Task<DbRows> DbCacheQuery::operator()(OperationOptions options) && {
    return executeOwned(std::move(pool_), std::move(slot_), std::move(sql_), std::move(params_),
        resource_, backendFailed_, std::move(options));
}

Task<DbRows> DbCacheQuery::executeOwned(DbPoolRef pool, std::optional<std::size_t> slot,
    std::pmr::string sql, std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource, bool* backendFailed,
    OperationOptions options) {
    if (slot.has_value()) {
        auto backend = visitDbPool(pool, [&](auto& client) {
            return queryOnDbTransactionSlot(client, *slot, std::move(sql), std::move(params),
                resource, options);
        });
        try {
            co_return co_await std::move(backend);
        } catch (...) {
            if (backendFailed != nullptr) {
                *backendFailed = true;
            }
            throw;
        }
    }

    auto backend = visitDbPool(pool, [&](auto& client) {
        return executeDbQuery(client, std::move(sql), std::move(params), resource,
            std::move(options));
    });
    try {
        co_return co_await std::move(backend);
    } catch (...) {
        if (backendFailed != nullptr) {
            *backendFailed = true;
        }
        throw;
    }
}

std::pmr::string encodeDbCacheRows(const DbRows& rows, std::pmr::memory_resource* resource) {
    std::pmr::string out("RUVIAQC1", resource);
    number(out, rows.size());
    for (const auto& row : rows) {
        number(out, row.size());
        const auto names = DbResultAccess::columnNames(row);
        if (names.size() != row.size()) {
            invalidCache();
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
DbRows decodeDbCacheRows(std::string_view bytes, std::pmr::memory_resource* resource) {
    if (!bytes.starts_with("RUVIAQC1")) {
        invalidCache();
    }
    Reader reader{bytes.substr(8)};
    const auto count = reader.number();
    if (count > reader.bytes.size() / 8) {
        invalidCache();
    }
    auto result = DbResultAccess::makeResult(resource);
    auto& rows = DbResultAccess::rows(result);
    rows.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t rowIndex = 0; rowIndex < count; ++rowIndex) {
        const auto columns = reader.number();
        if (columns > reader.bytes.size() / 9) {
            invalidCache();
        }
        auto row = DbResultAccess::ownedRow(resource);
        auto& fields = DbResultAccess::ownedFields(row);
        auto& names = DbResultAccess::ownedColumnNames(row);
        fields.reserve(static_cast<std::size_t>(columns));
        names.reserve(static_cast<std::size_t>(columns));
        for (std::uint64_t index = 0; index < columns; ++index) {
            names.emplace_back(reader.text());
            if (reader.bytes.empty()) {
                invalidCache();
            }
            const char present = reader.bytes.front();
            reader.bytes.remove_prefix(1);
            if (present == '\0') {
                fields.push_back(DbResultAccess::nullField(resource));
            } else if (present == '\1') {
                fields.push_back(DbResultAccess::ownedField(reader.text(), resource));
            } else {
                invalidCache();
            }
        }
        rows.push_back(std::move(row));
    }
    if (!reader.bytes.empty()) {
        invalidCache();
    }
    return result;
}
std::pmr::string dbCachePrefix(std::string_view nameSpace, std::pmr::memory_resource* resource) {
    std::pmr::string result("ruvia:qc:v1:", resource);
    result.append(digest(nameSpace, resource)).push_back(':');
    return result;
}
std::pmr::string dbCacheKey(std::string_view nameSpace, std::string_view id, std::string_view sql,
    std::span<const DbValue> params, DbDriver driver, std::pmr::memory_resource* resource) {
    auto result = dbCachePrefix(nameSpace, resource);
    if (!id.empty()) {
        result.append("id:").append(digest(id, resource));
        return result;
    }
    std::pmr::string input(resource);
    number(input, static_cast<std::uint64_t>(driver));
    text(input, sql);
    number(input, params.size());
    for (const auto& value : params) {
        const auto type = DbValueAccess::type(value);
        input.push_back(static_cast<char>(type));
        switch (type) {
            case DbValueType::kNull:
                break;
            case DbValueType::kString:
                text(input, DbValueAccess::text(value));
                break;
            case DbValueType::kSigned:
                number(input, static_cast<std::uint64_t>(DbValueAccess::signedValue(value)));
                break;
            case DbValueType::kUnsigned:
                number(input, DbValueAccess::unsignedValue(value));
                break;
            case DbValueType::kDouble:
                number(input, std::bit_cast<std::uint64_t>(DbValueAccess::doubleValue(value)));
                break;
            case DbValueType::kBool:
                number(input, DbValueAccess::boolValue(value));
                break;
        }
    }
    result.append("sql:").append(digest(input, resource));
    return result;
}
}  // namespace ruvia::detail
