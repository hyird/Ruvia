#include <utility>

#include "ruvia/web/db/db_types.h"

namespace ruvia {

db_value::db_value(std::nullptr_t)
    : storage_(std::monostate{}) {}

db_value::db_value(const char* value)
    : storage_(value == nullptr ? storage_type(std::monostate{})
                                : storage_type(std::in_place_type<borrowed_text>, value)) {}

db_value::db_value(std::string_view value)
    : storage_(std::in_place_type<borrowed_text>, value) {}

db_value::db_value(std::pmr::string value)
    : storage_(std::in_place_type<std::pmr::string>, std::move(value)) {}

db_value::db_value(bool value)
    : storage_(std::in_place_type<bool>, value) {}

detail::db_value_type db_value::type() const noexcept {
    return std::visit(
        [](const auto& value) noexcept {
            using value_type = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<value_type, std::monostate>) {
                return detail::db_value_type::null;
            } else if constexpr (std::is_same_v<value_type, borrowed_text> ||
                                 std::is_same_v<value_type, std::pmr::string>) {
                return detail::db_value_type::string;
            } else if constexpr (std::is_same_v<value_type, std::int64_t>) {
                return detail::db_value_type::signed_value;
            } else if constexpr (std::is_same_v<value_type, std::uint64_t>) {
                return detail::db_value_type::unsigned_value;
            } else if constexpr (std::is_same_v<value_type, double>) {
                return detail::db_value_type::double_value;
            } else {
                return detail::db_value_type::bool_value;
            }
        },
        storage_);
}

std::string_view db_value::text() const& noexcept {
    if (const auto* borrowed = std::get_if<borrowed_text>(&storage_)) {
        return borrowed->view();
    }
    if (const auto* owned = std::get_if<std::pmr::string>(&storage_)) {
        return *owned;
    }
    return {};
}

std::int64_t db_value::signed_value() const noexcept {
    const auto* value = std::get_if<std::int64_t>(&storage_);
    return value == nullptr ? 0 : *value;
}

std::uint64_t db_value::unsigned_value() const noexcept {
    const auto* value = std::get_if<std::uint64_t>(&storage_);
    return value == nullptr ? 0 : *value;
}

double db_value::get_double_value() const noexcept {
    const auto* value = std::get_if<double>(&storage_);
    return value == nullptr ? 0.0 : *value;
}

bool db_value::get_bool_value() const noexcept {
    const auto* value = std::get_if<bool>(&storage_);
    return value != nullptr && *value;
}

}  // namespace ruvia
