#include "ruvia/web/db/Db.h"

#include "ruvia/web/detail/db/DbUtils.h"

#include <utility>

namespace ruvia {

DbValue::DbValue(std::nullptr_t) {}

DbValue::DbValue(const char* value) {
    if (value == nullptr) {
        return;
    }

    type_ = DbValueType::kString;
    text_ = value;
}

DbValue::DbValue(std::string_view value) : type_(DbValueType::kString), text_(value) {}

DbValue::DbValue(std::pmr::string value)
    : type_(DbValueType::kString),
      ownedText_(std::move(value)),
      ownsText_(true) {}

DbValue::DbValue(bool value) : type_(DbValueType::kBool), boolValue_(value) {}

DbValueType DbValue::type() const noexcept {
    return type_;
}

std::string_view DbValue::text() const noexcept {
    if (ownsText_) {
        return ownedText_;
    }
    return text_;
}

std::int64_t DbValue::signedValue() const noexcept {
    return signedValue_;
}

std::uint64_t DbValue::unsignedValue() const noexcept {
    return unsignedValue_;
}

double DbValue::doubleValue() const noexcept {
    return doubleValue_;
}

bool DbValue::boolValue() const noexcept {
    return boolValue_;
}

DbField::DbField(std::pmr::memory_resource* resource)
    : value_(detail::pmrResourceOrDefault(resource)) {}

DbField::DbField(std::nullptr_t, std::pmr::memory_resource* resource)
    : DbField(resource) {}

DbField::DbField(std::string_view value, std::pmr::memory_resource* resource)
    : isNull_(false),
      value_(value, detail::pmrResourceOrDefault(resource)),
      valueView_(value_),
      ownsValue_(true) {}

DbField::DbField(BorrowedTag, std::string_view value, std::pmr::memory_resource* resource)
    : isNull_(false),
      value_(detail::pmrResourceOrDefault(resource)),
      valueView_(value),
      ownsValue_(false) {}

DbField DbField::borrowed(std::string_view value, std::pmr::memory_resource* resource) {
    return DbField(BorrowedTag{}, value, resource);
}

DbField::DbField(DbField&& other) noexcept
    : isNull_(std::exchange(other.isNull_, true)),
      value_(std::move(other.value_)),
      valueView_(std::exchange(other.valueView_, {})),
      ownsValue_(std::exchange(other.ownsValue_, false)) {
    refreshView();
}

DbField& DbField::operator=(DbField&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    isNull_ = std::exchange(other.isNull_, true);
    value_ = std::move(other.value_);
    valueView_ = std::exchange(other.valueView_, {});
    ownsValue_ = std::exchange(other.ownsValue_, false);
    refreshView();
    return *this;
}

bool DbField::isNull() const noexcept {
    return isNull_;
}

std::string_view DbField::text() const noexcept {
    return valueView_;
}

void DbField::refreshView() noexcept {
    if (ownsValue_) {
        valueView_ = value_;
    }
}

DbRow::DbRow(std::pmr::memory_resource* resource)
    : ownedFields_(detail::pmrResourceOrDefault(resource)) {}

DbRow::DbRow(const DbField* fields, std::size_t size, std::pmr::memory_resource* resource)
    : ownedFields_(detail::pmrResourceOrDefault(resource)),
      fields_(fields),
      size_(size),
      ownsFields_(false) {}

DbRow::DbRow(DbRow&& other) noexcept
    : ownedFields_(std::move(other.ownedFields_)),
      fields_(std::exchange(other.fields_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      ownsFields_(std::exchange(other.ownsFields_, true)) {
    refreshView();
}

DbRow& DbRow::operator=(DbRow&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    ownedFields_ = std::move(other.ownedFields_);
    fields_ = std::exchange(other.fields_, nullptr);
    size_ = std::exchange(other.size_, 0);
    ownsFields_ = std::exchange(other.ownsFields_, true);
    refreshView();
    return *this;
}

bool DbRow::empty() const noexcept {
    return size_ == 0;
}

std::size_t DbRow::size() const noexcept {
    return size_;
}

const DbField& DbRow::operator[](std::size_t index) const noexcept {
    return fields_[index];
}

const DbField* DbRow::begin() const noexcept {
    return fields_;
}

const DbField* DbRow::end() const noexcept {
    return fields_ + size_;
}

void DbRow::refreshView() noexcept {
    if (ownsFields_) {
        fields_ = ownedFields_.data();
        size_ = ownedFields_.size();
    }
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
      mounted_(std::exchange(other.mounted_, nullptr)),
      rawResult_(std::exchange(other.rawResult_, nullptr)),
      releaseRawResult_(std::exchange(other.releaseRawResult_, nullptr)) {}

QueryResult& QueryResult::operator=(QueryResult&& other) {
    if (this == &other) {
        return *this;
    }

    if (rawResult_ != nullptr && releaseRawResult_ != nullptr) {
        releaseRawResult_(rawResult_);
    }
    rawResult_ = nullptr;
    releaseRawResult_ = nullptr;

    rows_ = std::move(other.rows_);
    fields_ = std::move(other.fields_);
    affectedRows_ = std::exchange(other.affectedRows_, 0);
    lastInsertId_ = std::exchange(other.lastInsertId_, 0);
    mounted_ = std::exchange(other.mounted_, nullptr);
    rawResult_ = std::exchange(other.rawResult_, nullptr);
    releaseRawResult_ = std::exchange(other.releaseRawResult_, nullptr);
    return *this;
}

QueryResult::~QueryResult() {
    if (rawResult_ != nullptr && releaseRawResult_ != nullptr) {
        releaseRawResult_(rawResult_);
    }
}

std::span<const DbRow> QueryResult::rows() const noexcept {
    const auto& result = mounted_ == nullptr ? *this : *mounted_;
    return std::span<const DbRow>(result.rows_.data(), result.rows_.size());
}

std::uint64_t QueryResult::affectedRows() const noexcept {
    const auto& result = mounted_ == nullptr ? *this : *mounted_;
    return result.affectedRows_;
}

std::uint64_t QueryResult::lastInsertId() const noexcept {
    const auto& result = mounted_ == nullptr ? *this : *mounted_;
    return result.lastInsertId_;
}

DbMigrationReport::DbMigrationReport(std::pmr::memory_resource* resource)
    : DbMigrationReport(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

DbMigrationReport::DbMigrationReport(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
    : applied_(resource),
      skipped_(resource) {}

std::span<const std::pmr::string> DbMigrationReport::applied() const noexcept {
    return std::span<const std::pmr::string>(applied_.data(), applied_.size());
}

std::span<const std::pmr::string> DbMigrationReport::skipped() const noexcept {
    return std::span<const std::pmr::string>(skipped_.data(), skipped_.size());
}

bool DbMigrationReport::changed() const noexcept {
    return !applied_.empty();
}

}  // namespace ruvia
