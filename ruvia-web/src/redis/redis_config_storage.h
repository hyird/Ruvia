#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/redis/redis_types.h"

#include "client/client_tls_config_storage.h"
#include "integration/named_capability.h"
#include "redis/redis_config_validation.h"

namespace ruvia::detail {

// PMR-owned runtime form of the ordinary public redis_config value.
struct redis_config_storage final {
    redis_config_storage(const redis_config& source_value, std::pmr::memory_resource* resource)
        : redis_config_storage(
              validated_config_tag_type{}, validate(source_value), pmr_resource_or_default(resource)) {}

    redis_config_storage(const redis_config_storage& source_value, std::pmr::memory_resource* resource)
        : redis_config_storage(validated_config_tag_type{}, source_value, pmr_resource_or_default(resource)) {}

    std::pmr::string host_;
    std::uint16_t port_{6379};
    std::pmr::string username_;
    std::pmr::string password_;
    client_tls_config_storage tls_;
    std::uint32_t database_{0};
    std::size_t pool_size_per_worker_{4};
    std::size_t blocking_pool_size_per_worker_{1};
    std::optional<std::chrono::milliseconds> connect_timeout_;
    std::optional<std::chrono::milliseconds> command_timeout_;
    std::optional<std::chrono::milliseconds> acquire_timeout_;
    std::optional<std::size_t> max_reply_bytes_{64 * 1024 * 1024};
    std::size_t max_array_depth_{64};
    tcp_no_delay_policy tcp_no_delay_{tcp_no_delay_policy::enable};
    tcp_keep_alive_policy tcp_keep_alive_{tcp_keep_alive_policy::system_default};

private:
    struct validated_config_tag_type final {};

    [[nodiscard]] static const redis_config& validate(const redis_config& source_value) {
        validate_redis_config(source_value);
        return source_value;
    }

    // Public configuration validation and allocator-rebound copies enter the
    // same owning construction path; no intermediate string copies or views.
    template <typename source_type>
        requires(std::same_as<source_type, redis_config> ||
                    std::same_as<source_type, redis_config_storage>)
    redis_config_storage(
        validated_config_tag_type, const source_type& source_value, std::pmr::memory_resource* resource)
        : host_(source_value.host_, resource),
          port_(source_value.port_),
          username_(source_value.username_, resource),
          password_(source_value.password_, resource),
          tls_(source_value.tls_, resource),
          database_(source_value.database_),
          pool_size_per_worker_(source_value.pool_size_per_worker_),
          blocking_pool_size_per_worker_(source_value.blocking_pool_size_per_worker_),
          connect_timeout_(source_value.connect_timeout_),
          command_timeout_(source_value.command_timeout_),
          acquire_timeout_(source_value.acquire_timeout_),
          max_reply_bytes_(source_value.max_reply_bytes_),
          max_array_depth_(source_value.max_array_depth_),
          tcp_no_delay_(source_value.tcp_no_delay_),
          tcp_keep_alive_(source_value.tcp_keep_alive_) {}
};

using redis_definition_type = named_capability_definition<redis_config_storage>;

}  // namespace ruvia::detail
