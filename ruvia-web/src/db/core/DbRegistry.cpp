#include "../DbInternal.h"
#include "DbUtils.h"
#include "ruvia/http/Context.h"

#include <chrono>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <stdexcept>
#include <string_view>

namespace ruvia {

detail::DbRegistry::DbRegistry(
    asio::io_context& ioContext,
    std::pmr::memory_resource* resource,
    std::span<const detail::DbDefinition> databases)
    : resource_(detail::pmrResourceOrDefault(resource)),
      clients_(resource_) {
    clients_.reserve(databases.size());
    for (const auto& definition : databases) {
        if (definition.alias.empty()) {
            throw std::invalid_argument("database alias must not be empty");
        }
        if (std::ranges::any_of(
                clients_,
                [&definition](const Entry& entry) {
                    return std::string_view(entry.alias.data(), entry.alias.size()) ==
                        std::string_view(definition.alias);
                })) {
            throw std::invalid_argument("duplicate database alias");
        }

        auto client = detail::makeHttpPmrObject<MariaDbPool>(resource_, ioContext, definition.config, resource_);
        clients_.push_back(Entry{
            std::pmr::string(definition.alias, resource_),
            std::move(client)});
        if (std::string_view(clients_.back().alias.data(), clients_.back().alias.size()) == kDefaultDbAlias) {
            defaultClient_ = clients_.back().client.get();
        }
    }
}

detail::DbRegistry::~DbRegistry() = default;

Task<void> detail::DbRegistry::connect() {
    for (auto& entry : clients_) {
        co_await entry.client->connect();
    }
    co_return;
}

void detail::DbRegistry::closeNow() noexcept {
    for (auto& entry : clients_) {
        entry.client->closeNow();
    }
}

bool detail::DbRegistry::empty() const noexcept {
    return clients_.empty();
}

void detail::DbRegistry::scanDeadlines() noexcept {
    const auto now = std::chrono::steady_clock::now();
    for (auto& entry : clients_) {
        entry.client->scanDeadlines(now);
    }
}

bool detail::DbRegistry::hasAnyTimeout() const noexcept {
    return std::ranges::any_of(clients_, [](const Entry& entry) {
        return entry.client->hasAnyTimeout();
    });
}

DbHandle detail::DbRegistry::get(std::pmr::memory_resource* resource, RequestMemory* requestMemory) const {
    if (defaultClient_ == nullptr) {
        throw std::logic_error("default database is not configured");
    }
    return DbHandle(*defaultClient_, resource, requestMemory);
}

DbHandle detail::DbRegistry::get(
    std::string_view alias,
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) const {
    for (const auto& entry : clients_) {
        if (std::string_view(entry.alias.data(), entry.alias.size()) == alias) {
            return DbHandle(*entry.client, resource, requestMemory);
        }
    }

    throw std::logic_error("database is not configured");
}

DbHandle Context::db() const {
    if (db_ == nullptr) {
        throw std::logic_error("database is not configured");
    }
    return db_->get(resource(), const_cast<RequestMemory*>(&memory_));
}

DbHandle Context::db(std::string_view alias) const {
    if (db_ == nullptr) {
        throw std::logic_error("database is not configured");
    }
    return db_->get(alias, resource(), const_cast<RequestMemory*>(&memory_));
}

}  // namespace ruvia
