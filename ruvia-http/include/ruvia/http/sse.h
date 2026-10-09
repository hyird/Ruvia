#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/borrowed_text.h"

namespace ruvia {

struct sse_message final {
    // SSE messages may be retained before they are formatted. Keep their text
    // zero-copy while preventing a temporary owning string from leaving an
    // already-dangling view in the saved message.
    // Absence emits no data field, so an event/id/retry-only block does not
    // dispatch a MessageEvent. A present empty value emits `data:` and therefore
    // dispatches one event whose data is empty.
    std::optional<::ruvia::borrowed_text> data_{};
    ::ruvia::borrowed_text event_{};
    std::optional<::ruvia::borrowed_text> id_{};
    std::optional<std::chrono::milliseconds> retry_{};
};

struct sse_format_options final {
    std::pmr::memory_resource* resource_{nullptr};
};

// Build one complete UTF-8 event-stream block. Invalid event/id line syntax
// throws std::invalid_argument.
[[nodiscard]] std::pmr::string format_sse_message(
    const sse_message& message, sse_format_options options = {});

}  // namespace ruvia
