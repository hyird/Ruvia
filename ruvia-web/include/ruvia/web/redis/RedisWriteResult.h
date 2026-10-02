#pragma once

#include <cstdint>

namespace ruvia {

class redis_write_result final {
public:
    explicit constexpr redis_write_result(std::uint64_t affected_entities) noexcept
        : affected_entities_(affected_entities) {}
    constexpr std::uint64_t affected_entities() const noexcept {
        return affected_entities_;
    }

private:
    std::uint64_t affected_entities_{};
};

}  // namespace ruvia
