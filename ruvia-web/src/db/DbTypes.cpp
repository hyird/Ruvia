#include "ruvia/web/db/Db.h"

#include "ruvia/web/detail/db/DbUtils.h"

#include <utility>

namespace ruvia {

DbValue::DbValue(std::nullptr_t)
    : storage_(std::monostate{}) {}

DbValue::DbValue(const char* value)
    : storage_(value == nullptr ? Storage(std::monostate{}) : Storage(std::in_place_type<BorrowedText>, value)) {}

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
            } else if constexpr (std::is_same_v<Value, BorrowedText> || std::is_same_v<Value, std::pmr::string>) {
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
        return borrowed->value;
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

bool DbField::isNull() const noexcept {
    return std::holds_alternative<std::monostate>(storage_);
}

std::string_view DbField::text() const& noexcept {
    if (const auto* owned = std::get_if<std::pmr::string>(&storage_)) {
        return *owned;
    }
    if (const auto* borrowed = std::get_if<BorrowedText>(&storage_)) {
        return borrowed->value;
    }
    return {};
}

DbRow::DbRow(std::pmr::memory_resource* resource)
    : resource_(detail::pmrResourceOrDefault(resource)),
      storage_(std::in_place_type<OwnedFields>, resource_) {}

DbRow::DbRow(const DbField* fields, std::size_t size, std::pmr::memory_resource* resource)
    : resource_(detail::pmrResourceOrDefault(resource)),
      storage_(std::in_place_type<BorrowedFields>, fields, size) {}

DbRow::DbRow(DbRow&& other) noexcept
    : resource_(other.resource_),
      storage_([&other]() noexcept -> Storage {
          if (auto* owned = std::get_if<OwnedFields>(&other.storage_)) {
              return Storage(std::in_place_type<OwnedFields>, std::move(*owned));
          }
          return Storage(std::in_place_type<BorrowedFields>, std::get<BorrowedFields>(other.storage_));
      }()) {
    other.storage_.emplace<OwnedFields>(other.resource_);
}

DbRow& DbRow::operator=(DbRow&& other) {
    if (this == &other) {
        return *this;
    }
    if (auto* owned = std::get_if<OwnedFields>(&other.storage_)) {
        if (auto* destination = std::get_if<OwnedFields>(&storage_)) {
            *destination = std::move(*owned);
        } else {
            OwnedFields replacement(std::move(*owned), resource_);
            storage_.emplace<OwnedFields>(std::move(replacement));
        }
    } else {
        storage_.emplace<BorrowedFields>(std::get<BorrowedFields>(other.storage_));
    }
    other.storage_.emplace<OwnedFields>(other.resource_);
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

QueryResult::QueryResult(std::pmr::memory_resource* resource)
    : QueryResult(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

QueryResult::QueryResult(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
    : rows_(resource),
      fields_(resource) {}

QueryResult::QueryResult(QueryResult&& other) noexcept
    : rows_(std::move(other.rows_)),
      fields_(std::move(other.fields_)),
      affectedRows_(std::exchange(other.affectedRows_, 0)),
      lastInsertId_(std::exchange(other.lastInsertId_, 0)),
      rawResult_(std::move(other.rawResult_)) {
    other.rawResult_.template emplace<NoRawResult>();
}

QueryResult::~QueryResult() {
    if (const auto* owned = std::get_if<OwnedRawResult>(&rawResult_)) {
        owned->release(owned->value);
    }
}

std::span<const DbRow> QueryResult::rows() const& noexcept {
    return rows_;
}

std::uint64_t QueryResult::affectedRows() const noexcept {
    return affectedRows_;
}

std::uint64_t QueryResult::lastInsertId() const noexcept {
    return lastInsertId_;
}

DbMigrationReport::DbMigrationReport(std::pmr::memory_resource* resource)
    : DbMigrationReport(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

DbMigrationReport::DbMigrationReport(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
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
