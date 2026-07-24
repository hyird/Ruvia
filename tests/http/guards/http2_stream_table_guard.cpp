#include "ruvia/http/detail/http2/stream/Http2StreamTable.h"

#include <cstdint>
#include <memory_resource>

int main() {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::detail::Http2StreamTable streams(&resource);

    for (std::uint32_t streamId = 1; streamId <= 39; streamId += 2) {
        if (streams.create(streamId, 65535) == nullptr) {
            return 1;
        }
    }

    auto* abortedStream = streams.find(35);
    if (abortedStream == nullptr) {
        return 2;
    }

    bool removedDuringIteration = false;
    std::uint32_t visited = 0;
    streams.forEach([&](ruvia::detail::Http2StreamState& stream) noexcept {
        ++visited;
        if (!removedDuringIteration && stream.id() == 33) {
            removedDuringIteration = true;
            (void)abortedStream->abort(ruvia::detail::Http2StreamCloseSource::kLocal);
            streams.removeAborted([](const ruvia::detail::Http2StreamState&) noexcept {});
        }
    });

    if (!removedDuringIteration) {
        return 3;
    }
    if (visited != 19) {
        return 4;
    }
    if (streams.find(35) != nullptr) {
        return 5;
    }
    if (streams.size() != 19) {
        return 6;
    }

    return 0;
}
