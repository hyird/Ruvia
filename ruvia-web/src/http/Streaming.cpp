#include "ruvia/web/detail/http/StreamingAccess.h"

#include "ruvia/core/Task.h"
#include "ruvia/core/memory/PmrResource.h"

namespace ruvia {

ScopedOperation<void> SseWriter::write(const SseMessage& message) {
    std::pmr::string frame(detail::processResource());
    detail::formatSseMessage(frame, message);
    return writer_.writeOwned(std::move(frame));
}

}  // namespace ruvia
