#include "ruvia/db/Db.h"

#include "../DbInternal.h"
#include "DbUtils.h"
#include "ruvia/memory/MemoryPool.h"

#include <stdexcept>
#include <utility>

namespace ruvia {

DbHandle::DbHandle(
    detail::MariaDbPool& client,
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) noexcept
    : client_(client),
      resource_(detail::resolveDbResource(resource)),
      requestMemory_(requestMemory) {}

QueryResult DbHandle::mountResult(QueryResult result) const {
    if (requestMemory_ == nullptr) {
        return result;
    }
    const auto& mounted = requestMemory_->emplace<QueryResult>(std::move(result));
    QueryResult view(resource_);
    view.mounted_ = &mounted;
    return view;
}

Task<QueryResult> DbHandle::query(std::string_view sql, std::initializer_list<DbValue> params) const {
    return query(sql, std::span<const DbValue>(params.begin(), params.size()));
}

Task<QueryResult> DbHandle::query(std::string_view sql, std::span<const DbValue> params) const {
    std::pmr::string sqlCopy(sql, resource_);
    auto paramCopy = detail::cloneDbValues(params, resource_);

    auto result = co_await client_.execute(std::move(sqlCopy), std::move(paramCopy), resource_);
    co_return mountResult(std::move(result));
}

Task<QueryResult> DbHandle::execute(std::string_view sql, std::initializer_list<DbValue> params) const {
    return execute(sql, std::span<const DbValue>(params.begin(), params.size()));
}

Task<QueryResult> DbHandle::execute(std::string_view sql, std::span<const DbValue> params) const {
    std::pmr::string sqlCopy(sql, resource_);
    auto paramCopy = detail::cloneDbValues(params, resource_);

    auto result = co_await client_.execute(std::move(sqlCopy), std::move(paramCopy), resource_);
    co_return mountResult(std::move(result));
}

Task<DbStreamResult> DbHandle::queryStream(std::string_view sql, std::initializer_list<DbValue> params) const {
    return queryStream(sql, std::span<const DbValue>(params.begin(), params.size()));
}

Task<DbStreamResult> DbHandle::queryStream(std::string_view sql, std::span<const DbValue> params) const {
    std::pmr::string sqlCopy(sql, resource_);
    auto paramCopy = detail::cloneDbValues(params, resource_);

    return client_.stream(std::move(sqlCopy), std::move(paramCopy), resource_);
}

Task<DbTransaction> DbHandle::beginTransaction() const {
    return client_.beginTransaction(resource_, requestMemory_);
}

DbStreamResult::DbStreamResult(
    detail::MariaDbPool& client,
    std::size_t slot,
    void* result,
    std::pmr::memory_resource* resource) noexcept
    : client_(&client),
      slot_(slot),
      result_(result),
      resource_(detail::resolveDbResource(resource)),
      active_(result != nullptr) {}

DbStreamResult::DbStreamResult(DbStreamResult&& other) noexcept
    : client_(std::exchange(other.client_, nullptr)),
      slot_(std::exchange(other.slot_, 0)),
      result_(std::exchange(other.result_, nullptr)),
      resource_(std::exchange(other.resource_, std::pmr::get_default_resource())),
      active_(std::exchange(other.active_, false)) {}

DbStreamResult& DbStreamResult::operator=(DbStreamResult&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    client_ = std::exchange(other.client_, nullptr);
    slot_ = std::exchange(other.slot_, 0);
    result_ = std::exchange(other.result_, nullptr);
    resource_ = std::exchange(other.resource_, std::pmr::get_default_resource());
    active_ = std::exchange(other.active_, false);
    return *this;
}

DbStreamResult::~DbStreamResult() {
    reset();
}

bool DbStreamResult::active() const noexcept {
    return active_;
}

Task<std::optional<DbRow>> DbStreamResult::read() {
    if (!active_ || client_ == nullptr || result_ == nullptr) {
        co_return std::nullopt;
    }

    try {
        auto row = co_await client_->readStreamRow(slot_, result_, resource_);
        if (!row) {
            release();
        }
        co_return row;
    } catch (...) {
        release();
        throw;
    }
}

Task<void> DbStreamResult::close() {
    if (!active_ || client_ == nullptr || result_ == nullptr) {
        co_return;
    }
    active_ = false;
    auto* client = client_;
    const auto slot = slot_;
    auto* result = result_;
    auto* resource = resource_;
    client_ = nullptr;
    slot_ = 0;
    result_ = nullptr;
    co_await client->closeStream(slot, result, resource);
}

void DbStreamResult::reset() noexcept {
    if (active_ && client_ != nullptr && result_ != nullptr) {
        client_->abortStream(slot_, result_);
    }
    client_ = nullptr;
    slot_ = 0;
    result_ = nullptr;
    active_ = false;
}

void DbStreamResult::release() noexcept {
    client_ = nullptr;
    slot_ = 0;
    result_ = nullptr;
    active_ = false;
}

DbTransaction::DbTransaction(
    detail::MariaDbPool& client,
    std::size_t slot,
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) noexcept
    : client_(&client),
      slot_(slot),
      resource_(detail::resolveDbResource(resource)),
      requestMemory_(requestMemory),
      active_(true) {}

DbTransaction::DbTransaction(DbTransaction&& other) noexcept
    : client_(std::exchange(other.client_, nullptr)),
      slot_(std::exchange(other.slot_, 0)),
      resource_(std::exchange(other.resource_, std::pmr::get_default_resource())),
      requestMemory_(std::exchange(other.requestMemory_, nullptr)),
      active_(std::exchange(other.active_, false)) {}

DbTransaction& DbTransaction::operator=(DbTransaction&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    client_ = std::exchange(other.client_, nullptr);
    slot_ = std::exchange(other.slot_, 0);
    resource_ = std::exchange(other.resource_, std::pmr::get_default_resource());
    requestMemory_ = std::exchange(other.requestMemory_, nullptr);
    active_ = std::exchange(other.active_, false);
    return *this;
}

DbTransaction::~DbTransaction() {
    reset();
}

bool DbTransaction::active() const noexcept {
    return active_;
}

QueryResult DbTransaction::mountResult(QueryResult result) const {
    if (requestMemory_ == nullptr) {
        return result;
    }

    const auto& mounted = requestMemory_->emplace<QueryResult>(std::move(result));
    QueryResult view(resource_);
    view.mounted_ = &mounted;
    return view;
}

Task<QueryResult> DbTransaction::query(std::string_view sql, std::initializer_list<DbValue> params) {
    return query(sql, std::span<const DbValue>(params.begin(), params.size()));
}

Task<QueryResult> DbTransaction::query(std::string_view sql, std::span<const DbValue> params) {
    if (!active_ || client_ == nullptr) {
        throw std::logic_error("database transaction is not active");
    }

    std::pmr::string sqlCopy(sql, resource_);
    auto paramCopy = detail::cloneDbValues(params, resource_);

    try {
        auto result = co_await client_->executeOnTransactionSlot(
            slot_,
            std::move(sqlCopy),
            std::move(paramCopy),
            resource_);
        co_return mountResult(std::move(result));
    } catch (...) {
        client_ = nullptr;
        slot_ = 0;
        active_ = false;
        throw;
    }
}

Task<QueryResult> DbTransaction::execute(std::string_view sql, std::initializer_list<DbValue> params) {
    return execute(sql, std::span<const DbValue>(params.begin(), params.size()));
}

Task<QueryResult> DbTransaction::execute(std::string_view sql, std::span<const DbValue> params) {
    if (!active_ || client_ == nullptr) {
        throw std::logic_error("database transaction is not active");
    }

    std::pmr::string sqlCopy(sql, resource_);
    auto paramCopy = detail::cloneDbValues(params, resource_);

    try {
        auto result = co_await client_->executeOnTransactionSlot(
            slot_,
            std::move(sqlCopy),
            std::move(paramCopy),
            resource_);
        co_return mountResult(std::move(result));
    } catch (...) {
        client_ = nullptr;
        slot_ = 0;
        active_ = false;
        throw;
    }
}

Task<void> DbTransaction::commit() {
    if (!active_ || client_ == nullptr) {
        throw std::logic_error("database transaction is not active");
    }
    active_ = false;
    auto* client = client_;
    const auto slot = slot_;
    auto* resource = resource_;
    client_ = nullptr;
    slot_ = 0;
    return client->commitTransaction(slot, resource);
}

Task<void> DbTransaction::rollback() {
    if (!active_ || client_ == nullptr) {
        throw std::logic_error("database transaction is not active");
    }
    active_ = false;
    auto* client = client_;
    const auto slot = slot_;
    auto* resource = resource_;
    client_ = nullptr;
    slot_ = 0;
    return client->rollbackTransaction(slot, resource);
}

void DbTransaction::reset() noexcept {
    if (active_ && client_ != nullptr) {
        client_->abortTransaction(slot_);
    }
    client_ = nullptr;
    slot_ = 0;
    active_ = false;
}

void DbTransaction::release() noexcept {
    if (active_ && client_ != nullptr) {
        client_->finishTransaction(slot_);
    }
    client_ = nullptr;
    slot_ = 0;
    active_ = false;
}

}  // namespace ruvia
