#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace ruvia {
enum class RedisIndexKind : std::uint8_t { kNone,
    kTag,
    kText,
    kNumeric };
struct RedisIndexConfig final {
    std::string column{};
    RedisIndexKind kind{RedisIndexKind::kTag};
    bool sortable{false};
};
struct RedisRepositoryConfig final {
    std::string prefix{};
    std::vector<RedisIndexConfig> indexes{};
};
// Omitted expiration preserves an existing TTL; new entities are persistent.
struct RedisWriteOptions final {
    std::optional<std::chrono::milliseconds> ttl{};
    bool persist{false};
};
}  // namespace ruvia
