#pragma once

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/db/db_cache.h"
#include "ruvia/web/db/db_types.h"

#include "client/client_tls_config_storage.h"
#include "db/db_config_validation.h"
#include "integration/named_capability.h"

namespace ruvia::detail {

struct db_cache_config_storage final {
    template <typename config_type>
    db_cache_config_storage(const config_type& source_value, std::pmr::memory_resource* resource)
        : duration_(source_value.duration_),
          always_enabled_(source_value.always_enabled_),
          ignore_errors_(source_value.ignore_errors_),
          name_space_(source_value.name_space_, resource) {
#ifndef RUVIA_ENABLE_REDIS
        throw std::invalid_argument("database query caching requires Redis support");
#endif
        if (duration_.count() <= 0 || name_space_.empty()) {
            throw std::invalid_argument("database cache requires a positive duration and nonempty namespace");
        }
    }
    std::chrono::milliseconds duration_;
    bool always_enabled_;
    bool ignore_errors_;
    std::pmr::string name_space_;
};

// Worker/app-owned copy of the public startup configuration. Public db_config
// deliberately uses ordinary value types; retained runtime state is rebound to
// its owning PMR domain here.
struct db_config_storage final {
    db_config_storage(const db_config& source_value, std::pmr::memory_resource* resource)
        : db_config_storage(validated_db_config(source_value), pmr_resource_or_default(resource)) {}

    db_config_storage(validated_db_config_view source_value, std::pmr::memory_resource* resource)
        : db_config_storage(validated_config_tag_type{}, source_value.get(), pmr_resource_or_default(resource)) {}

    db_config_storage(const db_config_storage& source_value, std::pmr::memory_resource* resource)
        : db_config_storage(validated_config_tag_type{}, source_value, pmr_resource_or_default(resource)) {}

    db_driver driver_{db_driver::unspecified};
    std::pmr::string host_;
    std::uint16_t port_{0};
    std::pmr::string username_;
    std::pmr::string password_;
    client_tls_config_storage tls_;
    std::pmr::string database_;
    std::optional<std::chrono::milliseconds> connect_timeout_;
    std::optional<std::chrono::milliseconds> read_timeout_;
    std::optional<std::chrono::milliseconds> write_timeout_;
    std::optional<std::chrono::milliseconds> query_timeout_;
    std::optional<std::chrono::milliseconds> acquire_timeout_;

private:
    struct validated_config_tag_type final {};

    db_config_storage(validated_config_tag_type, const db_config& source_value, std::pmr::memory_resource* resource)
        : driver_(source_value.driver_),
          host_(source_value.host_, resource),
          port_(source_value.port_.value_or(default_db_port(source_value.driver_))),
          username_(source_value.username_, resource),
          password_(source_value.password_, resource),
          tls_(source_value.tls_, resource),
          database_(source_value.database_, resource),
          connect_timeout_(source_value.connect_timeout_),
          read_timeout_(source_value.read_timeout_),
          write_timeout_(source_value.write_timeout_),
          query_timeout_(source_value.query_timeout_),
          acquire_timeout_(source_value.acquire_timeout_) {
    }

    db_config_storage(
        validated_config_tag_type, const db_config_storage& source_value, std::pmr::memory_resource* resource)
        : driver_(source_value.driver_),
          host_(source_value.host_, resource),
          port_(source_value.port_),
          username_(source_value.username_, resource),
          password_(source_value.password_, resource),
          tls_(source_value.tls_, resource),
          database_(source_value.database_, resource),
          connect_timeout_(source_value.connect_timeout_),
          read_timeout_(source_value.read_timeout_),
          write_timeout_(source_value.write_timeout_),
          query_timeout_(source_value.query_timeout_),
          acquire_timeout_(source_value.acquire_timeout_) {
    }
};

struct db_query_cache_registration_storage final {
    db_query_cache_registration_storage(const db_query_cache_registration& source_value,
        std::pmr::memory_resource* resource)
        : redis_alias_(source_value.redis_alias_, resource),
          policy_(source_value.policy_, resource) {
        validate_capability_alias(redis_alias_, "database query cache Redis alias must not be empty");
    }
    std::pmr::string redis_alias_;
    db_cache_config_storage policy_;
};

struct db_definition final {
    using config_storage_type = db_config_storage;
    std::pmr::string alias_;
    db_config_storage config_;
    std::optional<db_query_cache_registration_storage> query_cache_{};
};

}  // namespace ruvia::detail
