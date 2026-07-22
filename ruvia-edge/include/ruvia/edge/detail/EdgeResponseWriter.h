#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <asio/awaitable.hpp>

#include "ruvia/edge/detail/EdgeHeaderRules.h"

namespace ruvia::edge {

// The protocol-independent sink the serve core writes one response into. A
// response is either buffered (respond) or streamed (respondHead, then
// respondChunk any number of times, then respondEnd). Each protocol supplies its
// own adapter; the serve core never touches wire bytes or frames.
class ResponseWriter {
public:
    ResponseWriter() = default;
    virtual ~ResponseWriter() = default;
    ResponseWriter(const ResponseWriter&) = delete;
    ResponseWriter& operator=(const ResponseWriter&) = delete;

    virtual asio::awaitable<bool> respond(
        std::uint16_t status,
        const Headers& headers,
        std::string_view body,
        std::string_view cacheResult,
        std::optional<std::uint64_t> age,
        bool omitBody,
        bool keepAlive) = 0;
    virtual asio::awaitable<bool> respondHead(
        std::uint16_t status,
        const Headers& headers,
        std::string_view cacheResult,
        bool hasBody,
        std::optional<std::size_t> contentLength,
        bool keepAlive) = 0;
    virtual asio::awaitable<bool> respondChunk(std::string_view chunk) = 0;
    virtual asio::awaitable<bool> respondEnd() = 0;
    [[nodiscard]] virtual std::size_t bytesWritten() const = 0;
    [[nodiscard]] virtual bool connectionReusable() const noexcept = 0;
};

}  // namespace ruvia::edge
