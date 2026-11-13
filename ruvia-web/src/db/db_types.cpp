#include "ruvia/web/db/db_types.h"

#include <utility>
#include <variant>

#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_schema.h"

namespace ruvia {
db_value::db_value(std::nullptr_t)
    : storage_(std::monostate{}) {}

db_value::db_value(const char* value)
    : storage_(value == nullptr ? storage_type(std::monostate{})
                                : storage_type(std::in_place_type<borrowed_text>, value)) {}

db_value::db_value(std::string_view value)
    : storage_(std::in_place_type<borrowed_text>, value) {}

db_value::db_value(std::in_place_type_t<std::string>, std::string value)
    : storage_(std::in_place_type<std::string>, std::move(value)) {}

db_value::db_value(const db_value& other)
    : storage_(std::visit(
          [](const auto& value) -> storage_type {
              using value_type = std::remove_cvref_t<decltype(value)>;
              if constexpr (std::is_same_v<value_type, std::string>) {
                  return storage_type(std::in_place_type<value_type>, value, value.get_allocator());
              } else {
                  return storage_type(std::in_place_type<value_type>, value);
              }
          },
          other.storage_)) {}

db_value::db_value(bool value)
    : storage_(std::in_place_type<bool>, value) {}

db_value_type db_value::type() const noexcept {
    return std::visit(
        [](const auto& value) noexcept {
            using value_type = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<value_type, std::monostate>) {
                return db_value_type::null;
            } else if constexpr (std::is_same_v<value_type, borrowed_text> ||
                                 std::is_same_v<value_type, std::string>) {
                return db_value_type::string;
            } else if constexpr (std::is_same_v<value_type, std::int64_t>) {
                return db_value_type::signed_value;
            } else if constexpr (std::is_same_v<value_type, std::uint64_t>) {
                return db_value_type::unsigned_value;
            } else if constexpr (std::is_same_v<value_type, double>) {
                return db_value_type::double_value;
            } else {
                return db_value_type::bool_value;
            }
        },
        storage_);
}

std::string_view db_value::text() const& {
    if (const auto* borrowed = std::get_if<borrowed_text>(&storage_)) {
        return borrowed->view();
    }
    return std::get<std::string>(storage_);
}

std::int64_t db_value::signed_value() const {
    return std::get<std::int64_t>(storage_);
}

std::uint64_t db_value::unsigned_value() const {
    return std::get<std::uint64_t>(storage_);
}

double db_value::double_value() const {
    return std::get<double>(storage_);
}

bool db_value::bool_value() const {
    return std::get<bool>(storage_);
}

db_field::db_field() noexcept = default;

db_field::db_field(std::nullptr_t) noexcept {}

db_field::db_field(std::string_view value)
    : storage_(std::in_place_type<std::string>, value) {}

db_field::db_field(borrowed_tag_type, std::string_view value) noexcept
    : storage_(std::in_place_type<borrowed_text>, value) {}

db_field db_field::borrowed(std::string_view value) noexcept {
    return db_field(borrowed_tag_type{}, value);
}

db_field::db_field(db_field&& other) noexcept
    : storage_(std::move(other.storage_)) {
    other.storage_.emplace<std::monostate>();
}

// NOLINTNEXTLINE(performance-noexcept-move-constructor)
db_field& db_field::operator=(db_field&& other) noexcept = default;

std::optional<std::string_view> db_field::value() const& noexcept {
    if (const auto* owned = std::get_if<std::string>(&storage_)) {
        return *owned;
    }
    if (const auto* borrowed = std::get_if<borrowed_text>(&storage_)) {
        return borrowed->view();
    }
    return std::nullopt;
}

db_row::db_row(const db_field* fields_value, std::size_t size, const std::string* column_names,
    std::size_t column_count) noexcept
    : fields_(fields_value, size),
      column_names_(column_names, column_count) {}

db_row::db_row(db_row&& other) noexcept
    : fields_(std::exchange(other.fields_, {})),
      column_names_(std::exchange(other.column_names_, {})) {}

db_row& db_row::operator=(db_row&& other) noexcept {
    if (this != &other) {
        fields_ = std::exchange(other.fields_, {});
        column_names_ = std::exchange(other.column_names_, {});
    }
    return *this;
}

bool db_row::empty() const noexcept {
    return fields_.empty();
}

std::size_t db_row::size() const noexcept {
    return fields_.size();
}

const db_field& db_row::operator[](std::size_t index) const& noexcept {
    return fields_[index];
}

bool db_row::contains(std::string_view column) const noexcept {
    return find(column) != nullptr;
}

const db_field* db_row::find(std::string_view column) const& noexcept {
    for (std::size_t index = 0; index < column_names_.size(); ++index) {
        if (column_names_[index] == column) {
            return &fields_[index];
        }
    }
    return nullptr;
}

const db_field& db_row::operator[](std::string_view column) const& {
    if (const auto* field = find(column)) {
        return *field;
    }
    throw std::out_of_range("database result has no such column");
}

const db_field* db_row::begin() const& noexcept {
    return fields_.data();
}

const db_field* db_row::end() const& noexcept {
    const auto* first = begin();
    return fields_.empty() ? first : first + fields_.size();
}

std::span<const std::string> db_row::column_names() const& noexcept {
    return column_names_;
}

db_rows::db_rows() noexcept = default;

db_rows::db_rows(db_rows&& other) noexcept
    : rows_(std::move(other.rows_)),
      fields_(std::move(other.fields_)),
      column_names_(std::move(other.column_names_)),
      raw_result_(other.raw_result_) {
    other.raw_result_.template emplace<no_raw_result_type>();
}

db_rows::~db_rows() {
    if (const auto* owned = std::get_if<owned_raw_result_type>(&raw_result_)) {
        owned->release_(owned->value_);
    }
}

db_rows& db_rows::operator=(db_rows&& other) {
    if (this == &other) {
        return *this;
    }
    if (const auto* owned = std::get_if<owned_raw_result_type>(&raw_result_)) {
        owned->release_(owned->value_);
    }
    rows_ = std::move(other.rows_);
    fields_ = std::move(other.fields_);
    column_names_ = std::move(other.column_names_);
    raw_result_ = other.raw_result_;
    other.raw_result_.template emplace<no_raw_result_type>();
    return *this;
}

bool db_rows::empty() const noexcept {
    return rows_.empty();
}

std::size_t db_rows::size() const noexcept {
    return rows_.size();
}

const db_row& db_rows::operator[](std::size_t index) const& noexcept {
    return rows_[index];
}

const db_row* db_rows::begin() const& noexcept {
    return rows_.data();
}

const db_row* db_rows::end() const& noexcept {
    const auto* first = begin();
    return rows_.empty() ? first : first + rows_.size();
}

const db_row& db_rows::front() const& noexcept {
    return rows_.front();
}

const db_row& db_rows::back() const& noexcept {
    return rows_.back();
}

std::span<const std::string> db_rows::column_names() const& noexcept {
    return column_names_;
}

db_migration_report::db_migration_report() noexcept = default;

db_migration_report& db_migration_report::operator=(db_migration_report&& other) = default;
std::span<const std::string> db_migration_report::applied() const& noexcept {
    return applied_;
}

std::span<const std::string> db_migration_report::skipped() const& noexcept {
    return skipped_;
}

bool db_migration_report::changed() const noexcept {
    return !applied_.empty();
}

}  // namespace ruvia
