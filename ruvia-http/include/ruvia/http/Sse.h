#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace ruvia {

struct SseMessage final {
    // Absence emits no data field, so an event/id/retry-only block does not
    // dispatch a MessageEvent. A present empty value emits `data:` and therefore
    // dispatches one event whose data is empty.
    std::optional<std::string_view> data;
    std::string_view event;
    std::optional<std::string_view> id;
    std::optional<std::uint32_t> retry;
};

namespace detail {
void formatSseMessage(std::pmr::string& frame, const SseMessage& message);
}  // namespace detail

}  // namespace ruvia
