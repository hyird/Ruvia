#include "ruvia/web/db/DbTypes.h"

#include <utility>

#include "ruvia/web/db/DbMigration.h"
#include "ruvia/web/db/DbRows.h"
#include "ruvia/web/detail/db/DbUtils.h"

namespace ruvia {

DbField::DbField(std::pmr::memory_resource* resource)
    : resource_(detail::pmrResourceOrDefault(resource)),
      storage_(std::monostate{}) {}

DbField::DbField(std::nullptr_t, std::pmr::memory_resource* resource)
    : DbField(resource) {}

DbField::DbField(std::string_view value, std::pmr::memory_resource* resource)
    : resource_(detail::pmrResourceOrDefault(resource)),
      storage_(std::in_place_type<std::pmr::string>, value, resource_) {}

DbField::DbField(BorrowedTag, std::string_view value, std::pmr::memory_resource* resource)
    : resource_(detail::pmrResourceOrDefault(resource)),
      storage_(std::in_place_type<BorrowedText>, value) {}

DbField DbField::borrowed(std::string_view value, std::pmr::memory_resource* resource) {
    return DbField(BorrowedTag{}, value, resource);
}

DbField::DbField(DbField&& other) noexcept
    : resource_(other.resource_),
      storage_(std::move(other.storage_)) {
    other.storage_.emplace<std::monostate>();
}

// NOLINTNEXTLINE(performance-noexcept-move-constructor)
DbField& DbField::operator=(DbField&& other) {
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
    } else if (const auto* borrowed = std::get_if<BorrowedText>(&other.storage_)) {
        storage_.emplace<BorrowedText>(*borrowed);
    } else {
        storage_.emplace<std::monostate>();
    }
    other.storage_.emplace<std::monostate>();
    return *this;
}

std::optional<std::string_view> DbField::value() const& noexcept {
    if (const auto* owned = std::get_if<std::pmr::string>(&storage_)) {
        return *owned;
    }
    if (const auto* borrowed = std::get_if<BorrowedText>(&storage_)) {
        return borrowed->view();
    }
    return std::nullopt;
}

DbRow::DbRow(std::pmr::memory_resource* resource)
    : resource_(detail::pmrResourceOrDefault(resource)),
      storage_(std::in_place_type<OwnedFields>, resource_),
      columnNames_(std::in_place_type<OwnedColumnNames>, resource_) {}

DbRow::DbRow(const DbField* fields, std::size_t size, const std::pmr::string* columnNames,
    std::size_t columnCount, std::pmr::memory_resource* resource)
    : resource_(detail::pmrResourceOrDefault(resource)),
      storage_(std::in_place_type<BorrowedFields>, fields, size),
      columnNames_(std::in_place_type<BorrowedColumnNames>, columnNames, columnCount) {}

DbRow::DbRow(DbRow&& other) noexcept
    : resource_(other.resource_),
      storage_([&other]() noexcept -> Storage {
          if (auto* owned = std::get_if<OwnedFields>(&other.storage_)) {
              return Storage(std::in_place_type<OwnedFields>, std::move(*owned));
          }
          return Storage(
              std::in_place_type<BorrowedFields>, std::get<BorrowedFields>(other.storage_));
      }()),
      columnNames_([&other]() noexcept -> ColumnNameStorage {
          if (auto* owned = std::get_if<OwnedColumnNames>(&other.columnNames_)) {
              return ColumnNameStorage(std::in_place_type<OwnedColumnNames>, std::move(*owned));
          }
          return ColumnNameStorage(std::in_place_type<BorrowedColumnNames>,
              std::get<BorrowedColumnNames>(other.columnNames_));
      }()) {
    other.storage_.emplace<OwnedFields>(other.resource_);
    other.columnNames_.emplace<OwnedColumnNames>(other.resource_);
}

// NOLINTNEXTLINE(performance-noexcept-move-constructor)
DbRow& DbRow::operator=(DbRow&& other) {
    if (this == &other) {
        return *this;
    }

    // Prepare both halves before publishing either one. A later allocation
    // failure must not leave old column names indexing a shorter field array.
    Storage next_fields(std::in_place_type<BorrowedFields>);
    ColumnNameStorage next_names(std::in_place_type<BorrowedColumnNames>);
    const bool same_resource = resource_ == other.resource_;
    if (auto* owned = std::get_if<OwnedColumnNames>(&other.columnNames_)) {
        if (same_resource) {
            next_names.emplace<OwnedColumnNames>(std::move(*owned));
        } else {
            next_names.emplace<OwnedColumnNames>(*owned, resource_);
        }
    } else {
        next_names.emplace<BorrowedColumnNames>(
            std::get<BorrowedColumnNames>(other.columnNames_));
    }
    if (auto* owned = std::get_if<OwnedFields>(&other.storage_)) {
        if (same_resource) {
            next_fields.emplace<OwnedFields>(std::move(*owned));
        } else {
            OwnedFields fields(resource_);
            fields.reserve(owned->size());
            for (auto& field : *owned) {
                // Moving the vector alone relocates its buffer but retains each
                // field's allocator. Normalize through the field assignment.
                DbField replacement(resource_);
                replacement = std::move(field);
                fields.push_back(std::move(replacement));
            }
            next_fields.emplace<OwnedFields>(std::move(fields));
        }
    } else {
        next_fields.emplace<BorrowedFields>(std::get<BorrowedFields>(other.storage_));
    }

    storage_ = std::move(next_fields);
    columnNames_ = std::move(next_names);
    other.storage_.emplace<OwnedFields>(other.resource_);
    other.columnNames_.emplace<OwnedColumnNames>(other.resource_);
    return *this;
}

bool DbRow::empty() const noexcept {
    return size() == 0;
}

std::size_t DbRow::size() const noexcept {
    if (const auto* owned = std::get_if<OwnedFields>(&storage_)) {
        return owned->size();
    }
    return std::get<BorrowedFields>(storage_).size();
}

const DbField& DbRow::operator[](std::size_t index) const& noexcept {
    if (const auto* owned = std::get_if<OwnedFields>(&storage_)) {
        return (*owned)[index];
    }
    return std::get<BorrowedFields>(storage_)[index];
}

const DbField& DbRow::operator[](std::string_view column) const& {
    const auto names = columnNames();
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == column) {
            return (*this)[index];
        }
    }
    throw std::out_of_range("database result has no such column");
}

const DbField* DbRow::begin() const& noexcept {
    if (const auto* owned = std::get_if<OwnedFields>(&storage_)) {
        return owned->data();
    }
    return std::get<BorrowedFields>(storage_).data();
}

const DbField* DbRow::end() const& noexcept {
    const auto* first = begin();
    const auto count = size();
    return count == 0 ? first : first + count;
}

DbRow::OwnedFields& DbRow::ownedFields() noexcept {
    return std::get<OwnedFields>(storage_);
}

DbRow::OwnedColumnNames& DbRow::ownedColumnNames() noexcept {
    return std::get<OwnedColumnNames>(columnNames_);
}

std::span<const std::pmr::string> DbRow::columnNames() const noexcept {
    if (const auto* owned = std::get_if<OwnedColumnNames>(&columnNames_)) {
        return *owned;
    }
    return std::get<BorrowedColumnNames>(columnNames_);
}

DbRows::DbRows(std::pmr::memory_resource* resource)
    : DbRows(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

DbRows::DbRows(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
    : rows_(resource),
      fields_(resource),
      columnNames_(resource) {}

DbRows::DbRows(DbRows&& other) noexcept
    : rows_(std::move(other.rows_)),
      fields_(std::move(other.fields_)),
      columnNames_(std::move(other.columnNames_)),
      rawResult_(other.rawResult_) {
    other.rawResult_.template emplace<NoRawResult>();
}

DbRows::~DbRows() {
    if (const auto* owned = std::get_if<OwnedRawResult>(&rawResult_)) {
        owned->release(owned->value);
    }
}

bool DbRows::empty() const noexcept {
    return rows_.empty();
}

std::size_t DbRows::size() const noexcept {
    return rows_.size();
}

const DbRow& DbRows::operator[](std::size_t index) const& noexcept {
    return rows_[index];
}

const DbRow* DbRows::begin() const& noexcept {
    return rows_.data();
}

const DbRow* DbRows::end() const& noexcept {
    const auto* first = begin();
    return rows_.empty() ? first : first + rows_.size();
}

const DbRow& DbRows::front() const& noexcept {
    return rows_.front();
}

DbMigrationReport::DbMigrationReport(std::pmr::memory_resource* resource)
    : DbMigrationReport(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

DbMigrationReport::DbMigrationReport(
    detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
    : applied_(resource),
      skipped_(resource) {}

std::span<const std::pmr::string> DbMigrationReport::applied() const& noexcept {
    return applied_;
}

std::span<const std::pmr::string> DbMigrationReport::skipped() const& noexcept {
    return skipped_;
}

bool DbMigrationReport::changed() const noexcept {
    return !applied_.empty();
}

}  // namespace ruvia
