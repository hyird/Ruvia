#include "ruvia/web/db/db_types.h"

#include <utility>
#include <variant>

#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_schema.h"
#include "ruvia/web/detail/db/db_utils.h"

#include "util/pmr_owned_storage.h"

namespace ruvia {

db_field::db_field(std::pmr::memory_resource* resource)
    : resource_(pmr_resource_or_default(resource)),
      storage_(std::monostate{}) {}

db_field::db_field(std::nullptr_t, std::pmr::memory_resource* resource)
    : db_field(resource) {}

db_field::db_field(std::string_view value, std::pmr::memory_resource* resource)
    : resource_(pmr_resource_or_default(resource)),
      storage_(std::in_place_type<std::pmr::string>, value, resource_) {}

db_field::db_field(borrowed_tag_type, std::string_view value, std::pmr::memory_resource* resource)
    : resource_(pmr_resource_or_default(resource)),
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
        auto replacement = detail::rebind_owned_storage(*owned, resource_);
        if (auto* destination = std::get_if<std::pmr::string>(&storage_)) {
            destination->swap(replacement);
        } else {
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

db_row::db_row(const db_field* fields_value, std::size_t size, const std::pmr::string* column_names,
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

std::span<const std::pmr::string> db_row::column_names() const& noexcept {
    return column_names_;
}

db_rows::db_rows(std::pmr::memory_resource* resource)
    : db_rows(resolved_pmr_resource_tag{}, pmr_resource_or_default(resource)) {}

db_rows::db_rows(resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
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

db_rows& db_rows::operator=(db_rows&& other) {
    if (this == &other) {
        return *this;
    }
    auto* const target = rows_.get_allocator().resource();
    if (other.rows_.get_allocator().resource() == target &&
        other.fields_.get_allocator().resource() == target &&
        other.column_names_.get_allocator().resource() == target) {
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
    std::pmr::vector<std::pmr::string> new_column_names(other.column_names_, target);
    std::pmr::vector<db_field> new_fields(target);
    new_fields.reserve(other.fields_.size());
    for (const auto& field : other.fields_) {
        if (const auto text = field.value()) {
            new_fields.push_back(db_field(*text, target));
        } else {
            new_fields.push_back(db_field(nullptr, target));
        }
    }
    std::pmr::vector<db_row> new_rows(target);
    new_rows.reserve(other.rows_.size());
    std::size_t field_offset = 0;
    for (const auto& row : other.rows_) {
        new_rows.push_back(db_row(
            new_fields.data() + field_offset, row.size(), new_column_names.data(), new_column_names.size()));
        field_offset += row.size();
    }
    if (const auto* owned = std::get_if<owned_raw_result_type>(&raw_result_)) {
        owned->release_(owned->value_);
    }
    raw_result_.template emplace<no_raw_result_type>();
    rows_.swap(new_rows);
    fields_.swap(new_fields);
    column_names_.swap(new_column_names);
    if (const auto* owned = std::get_if<owned_raw_result_type>(&other.raw_result_)) {
        owned->release_(owned->value_);
    }
    other.raw_result_.template emplace<no_raw_result_type>();
    other.rows_.clear();
    other.fields_.clear();
    other.column_names_.clear();
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

std::span<const std::pmr::string> db_rows::column_names() const& noexcept {
    return column_names_;
}

db_migration_report::db_migration_report(std::pmr::memory_resource* resource)
    : db_migration_report(resolved_pmr_resource_tag{}, pmr_resource_or_default(resource)) {}

db_migration_report::db_migration_report(
    resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
    : applied_(resource),
      skipped_(resource) {}

db_migration_report& db_migration_report::operator=(db_migration_report&& other) {
    if (this != &other) {
        auto* const target = applied_.get_allocator().resource();
        auto new_applied = detail::rebind_owned_storage(other.applied_, target, other.skipped_);
        auto new_skipped = detail::rebind_owned_storage(other.skipped_, target, other.applied_);
        applied_.swap(new_applied);
        skipped_.swap(new_skipped);
        other.applied_.clear();
        other.skipped_.clear();
    }
    return *this;
}
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
