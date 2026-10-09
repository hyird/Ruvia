#include "ruvia/web/db/db_types.h"

#include <utility>
#include <variant>

#include "ruvia/web/db/db_migration.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/detail/db/db_utils.h"

namespace ruvia {

db_field::db_field(std::pmr::memory_resource* resource)
    : resource_(detail::pmr_resource_or_default(resource)),
      storage_(std::monostate{}) {}

db_field::db_field(const db_field& other, std::pmr::memory_resource* resource)
    : db_field(resource) {
    if (const auto* owned = std::get_if<std::pmr::string>(&other.storage_)) {
        storage_.emplace<std::pmr::string>(*owned, resource_);
    } else if (const auto* borrowed = std::get_if<borrowed_text>(&other.storage_)) {
        storage_.emplace<borrowed_text>(*borrowed);
    }
}

db_field::db_field(std::nullptr_t, std::pmr::memory_resource* resource)
    : db_field(resource) {}

db_field::db_field(std::string_view value, std::pmr::memory_resource* resource)
    : resource_(detail::pmr_resource_or_default(resource)),
      storage_(std::in_place_type<std::pmr::string>, value, resource_) {}

db_field::db_field(borrowed_tag_type, std::string_view value, std::pmr::memory_resource* resource)
    : resource_(detail::pmr_resource_or_default(resource)),
      storage_(std::in_place_type<borrowed_text>, value) {}

db_field db_field::borrowed(std::string_view value, std::pmr::memory_resource* resource) {
    return db_field(borrowed_tag_type{}, value, resource);
}

db_field::db_field(db_field&& other) noexcept
    : resource_(other.resource_),
      storage_(std::move(other.storage_)) {
    other.storage_.emplace<std::monostate>();
}

// NOLINTNEXTLINE(performance-noexcept-move-constructor)
db_field& db_field::operator=(db_field&& other) {
    if (this == &other) {
        return *this;
    }
    if (auto* owned = std::get_if<std::pmr::string>(&other.storage_)) {
        if (auto* destination = std::get_if<std::pmr::string>(&storage_)) {
            *destination = std::move(*owned);
        } else {
            std::pmr::string replacement(std::move(*owned), resource_);
            storage_.emplace<std::pmr::string>(std::move(replacement));
        }
    } else if (const auto* borrowed = std::get_if<borrowed_text>(&other.storage_)) {
        storage_.emplace<borrowed_text>(*borrowed);
    } else {
        storage_.emplace<std::monostate>();
    }
    other.storage_.emplace<std::monostate>();
    return *this;
}

std::optional<std::string_view> db_field::value() const& noexcept {
    if (const auto* owned = std::get_if<std::pmr::string>(&storage_)) {
        return *owned;
    }
    if (const auto* borrowed = std::get_if<borrowed_text>(&storage_)) {
        return borrowed->view();
    }
    return std::nullopt;
}

db_row::db_row(std::pmr::memory_resource* resource)
    : resource_(detail::pmr_resource_or_default(resource)),
      storage_(std::in_place_type<owned_fields_type>, resource_),
      column_names_(std::in_place_type<owned_column_names_type>, resource_) {}

db_row::db_row(const db_field* fields_value, std::size_t size, const std::pmr::string* column_names,
    std::size_t column_count, std::pmr::memory_resource* resource)
    : resource_(detail::pmr_resource_or_default(resource)),
      storage_(std::in_place_type<borrowed_fields_type>, fields_value, size),
      column_names_(std::in_place_type<borrowed_column_names_type>, column_names, column_count) {}

db_row::db_row(db_row&& other) noexcept
    : resource_(other.resource_),
      storage_([&other]() noexcept -> storage_type {
          if (auto* owned = std::get_if<owned_fields_type>(&other.storage_)) {
              return storage_type(std::in_place_type<owned_fields_type>, std::move(*owned));
          }
          return storage_type(
              std::in_place_type<borrowed_fields_type>, std::get<borrowed_fields_type>(other.storage_));
      }()),
      column_names_([&other]() noexcept -> column_name_storage_type {
          if (auto* owned = std::get_if<owned_column_names_type>(&other.column_names_)) {
              return column_name_storage_type(std::in_place_type<owned_column_names_type>, std::move(*owned));
          }
          return column_name_storage_type(std::in_place_type<borrowed_column_names_type>,
              std::get<borrowed_column_names_type>(other.column_names_));
      }()) {
    other.storage_.emplace<borrowed_fields_type>();
    other.column_names_.emplace<borrowed_column_names_type>();
}

// NOLINTNEXTLINE(performance-noexcept-move-constructor)
db_row& db_row::operator=(db_row&& other) {
    if (this == &other) {
        return *this;
    }
    if (resource_ == other.resource_) {
        storage_.swap(other.storage_);
        column_names_.swap(other.column_names_);
    } else {
        // Prepare both halves before changing either row. Moving vector elements
        // across allocators would retain each db_field's source resource.
        auto fields_value = [&]() -> storage_type {
            if (const auto* owned = std::get_if<owned_fields_type>(&other.storage_)) {
                owned_fields_type replacement(resource_);
                replacement.reserve(owned->size());
                for (const auto& field : *owned) {
                    replacement.push_back(db_field(field, resource_));
                }
                return storage_type(std::in_place_type<owned_fields_type>, std::move(replacement));
            }
            return storage_type(std::in_place_type<borrowed_fields_type>, std::get<borrowed_fields_type>(other.storage_));
        }();
        auto names = [&]() -> column_name_storage_type {
            if (const auto* owned = std::get_if<owned_column_names_type>(&other.column_names_)) {
                return column_name_storage_type(std::in_place_type<owned_column_names_type>, *owned, resource_);
            }
            return column_name_storage_type(std::in_place_type<borrowed_column_names_type>,
                std::get<borrowed_column_names_type>(other.column_names_));
        }();
        storage_.swap(fields_value);
        column_names_.swap(names);
    }
    other.storage_.emplace<borrowed_fields_type>();
    other.column_names_.emplace<borrowed_column_names_type>();
    return *this;
}

bool db_row::empty() const noexcept {
    return size() == 0;
}

std::size_t db_row::size() const noexcept {
    if (const auto* owned = std::get_if<owned_fields_type>(&storage_)) {
        return owned->size();
    }
    return std::get<borrowed_fields_type>(storage_).size();
}

const db_field& db_row::operator[](std::size_t index) const& noexcept {
    if (const auto* owned = std::get_if<owned_fields_type>(&storage_)) {
        return (*owned)[index];
    }
    return std::get<borrowed_fields_type>(storage_)[index];
}

const db_field& db_row::operator[](std::string_view column) const& {
    const auto names = column_names();
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == column) {
            return (*this)[index];
        }
    }
    throw std::out_of_range("database result has no such column");
}

const db_field* db_row::begin() const& noexcept {
    if (const auto* owned = std::get_if<owned_fields_type>(&storage_)) {
        return owned->data();
    }
    return std::get<borrowed_fields_type>(storage_).data();
}

const db_field* db_row::end() const& noexcept {
    const auto* first = begin();
    const auto count = size();
    return count == 0 ? first : first + count;
}

db_row::owned_fields_type& db_row::owned_fields() noexcept {
    return std::get<owned_fields_type>(storage_);
}

db_row::owned_column_names_type& db_row::owned_column_names() noexcept {
    return std::get<owned_column_names_type>(column_names_);
}

std::span<const std::pmr::string> db_row::column_names() const noexcept {
    if (const auto* owned = std::get_if<owned_column_names_type>(&column_names_)) {
        return *owned;
    }
    return std::get<borrowed_column_names_type>(column_names_);
}

db_rows::db_rows(std::pmr::memory_resource* resource)
    : db_rows(detail::resolved_pmr_resource_tag{}, detail::pmr_resource_or_default(resource)) {}

db_rows::db_rows(detail::resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
    : rows_(resource),
      fields_(resource),
      column_names_(resource) {}

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

db_migration_report::db_migration_report(std::pmr::memory_resource* resource)
    : db_migration_report(detail::resolved_pmr_resource_tag{}, detail::pmr_resource_or_default(resource)) {}

db_migration_report::db_migration_report(
    detail::resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
    : applied_(resource),
      skipped_(resource) {}

std::span<const std::pmr::string> db_migration_report::applied() const& noexcept {
    return applied_;
}

std::span<const std::pmr::string> db_migration_report::skipped() const& noexcept {
    return skipped_;
}

bool db_migration_report::changed() const noexcept {
    return !applied_.empty();
}

}  // namespace ruvia
