#pragma once

#include <memory>
#include <memory_resource>
#include <span>

#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"

namespace asio {
class io_context;
}

namespace ruvia::detail {

class ConnectionScanner;
class DbRegistry;
class RedisRegistry;
struct DbDefinition;
struct RedisDefinition;

// Owns the database and Redis registries attached to one HTTP server worker.
class WorkerDataState final {
public:
    WorkerDataState(asio::io_context& ioContext, const WorkerHandle& worker, std::pmr::memory_resource* resource, std::span<const DbDefinition> databases, std::span<const RedisDefinition> redis, ConnectionScanner& scanner);
    ~WorkerDataState();

    WorkerDataState(const WorkerDataState&) = delete;
    WorkerDataState& operator=(const WorkerDataState&) = delete;

    [[nodiscard]] Task<void> connect();
    void closeNow() noexcept;

    [[nodiscard]] DbRegistry& databases() noexcept;
    [[nodiscard]] const DbRegistry& databases() const noexcept;
    [[nodiscard]] RedisRegistry& redis() noexcept;
    [[nodiscard]] const RedisRegistry& redis() const noexcept;
    [[nodiscard]] bool hasMaintenance() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ruvia::detail
