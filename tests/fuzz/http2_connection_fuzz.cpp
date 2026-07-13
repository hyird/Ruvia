#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/http2/Http2FrameTypes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

namespace {

using ruvia::detail::Http2Connection;
using ruvia::detail::Http2FeedResult;
using ruvia::detail::Http2Role;

void drain(Http2Connection& connection) {
    while (connection.nextEvent().has_value()) {
    }
    const auto output = connection.pendingOutput();
    if (!output.empty()) {
        (void)connection.consumeOutput(output.size());
    }
    (void)connection.takeDrainedDataStreams();
}

void feedChunk(Http2Connection& connection, std::string_view chunk) {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const auto result = connection.feed(chunk);
        drain(connection);
        if (result != Http2FeedResult::kEventsPending) {
            return;
        }
    }
}

void exercise(
    Http2Role role,
    std::string_view input,
    bool seedHandshake,
    std::size_t chunkSize) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection connection(&resource, role);
    connection.beginConnection();
    drain(connection);

    if (role == Http2Role::kServer) {
        feedChunk(connection, ruvia::detail::kHttp2ClientPreface);
    }
    if (seedHandshake) {
        static constexpr char kEmptySettings[] = {
            0, 0, 0,
            static_cast<char>(ruvia::detail::Http2FrameType::kSettings),
            0,
            0, 0, 0, 0,
        };
        feedChunk(connection, std::string_view(kEmptySettings, sizeof(kEmptySettings)));
    }

    for (std::size_t offset = 0; offset < input.size();) {
        const auto bytes = std::min(chunkSize, input.size() - offset);
        feedChunk(connection, input.substr(offset, bytes));
        offset += bytes;
        if (connection.connectionError().has_value()) {
            break;
        }
    }
    drain(connection);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const auto control = data[0];
    const auto input = std::string_view(
        reinterpret_cast<const char*>(data + 1),
        size - 1);
    const auto chunkSize = input.empty()
        ? std::size_t{1}
        : 1U + static_cast<std::size_t>(control >> 2U) % input.size();

    exercise(
        (control & 0x01U) == 0 ? Http2Role::kServer : Http2Role::kClient,
        input,
        (control & 0x02U) != 0,
        chunkSize);
    return 0;
}
