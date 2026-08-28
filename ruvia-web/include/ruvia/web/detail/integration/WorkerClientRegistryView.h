#pragma once

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

// A copyable view of the client registries owned by one worker. Keeping the
// three pointers together lets request and posted-job plumbing pass and retire
// one complete worker-local client set as a single value.
class WorkerClientRegistryView final {
public:
    constexpr WorkerClientRegistryView() noexcept = default;

    constexpr WorkerClientRegistryView(DbRegistry* databases, RedisRegistry* redis, HttpClientRegistry* httpClients) noexcept
        : databases_(databases),
          redis_(redis),
          httpClients_(httpClients) {}

    [[nodiscard]] constexpr DbRegistry* databases() const noexcept {
        return databases_;
    }

    [[nodiscard]] constexpr RedisRegistry* redis() const noexcept {
        return redis_;
    }

    [[nodiscard]] constexpr HttpClientRegistry* httpClients() const noexcept {
        return httpClients_;
    }

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
    DbRegistry* databases_{nullptr};
    RedisRegistry* redis_{nullptr};
    HttpClientRegistry* httpClients_{nullptr};
};

}  // namespace detail
}  // namespace ruvia
