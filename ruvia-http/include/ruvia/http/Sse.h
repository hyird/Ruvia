#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace ruvia {

struct SseMessage final {
    std::string_view data;
    std::string_view event;
    std::optional<std::string_view> id;
    std::optional<std::uint32_t> retry;
};

namespace detail {
void formatSseMessage(std::pmr::string& frame, const SseMessage& message);
}  // namespace detail

}  // namespace ruvia
