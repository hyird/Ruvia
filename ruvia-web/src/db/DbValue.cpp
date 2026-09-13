#include <utility>

#include "ruvia/web/db/DbTypes.h"

namespace ruvia {

DbValue::DbValue(std::nullptr_t)
    : storage_(std::monostate{}) {}

DbValue::DbValue(const char* value)
    : storage_(value == nullptr ? Storage(std::monostate{})
                                : Storage(std::in_place_type<BorrowedText>, value)) {}

DbValue::DbValue(std::string_view value)
    : storage_(std::in_place_type<BorrowedText>, value) {}

DbValue::DbValue(std::pmr::string value)
    : storage_(std::in_place_type<std::pmr::string>, std::move(value)) {}

DbValue::DbValue(bool value)
    : storage_(std::in_place_type<bool>, value) {}

detail::DbValueType DbValue::type() const noexcept {
    return std::visit(
        [](const auto& value) noexcept {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, std::monostate>) {
                return detail::DbValueType::kNull;
            } else if constexpr (std::is_same_v<Value, BorrowedText> ||
                                 std::is_same_v<Value, std::pmr::string>) {
                return detail::DbValueType::kString;
            } else if constexpr (std::is_same_v<Value, std::int64_t>) {
                return detail::DbValueType::kSigned;
            } else if constexpr (std::is_same_v<Value, std::uint64_t>) {
                return detail::DbValueType::kUnsigned;
            } else if constexpr (std::is_same_v<Value, double>) {
                return detail::DbValueType::kDouble;
            } else {
                return detail::DbValueType::kBool;
            }
        },
        storage_);
}

std::string_view DbValue::text() const& noexcept {
    if (const auto* borrowed = std::get_if<BorrowedText>(&storage_)) {
        return borrowed->view();
    }
    if (const auto* owned = std::get_if<std::pmr::string>(&storage_)) {
        return *owned;
    }
    return {};
}

std::int64_t DbValue::signedValue() const noexcept {
    const auto* value = std::get_if<std::int64_t>(&storage_);
    return value == nullptr ? 0 : *value;
}

std::uint64_t DbValue::unsignedValue() const noexcept {
    const auto* value = std::get_if<std::uint64_t>(&storage_);
    return value == nullptr ? 0 : *value;
}

double DbValue::doubleValue() const noexcept {
    const auto* value = std::get_if<double>(&storage_);
    return value == nullptr ? 0.0 : *value;
}

bool DbValue::boolValue() const noexcept {
    const auto* value = std::get_if<bool>(&storage_);
    return value != nullptr && *value;
}

}  // namespace ruvia
