#include "ruvia/web/detail/db/DbInternal.h"
#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbSlotSocket.h"
#include "ruvia/web/detail/db/DbUtils.h"

#include <mysql/mysql.h>

#include <exception>
#include <memory_resource>
#include <utility>

namespace ruvia {

detail::MariaDbPool::ConnectionSlot::ConnectionSlot(
    asio::io_context& ioContext,
    std::pmr::memory_resource* resource)
    : resolver(ioContext),
      waitSocket(nullptr, SlotSocketDeleter{detail::pmrResourceOrDefault(resource)}) {}

detail::MariaDbPool::ConnectionSlot::~ConnectionSlot() {
    if (waitActive) {
        std::terminate();
    }
}
detail::MariaDbPool::ConnectionSlot::ConnectionSlot(ConnectionSlot&&) noexcept = default;
detail::MariaDbPool::ConnectionSlot& detail::MariaDbPool::ConnectionSlot::operator=(ConnectionSlot&&) noexcept = default;

detail::MariaDbPool::MariaDbPool(asio::io_context& ioContext, DbConfig config, std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      config_(std::move(config)),
      resource_(detail::pmrResourceOrDefault(resource)),
      slots_(resource_),
      scheduler_(1, resource_) {
    detail::validateDbConfig(config_);
    if (config_.driver != DbDriver::kMariaDb) {
        throw std::invalid_argument("MariaDB pool requires the MariaDB driver");
    }
    slots_.reserve(1);
    slots_.emplace_back(ioContext_, resource_);
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
    (void)scheduler_.close();
    for (auto& slot : slots_) {
        closeSlot(slot);
    }
}

void detail::MariaDbPool::scanDeadlines(std::chrono::steady_clock::time_point now) noexcept {
    if (config_.acquireTimeout.has_value()) {
        scheduler_.scanDeadlines(now);
    }

    for (auto& slot : slots_) {
        const auto kind = slot.deadline.expire(now);
        if (!kind.has_value()) {
            continue;
        }
        if (*kind == ConnectionSlot::DeadlineKind::kResolve) {
            slot.resolver.cancel();
        } else if (*kind == ConnectionSlot::DeadlineKind::kSocket) {
            if (slot.waitSocket != nullptr) {
                slot.waitSocket->cancel();
            }
        } else if (*kind == ConnectionSlot::DeadlineKind::kSleep) {
            auto handle = std::exchange(slot.deadlineContinuation, {});
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
    slot.closeRequested = true;
    slot.resolver.cancel();
    if (slot.waitActive) {
        const auto* activeKind = slot.deadline.kind();
        if (activeKind != nullptr &&
            *activeKind == ConnectionSlot::DeadlineKind::kSleep) {
            auto handle = std::exchange(slot.deadlineContinuation, {});
            if (handle) {
                handle.resume();
            }
        } else if (slot.waitSocket != nullptr) {
            // Keep the wrapper and native driver connection alive until every
            // queued wait completion has run. The resumed coroutine observes
            // closeRequested before calling a MariaDB *_cont function.
            if (!slot.waitSocket->release()) {
                std::terminate();
            }
        }
        return;
    }

    const auto* kind = slot.deadline.kind();
    if (kind != nullptr &&
        *kind == ConnectionSlot::DeadlineKind::kSocket &&
        slot.waitSocket != nullptr) {
        slot.waitSocket->cancel();
    } else if (kind != nullptr &&
               *kind == ConnectionSlot::DeadlineKind::kSleep) {
        auto handle = std::exchange(slot.deadlineContinuation, {});
        if (handle) {
            handle.resume();
        }
    }
    clearSlotDeadline(slot);
    // Detach the fd from ASIO before mysql_close() closes it.
    if (slot.waitSocket != nullptr) {
        if (!slot.waitSocket->release()) {
            std::terminate();
        }
        slot.waitSocket.reset();
    }
    if (slot.connection != nullptr) {
        mysql_close(slot.connection);
        slot.connection = nullptr;
    }
    slot.connected = false;
    slot.closeRequested = false;
}

void detail::MariaDbPool::setSlotDeadline(
    ConnectionSlot& slot,
    std::chrono::milliseconds timeout,
    ConnectionSlot::DeadlineKind kind) noexcept {
    if (timeout.count() <= 0) {
        slot.deadline.reset();
        return;
    }
    slot.deadline.arm(
        detail::workerTimerDeadlineAfter(timeout),
        kind);
}

void detail::MariaDbPool::clearSlotDeadline(ConnectionSlot& slot) noexcept {
    (void)slot.deadline.clear();
    slot.deadlineContinuation = {};
}

}  // namespace ruvia
