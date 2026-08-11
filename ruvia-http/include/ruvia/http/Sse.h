#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/BorrowedText.h"

namespace ruvia {

struct SseMessage final {
    // SSE messages may be retained before they are formatted. Keep their text
    // zero-copy while preventing a temporary owning string from leaving an
    // already-dangling view in the saved message.
    // Absence emits no data field, so an event/id/retry-only block does not
    // dispatch a MessageEvent. A present empty value emits `data:` and therefore
    // dispatches one event whose data is empty.
    std::optional<::ruvia::BorrowedText> data;
    ::ruvia::BorrowedText event;
    std::optional<::ruvia::BorrowedText> id;
    std::optional<std::uint32_t> retry;
};

// Replace `frame` with one complete UTF-8 event-stream block. Invalid event/id
// line syntax throws std::invalid_argument; allocation follows frame's resource.
void formatSseMessage(std::pmr::string& frame, const SseMessage& message);

}  // namespace ruvia
