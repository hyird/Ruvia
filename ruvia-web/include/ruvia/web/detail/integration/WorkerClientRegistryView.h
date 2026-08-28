#pragma once

#include <cstddef>
#include <memory_resource>
#include <string_view>

namespace ruvia {

class HttpClientHandle;
class StopToken;

#ifdef RUVIA_ENABLE_DATABASE
class DbHandle;
#endif

#ifdef RUVIA_ENABLE_REDIS
class RedisHandle;
#endif

namespace detail {

class DbRegistry;
class HttpClientRegistry;
class RedisRegistry;
class ScopedOperationScope;

// A copyable view of the complete client registry set owned by one worker.
// The only states are fully attached and explicitly detached: partial registry
// graphs are not representable. Dispatch retirement detaches the view after all
// posted work has joined, before the worker-owned registries are destroyed.
class WorkerClientRegistryView final {
public:
    [[nodiscard]] static constexpr WorkerClientRegistryView detached() noexcept {
        return WorkerClientRegistryView(nullptr);
    }

    constexpr WorkerClientRegistryView(DbRegistry& databases, RedisRegistry& redis, HttpClientRegistry& httpClients) noexcept
        : databases_(&databases),
          redis_(&redis),
          httpClients_(&httpClients) {}

    [[nodiscard]] constexpr bool attached() const noexcept {
        return httpClients_ != nullptr;
    }

    friend constexpr bool operator==(const WorkerClientRegistryView&, const WorkerClientRegistryView&) noexcept = default;

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] DbHandle db(std::pmr::memory_resource* resource, ScopedOperationScope& operationScope, const StopToken& stopToken) const;
    [[nodiscard]] DbHandle db(std::string_view alias, std::pmr::memory_resource* resource, ScopedOperationScope& operationScope, const StopToken& stopToken) const;
#endif

#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] RedisHandle redis(std::pmr::memory_resource* resource, ScopedOperationScope& operationScope, const StopToken& stopToken) const;
    [[nodiscard]] RedisHandle redis(std::string_view alias, std::pmr::memory_resource* resource, ScopedOperationScope& operationScope, const StopToken& stopToken) const;
#endif

    [[nodiscard]] HttpClientHandle httpClient(std::pmr::memory_resource* resource, ScopedOperationScope& operationScope, const StopToken& stopToken) const;
    [[nodiscard]] HttpClientHandle httpClient(std::string_view alias, std::pmr::memory_resource* resource, ScopedOperationScope& operationScope, const StopToken& stopToken) const;

private:
    explicit constexpr WorkerClientRegistryView(std::nullptr_t) noexcept {}

    DbRegistry* databases_{nullptr};
    RedisRegistry* redis_{nullptr};
    HttpClientRegistry* httpClients_{nullptr};
};

}  // namespace detail
}  // namespace ruvia
