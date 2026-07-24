#pragma once

#include <memory>
#include <memory_resource>
#include <span>

#include "ruvia/core/Task.h"

namespace asio {
class io_context;
}

namespace ruvia::detail {

class ConnectionScanner;
class DbRegistry;
class RedisRegistry;
struct DbDefinition;
struct RedisDefinition;

// Common worker-local integration owner used by both HttpServer workers and
// application-created EventLoops. It has no App, Context, Router, or HTTP
// server dependency.
class DataAccessState final {
public:
    DataAccessState(asio::io_context& ioContext, std::pmr::memory_resource* resource, std::span<const DbDefinition> databases, std::span<const RedisDefinition> redis, ConnectionScanner& scanner);
    ~DataAccessState();

    DataAccessState(const DataAccessState&) = delete;
    DataAccessState& operator=(const DataAccessState&) = delete;

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
