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

#include "ruvia/web/db/db_entity.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_utils.h"
#include "ruvia/web/detail/db/db_value_access.h"

namespace ruvia::detail {

template <typename t_type>
void decode_array_element(std::string_view text, t_type& value, std::pmr::memory_resource* resource) {
    if constexpr (is_optional<t_type>::value) {
        using v_type = typename is_optional<t_type>::value_type;
        auto inner = entity_value_slot<v_type>::make_value(resource);
        decode_array_element(text, inner, resource);
        value = std::move(inner);
    } else if constexpr (std::is_same_v<t_type, std::pmr::string>) {
        value.assign(text);
    } else if constexpr (std::is_same_v<t_type, string>) {
        value.assign_owned(text);
    } else if constexpr (std::is_same_v<t_type, bool>) {
        if (text == "t" || text == "true" || text == "1") {
            value = true;
        } else if (text == "f" || text == "false" || text == "0") {
            value = false;
        } else {
            throw db_conversion_error(db_conversion_error::code_type::invalid_format, "invalid boolean array element");
        }
    } else {
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || end != text.data() + text.size()) {
            throw db_conversion_error(db_conversion_error::code_type::invalid_format, "invalid array element");
        }
    }
    (void)resource;
}

template <typename t_type>
void decode_db_field(const db_field& field, t_type& out, std::pmr::memory_resource* resource) {
    using u_type = std::remove_cv_t<t_type>;
    if constexpr (is_optional<u_type>::value) {
        using v_type = typename is_optional<u_type>::value_type;
        if (!field.value()) {
            out.reset();
            return;
        }
        auto value = entity_value_slot<v_type>::make_value(resource);
        decode_db_field(field, value, resource);
        out = std::move(value);
    } else if constexpr (is_pmr_vector<u_type>::value) {
        using v_type = typename is_pmr_vector<u_type>::value_type;
        const auto source_value = field.value();
        if (!source_value || source_value->size() < 2 || source_value->front() != '{' || source_value->back() != '}') {
            throw db_conversion_error(db_conversion_error::code_type::invalid_format, "invalid PostgreSQL array");
        }
        out.clear();
        std::pmr::string token(resource);
        bool quoted = false;
        bool quoted_token = false;
        bool quote_closed = false;
        bool escaped = false;
        const auto invalid_array = [] { throw db_conversion_error(db_conversion_error::code_type::invalid_format, "malformed one-dimensional PostgreSQL array"); };
        auto append = [&] {
            if (quoted || escaped || (token.empty() && !quoted_token)) {
                invalid_array();
            }
            if (token == "NULL" && !quoted_token) {
                if constexpr (is_optional<v_type>::value) {
                    out.emplace_back(std::nullopt);
                } else {
                    throw db_conversion_error(db_conversion_error::code_type::invalid_format, "NULL array element");
                }
            } else {
                auto value = entity_value_slot<v_type>::make_value(resource);
                decode_array_element(token, value, resource);
                out.push_back(std::move(value));
            }
            token.clear();
            quoted = false;
            quoted_token = false;
            quote_closed = false;
        };
        for (std::size_t i = 1; i + 1 < source_value->size(); ++i) {
            const char ch = (*source_value)[i];
            if (escaped) {
                token.push_back(ch);
                escaped = false;
            } else if (ch == '\\') {
                if (quote_closed) {
                    invalid_array();
                }
                escaped = true;
                quoted_token = true;
            } else if (ch == '"') {
                if (quoted) {
                    quoted = false;
                    quote_closed = true;
                } else {
                    if (!token.empty() || quote_closed) {
                        invalid_array();
                    }
                    quoted = true;
                    quoted_token = true;
                }
            } else if (ch == ',' && !quoted) {
                append();
            } else {
                if (quote_closed || (!quoted && (ch == '{' || ch == '}'))) {
                    invalid_array();
                }
                token.push_back(ch);
            }
        }
        if (source_value->size() > 2) {
            append();
        }
    } else if constexpr (std::is_same_v<u_type, std::pmr::string>) {
        if (auto value = field.as<std::string_view>()) {
            out.assign(*value);
        } else {
            throw std::invalid_argument("NULL in non-nullable entity column");
        }
    } else if constexpr (std::is_same_v<u_type, string>) {
        if (auto value = field.as<std::string_view>()) {
            out.assign_owned(*value);
        } else {
            throw std::invalid_argument("NULL in non-nullable entity column");
        }
    } else {
        if (auto value = field.as<u_type>()) {
            out = *value;
        } else {
            throw std::invalid_argument("NULL in non-nullable entity column");
        }
    }
    (void)resource;
}

template <typename e_type, typename c_type>
void decode_entity_field(e_type& entity, const db_field& field, std::pmr::memory_resource* resource) {
    if (!field.value()) {
        if constexpr (c_type::options.nullable_) {
            entity.template set_null<c_type::name>();
        } else {
            throw std::invalid_argument("NULL in non-nullable database entity column");
        }
        return;
    }
    using t_type = typename c_type::value_type;
    auto value = entity_value_slot<t_type>::make_value(resource);
    decode_db_field(field, value, resource);
    entity.template set<c_type::name>(std::move(value));
}

template <typename e_type, typename c_type>
void decode_entity_column(e_type& entity, const db_row& row, std::pmr::memory_resource* resource) {
    decode_entity_field<e_type, c_type>(entity, row[c_type::name.view()], resource);
}

template <typename e_type, typename... c_type, std::size_t... i>
void decode_entity(e_type& entity, const db_row& row, std::pmr::memory_resource* resource,
    std::tuple<c_type...>*, std::index_sequence<i...>) {
    (decode_entity_column<e_type, std::tuple_element_t<i, std::tuple<c_type...>>>(entity, row, resource), ...);
}

template <typename e_type>
class db_entity_row_decoder final {
    using columns_type = typename e_type::columns_type;
    static constexpr auto column_count = std::tuple_size_v<columns_type>;
    static constexpr auto missing_column = (std::numeric_limits<std::size_t>::max)();

public:
    db_entity_row_decoder() {
        selected_.fill(true);
    }
    explicit db_entity_row_decoder(const std::array<bool, column_count>& selected)
        : selected_(selected) {}

    e_type decode(const db_row& row, std::pmr::memory_resource* resource) {
        const auto names = db_result_access::column_names(row);
        if (!schema_ || schema_->data() != names.data() || schema_->size() != names.size()) {
            [&]<std::size_t... i>(std::index_sequence<i...>) {
                (bind_column<i>(names), ...);
            }(std::make_index_sequence<column_count>{});
            schema_ = names;
        }
        e_type result(resource);
        [&]<std::size_t... i>(std::index_sequence<i...>) {
            (decode_column<i>(result, row, resource), ...);
        }(std::make_index_sequence<column_count>{});
        return result;
    }

private:
    template <std::size_t i>
    void bind_column(std::span<const std::pmr::string> names) noexcept {
        indices_[i] = missing_column;
        if (!selected_[i]) {
            return;
        }
        using column_type = std::tuple_element_t<i, columns_type>;
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (names[index] == column_type::name.view()) {
                indices_[i] = index;
                return;
            }
        }
    }
    template <std::size_t i>
    void decode_column(e_type& result_value, const db_row& row, std::pmr::memory_resource* resource) const {
        if (!selected_[i]) {
            return;
        }
        if (indices_[i] == missing_column) {
            throw std::out_of_range("database result has no such column");
        }
        decode_entity_field<e_type, std::tuple_element_t<i, columns_type>>(result_value, row[indices_[i]], resource);
    }

    std::array<bool, column_count> selected_{};
    std::array<std::size_t, column_count> indices_{};
    // Used only while synchronously mapping a live, immutable db_rows result.
    std::optional<std::span<const std::pmr::string>> schema_{};
};

template <typename e_type>
entity_rows<e_type> map_entity_rows(db_rows&& rows, std::pmr::memory_resource* resource = nullptr) {
    auto* resolved = pmr_resource_or_default(resource);
    auto result_value = db_result_access::make_entity_rows<e_type>(resolved, rows.size());
    db_entity_row_decoder<e_type> decoder;
    for (const auto& row : rows) {
        result_value.push_back(decoder.decode(row, resolved));
    }
    return result_value;
}

template <typename t_type>
void append_entity_array_value(std::pmr::string& output, const t_type& value) {
    if constexpr (is_optional<t_type>::value) {
        if (value) {
            append_entity_array_value(output, *value);
        } else {
            output += "NULL";
        }
    } else if constexpr (std::is_same_v<t_type, std::pmr::string> || std::is_same_v<t_type, string>) {
        output.push_back('"');
        auto remaining = std::string_view(value);
        while (!remaining.empty()) {
            const auto next_value = std::ranges::find_if(remaining, [](char ch) noexcept {
                return ch == '"' || ch == '\\' || ch == '\0';
            });
            if (next_value == remaining.end()) {
                output.append(remaining);
                break;
            }
            const auto special = static_cast<std::size_t>(next_value - remaining.begin());
            if (special != 0) {
                output.append(remaining.data(), special);
            }
            auto escaped_end = special;
            do {
                const char ch = remaining[escaped_end];
                if (ch == '\0') {
                    throw std::invalid_argument("PostgreSQL array text cannot contain NUL");
                }
                output.push_back('\\');
                output.push_back(ch);
                ++escaped_end;
            } while (escaped_end < remaining.size() &&
                     (remaining[escaped_end] == '"' || remaining[escaped_end] == '\\' || remaining[escaped_end] == '\0'));
            remaining.remove_prefix(escaped_end);
        }
        output.push_back('"');
    } else if constexpr (std::is_same_v<t_type, bool>) {
        output += value ? "t" : "f";
    } else if constexpr (std::is_floating_point_v<t_type>) {
        append_db_number(output, static_cast<double>(value));
    } else if constexpr (std::is_signed_v<t_type>) {
        append_db_number(output, static_cast<std::int64_t>(value));
    } else {
        append_db_number(output, static_cast<std::uint64_t>(value));
    }
}

template <typename t_type>
db_value entity_db_value(const t_type& value, std::pmr::memory_resource* resource) {
    if constexpr (is_optional<t_type>::value) {
        if (!value) {
            return db_value(nullptr);
        }
        return entity_db_value(*value, resource);
    } else if constexpr (std::is_same_v<t_type, std::pmr::string>) {
        return db_value_access::owned_string(std::pmr::string(value, resource));
    } else if constexpr (std::is_same_v<t_type, string>) {
        return db_value_access::owned_string(std::pmr::string(std::string_view(value), resource));
    } else if constexpr (std::is_same_v<t_type, bool> || std::is_arithmetic_v<t_type>) {
        return db_value(value);
    } else if constexpr (is_pmr_vector<t_type>::value) {
        std::pmr::string encoded(resource);
        encoded.push_back('{');
        bool first = true;
        for (const auto& item : value) {
            if (!first) {
                encoded.push_back(',');
            }
            first = false;
            if constexpr (std::is_same_v<typename is_pmr_vector<t_type>::value_type, bool>) {
                append_entity_array_value(encoded, static_cast<bool>(item));
            } else {
                append_entity_array_value(encoded, item);
            }
        }
        encoded.push_back('}');
        return db_value_access::owned_string(std::move(encoded));
    } else {
        static_assert(std::is_same_v<t_type, void>, "unsupported entity value type");
    }
}

}  // namespace ruvia::detail
