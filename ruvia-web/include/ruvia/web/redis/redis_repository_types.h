#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace ruvia {
enum class redis_index_kind : std::uint8_t { none,
    tag,
    text,
    numeric };
struct redis_index_config final {
    std::string field_{};
    redis_index_kind kind_{redis_index_kind::tag};
    bool sortable_{false};
};
struct redis_repository_config final {
    std::string prefix_{};
    std::vector<redis_index_config> indexes_{};
};
// Omitted expiration preserves an existing TTL; new entities are persistent.
struct redis_write_options final {
    std::optional<std::chrono::milliseconds> ttl_{};
    bool persist_{false};
};
}  // namespace ruvia
