#pragma once

#include <charconv>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/db/DbEntity.h"
#include "ruvia/web/db/DbRows.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#include "ruvia/web/detail/db/DbUtils.h"
#include "ruvia/web/detail/db/DbValueAccess.h"

namespace ruvia::detail {

template <typename T>
void decodeArrayElement(std::string_view text, T& value, std::pmr::memory_resource* resource) {
    if constexpr (is_optional<T>::value) {
        using V = typename is_optional<T>::value_type;
        auto inner = DbEntitySlot<V>::makeValue(resource);
        decodeArrayElement(text, inner, resource);
        value = std::move(inner);
    } else if constexpr (std::is_same_v<T, std::pmr::string>) {
        value.assign(text);
    } else if constexpr (std::is_same_v<T, String>) {
        value.assignOwned(text);
    } else if constexpr (std::is_same_v<T, bool>) {
        if (text == "t" || text == "true" || text == "1") {
            value = true;
        } else if (text == "f" || text == "false" || text == "0") {
            value = false;
        } else {
            throw DbConversionError(DbConversionError::Code::kInvalidFormat, "invalid boolean array element");
        }
    } else {
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || end != text.data() + text.size()) {
            throw DbConversionError(DbConversionError::Code::kInvalidFormat, "invalid array element");
        }
    }
    (void)resource;
}

template <typename T>
void decodeDbField(const DbField& field, T& out, std::pmr::memory_resource* resource) {
    using U = std::remove_cv_t<T>;
    if constexpr (is_optional<U>::value) {
        using V = typename is_optional<U>::value_type;
        if (!field.value()) {
            out.reset();
            return;
        }
        auto value = DbEntitySlot<V>::makeValue(resource);
        decodeDbField(field, value, resource);
        out = std::move(value);
    } else if constexpr (IsPmrVector<U>::value) {
        using V = typename IsPmrVector<U>::value_type;
        const auto source = field.value();
        if (!source || source->size() < 2 || source->front() != '{' || source->back() != '}') {
            throw DbConversionError(DbConversionError::Code::kInvalidFormat, "invalid PostgreSQL array");
        }
        out.clear();
        std::pmr::string token(resource);
        bool quoted = false;
        bool quotedToken = false;
        bool quoteClosed = false;
        bool escaped = false;
        const auto invalidArray = [] { throw DbConversionError(DbConversionError::Code::kInvalidFormat, "malformed one-dimensional PostgreSQL array"); };
        auto append = [&] {
            if (quoted || escaped || (token.empty() && !quotedToken)) {
                invalidArray();
            }
            if (token == "NULL" && !quotedToken) {
                if constexpr (is_optional<V>::value) {
                    out.emplace_back(std::nullopt);
                } else {
                    throw DbConversionError(DbConversionError::Code::kInvalidFormat, "NULL array element");
                }
            } else {
                auto value = DbEntitySlot<V>::makeValue(resource);
                decodeArrayElement(token, value, resource);
                out.push_back(std::move(value));
            }
            token.clear();
            quoted = false;
            quotedToken = false;
            quoteClosed = false;
        };
        for (std::size_t i = 1; i + 1 < source->size(); ++i) {
            const char ch = (*source)[i];
            if (escaped) {
                token.push_back(ch);
                escaped = false;
            } else if (ch == '\\') {
                if (quoteClosed) {
                    invalidArray();
                }
                escaped = true;
                quotedToken = true;
            } else if (ch == '"') {
                if (quoted) {
                    quoted = false;
                    quoteClosed = true;
                } else {
                    if (!token.empty() || quoteClosed) {
                        invalidArray();
                    }
                    quoted = true;
                    quotedToken = true;
                }
            } else if (ch == ',' && !quoted) {
                append();
            } else {
                if (quoteClosed || (!quoted && (ch == '{' || ch == '}'))) {
                    invalidArray();
                }
                token.push_back(ch);
            }
        }
        if (source->size() > 2) {
            append();
        }
    } else if constexpr (std::is_same_v<U, std::pmr::string>) {
        if (auto value = field.as<std::string_view>()) {
            out.assign(*value);
        } else {
            throw std::invalid_argument("NULL in non-nullable entity column");
        }
    } else if constexpr (std::is_same_v<U, String>) {
        if (auto value = field.as<std::string_view>()) {
            out.assignOwned(*value);
        } else {
            throw std::invalid_argument("NULL in non-nullable entity column");
        }
    } else {
        if (auto value = field.as<U>()) {
            out = *value;
        } else {
            throw std::invalid_argument("NULL in non-nullable entity column");
        }
    }
    (void)resource;
}

template <typename E, typename C>
void decodeEntityField(E& entity, const DbField& field, std::pmr::memory_resource* resource) {
    if (!field.value()) {
        if constexpr (C::options.nullable) {
            entity.template setNull<C::name>();
        } else {
            throw std::invalid_argument("NULL in non-nullable database entity column");
        }
        return;
    }
    using T = typename C::value_type;
    auto value = DbEntitySlot<T>::makeValue(resource);
    decodeDbField(field, value, resource);
    entity.template set<C::name>(std::move(value));
}

template <typename E, typename C>
void decodeEntityColumn(E& entity, const DbRow& row, std::pmr::memory_resource* resource) {
    decodeEntityField<E, C>(entity, row[C::name.view()], resource);
}

template <typename E, typename... C, std::size_t... I>
void decodeEntity(E& entity, const DbRow& row, std::pmr::memory_resource* resource,
    std::tuple<C...>*, std::index_sequence<I...>) {
    (decodeEntityColumn<E, std::tuple_element_t<I, std::tuple<C...>>>(entity, row, resource), ...);
}

template <typename E>
DbEntityRows<E> mapDbEntityRows(DbRows&& rows, std::pmr::memory_resource* resource = nullptr) {
    auto* resolved = pmrResourceOrDefault(resource);
    DbEntityRows<E> result(resolved);
    using Columns = typename E::Columns;
    for (const auto& row : rows) {
        E entity(resolved);
        decodeEntity(entity, row, resolved, static_cast<Columns*>(nullptr),
            std::make_index_sequence<std::tuple_size_v<Columns>>{});
        result.push_back(std::move(entity));
    }
    return result;
}

template <typename T>
void appendEntityArrayValue(std::pmr::string& output, const T& value) {
    if constexpr (is_optional<T>::value) {
        if (value) {
            appendEntityArrayValue(output, *value);
        } else {
            output += "NULL";
        }
    } else if constexpr (std::is_same_v<T, std::pmr::string> || std::is_same_v<T, String>) {
        output.push_back('"');
        for (const char ch : std::string_view(value)) {
            if (ch == '\0') {
                throw std::invalid_argument("PostgreSQL array text cannot contain NUL");
            }
            if (ch == '\\' || ch == '"') {
                output.push_back('\\');
            }
            output.push_back(ch);
        }
        output.push_back('"');
    } else if constexpr (std::is_same_v<T, bool>) {
        output += value ? "t" : "f";
    } else if constexpr (std::is_floating_point_v<T>) {
        appendDbNumber(output, static_cast<double>(value));
    } else if constexpr (std::is_signed_v<T>) {
        appendDbNumber(output, static_cast<std::int64_t>(value));
    } else {
        appendDbNumber(output, static_cast<std::uint64_t>(value));
    }
}

template <typename T>
DbValue entityDbValue(const T& value, std::pmr::memory_resource* resource) {
    if constexpr (is_optional<T>::value) {
        if (!value) {
            return DbValue(nullptr);
        }
        return entityDbValue(*value, resource);
    } else if constexpr (std::is_same_v<T, std::pmr::string>) {
        return DbValueAccess::ownedString(std::pmr::string(value, resource));
    } else if constexpr (std::is_same_v<T, String>) {
        return DbValueAccess::ownedString(std::pmr::string(std::string_view(value), resource));
    } else if constexpr (std::is_same_v<T, bool> || std::is_arithmetic_v<T>) {
        return DbValue(value);
    } else if constexpr (IsPmrVector<T>::value) {
        std::pmr::string encoded(resource);
        encoded.push_back('{');
        bool first = true;
        for (const auto& item : value) {
            if (!first) {
                encoded.push_back(',');
            }
            first = false;
            if constexpr (std::is_same_v<typename IsPmrVector<T>::value_type, bool>) {
                appendEntityArrayValue(encoded, static_cast<bool>(item));
            } else {
                appendEntityArrayValue(encoded, item);
            }
        }
        encoded.push_back('}');
        return DbValueAccess::ownedString(std::move(encoded));
    } else {
        static_assert(std::is_same_v<T, void>, "unsupported entity value type");
    }
}

}  // namespace ruvia::detail
