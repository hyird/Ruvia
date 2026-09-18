#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
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
class DbEntityRowDecoder final {
    using Columns = typename E::Columns;
    static constexpr auto kColumnCount = std::tuple_size_v<Columns>;
    static constexpr auto kMissingColumn = (std::numeric_limits<std::size_t>::max)();

public:
    DbEntityRowDecoder() {
        selected_.fill(true);
    }
    explicit DbEntityRowDecoder(const std::array<bool, kColumnCount>& selected)
        : selected_(selected) {}

    E decode(const DbRow& row, std::pmr::memory_resource* resource) {
        const auto names = DbResultAccess::columnNames(row);
        if (!schema_ || schema_->data() != names.data() || schema_->size() != names.size()) {
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                (bindColumn<I>(names), ...);
            }(std::make_index_sequence<kColumnCount>{});
            schema_ = names;
        }
        E result(resource);
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (decodeColumn<I>(result, row, resource), ...);
        }(std::make_index_sequence<kColumnCount>{});
        return result;
    }

private:
    template <std::size_t I>
    void bindColumn(std::span<const std::pmr::string> names) noexcept {
        indices_[I] = kMissingColumn;
        if (!selected_[I]) {
            return;
        }
        using Column = std::tuple_element_t<I, Columns>;
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (names[index] == Column::name.view()) {
                indices_[I] = index;
                return;
            }
        }
    }
    template <std::size_t I>
    void decodeColumn(E& result, const DbRow& row, std::pmr::memory_resource* resource) const {
        if (!selected_[I]) {
            return;
        }
        if (indices_[I] == kMissingColumn) {
            throw std::out_of_range("database result has no such column");
        }
        decodeEntityField<E, std::tuple_element_t<I, Columns>>(result, row[indices_[I]], resource);
    }

    std::array<bool, kColumnCount> selected_{};
    std::array<std::size_t, kColumnCount> indices_{};
    // Used only while synchronously mapping a live, immutable DbRows result.
    std::optional<std::span<const std::pmr::string>> schema_{};
};

template <typename E>
DbEntityRows<E> mapDbEntityRows(DbRows&& rows, std::pmr::memory_resource* resource = nullptr) {
    auto* resolved = pmrResourceOrDefault(resource);
    auto result = DbResultAccess::makeEntityRows<E>(resolved, rows.size());
    DbEntityRowDecoder<E> decoder;
    for (const auto& row : rows) {
        result.push_back(decoder.decode(row, resolved));
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
        auto remaining = std::string_view(value);
        while (!remaining.empty()) {
            const auto next = std::ranges::find_if(remaining, [](char ch) noexcept {
                return ch == '"' || ch == '\\' || ch == '\0';
            });
            if (next == remaining.end()) {
                output.append(remaining);
                break;
            }
            const auto special = static_cast<std::size_t>(next - remaining.begin());
            if (special != 0) {
                output.append(remaining.data(), special);
            }
            auto escapedEnd = special;
            do {
                const char ch = remaining[escapedEnd];
                if (ch == '\0') {
                    throw std::invalid_argument("PostgreSQL array text cannot contain NUL");
                }
                output.push_back('\\');
                output.push_back(ch);
                ++escapedEnd;
            } while (escapedEnd < remaining.size() &&
                     (remaining[escapedEnd] == '"' || remaining[escapedEnd] == '\\' || remaining[escapedEnd] == '\0'));
            remaining.remove_prefix(escapedEnd);
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
