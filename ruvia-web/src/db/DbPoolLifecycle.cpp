#include "ruvia/web/detail/db/DbInternal.h"
#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbSlotSocket.h"
#include "ruvia/web/detail/db/DbUtils.h"

#include <mysql/mysql.h>

#include <memory_resource>
#include <utility>

namespace ruvia {

detail::MariaDbPool::ConnectionSlot::ConnectionSlot(std::pmr::memory_resource* resource) noexcept
    : waitSocket(nullptr, SlotSocketDeleter{detail::pmrResourceOrDefault(resource)}) {}

detail::MariaDbPool::ConnectionSlot::~ConnectionSlot() = default;
detail::MariaDbPool::ConnectionSlot::ConnectionSlot(ConnectionSlot&&) noexcept = default;
detail::MariaDbPool::ConnectionSlot& detail::MariaDbPool::ConnectionSlot::operator=(ConnectionSlot&&) noexcept = default;

detail::MariaDbPool::MariaDbPool(asio::io_context& ioContext, DbConfig config, std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      config_(std::move(config)),
      resource_(detail::pmrResourceOrDefault(resource)),
      slots_(resource_),
      scheduler_(config_.poolSize, resource_) {
    detail::validateDbConfig(config_);
    if (config_.driver != DbDriver::kMariaDb) {
        throw std::invalid_argument("MariaDB pool requires the MariaDB driver");
    }
    const auto poolSize = config_.poolSize;
    slots_.reserve(poolSize);
    for (std::size_t i = 0; i < poolSize; ++i) {
        slots_.emplace_back(resource_);
    }
}

detail::MariaDbPool::~MariaDbPool() {
    closeNow();
}

Task<void> detail::MariaDbPool::connect() {
    for (auto& slot : slots_) {
        co_await connectUnlocked(slot);
    }
}

void detail::MariaDbPool::closeNow() noexcept {
    if (!scheduler_.close()) {
        return;
    }
    for (auto& slot : slots_) {
        closeSlot(slot);
    }
}

void detail::MariaDbPool::scanDeadlines(std::chrono::steady_clock::time_point now) noexcept {
    if (config_.acquireTimeout.has_value()) {
        scheduler_.scanDeadlines(now);
    }

    for (auto& slot : slots_) {
        if (!slot.deadlineActive || slot.deadline > now) {
            continue;
        }
        slot.timedOut = true;
        if (slot.deadlineKind == ConnectionSlot::DeadlineKind::kSocket) {
            if (slot.waitSocket != nullptr) {
                slot.waitSocket->cancel();
            }
        } else if (slot.deadlineKind == ConnectionSlot::DeadlineKind::kSleep) {
            auto handle = slot.deadlineContinuation;
            slot.deadlineContinuation = {};
            if (handle) {
                handle.resume();
            }
        }
    }
}

bool detail::MariaDbPool::hasAnyTimeout() const noexcept {
    return config_.connectTimeout.has_value() ||
        config_.queryTimeout.has_value() ||
        config_.readTimeout.has_value() ||
        config_.writeTimeout.has_value() ||
        config_.acquireTimeout.has_value();
}

void detail::MariaDbPool::closeSlot(ConnectionSlot& slot) noexcept {
    if (slot.deadlineKind == ConnectionSlot::DeadlineKind::kSocket && slot.waitSocket != nullptr) {
        slot.waitSocket->cancel();
    } else if (slot.deadlineKind == ConnectionSlot::DeadlineKind::kSleep) {
        auto handle = slot.deadlineContinuation;
        slot.deadlineContinuation = {};
        if (handle) {
            handle.resume();
        }
    }
    clearSlotDeadline(slot);
    // Detach the fd from ASIO before mysql_close() closes it.
    if (slot.waitSocket != nullptr) {
        slot.waitSocket->release();
        slot.waitSocket.reset();
    }
    if (slot.connection != nullptr) {
        mysql_close(slot.connection);
        slot.connection = nullptr;
    }
    slot.connected = false;
}

void detail::MariaDbPool::setSlotDeadline(
    ConnectionSlot& slot,
    std::chrono::milliseconds timeout,
    ConnectionSlot::DeadlineKind kind) noexcept {
    slot.deadlineKind = kind;
    slot.timedOut = false;
    if (timeout.count() <= 0) {
        slot.deadlineActive = false;
        return;
    }
    slot.deadline = std::chrono::steady_clock::now() + timeout;
    slot.deadlineActive = true;
}

void detail::MariaDbPool::clearSlotDeadline(ConnectionSlot& slot) noexcept {
    slot.deadlineActive = false;
    slot.deadlineKind = ConnectionSlot::DeadlineKind::kNone;
    slot.deadlineContinuation = {};
}

}  // namespace ruvia
