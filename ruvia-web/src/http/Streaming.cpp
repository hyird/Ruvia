#include "ruvia/web/detail/http/StreamingInternal.h"

#include "ruvia/core/Task.h"

namespace ruvia {

Task<void> SseWriter::writeSSE(const SseMessage& message) {
    auto& frame = detail::StreamingAccess::scratch(writer_);
    detail::formatSseMessage(frame, message);
    co_await writer_.write(frame);
}

}  // namespace ruvia
