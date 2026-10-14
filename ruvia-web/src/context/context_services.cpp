#include "context/context_services.h"

#include "http/streaming_access.h"

namespace ruvia::detail {

bool context_response_output::committed() const noexcept {
    if (buffered() != nullptr) {
        return false;
    }
    if (const auto* stream = response_stream()) {
        return streaming_access::committed(stream->writer());
    }
    return true;
}

}  // namespace ruvia::detail
