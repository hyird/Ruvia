#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <asio/awaitable.hpp>

#include "ruvia/edge/detail/proxy/HeaderRules.h"

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

    virtual asio::awaitable<bool> respond(std::uint16_t status, const Headers& headers, std::string_view body, std::string_view cacheResult, std::optional<std::uint64_t> age, bool omitBody, bool keepAlive) = 0;
    virtual asio::awaitable<bool> respondHead(std::uint16_t status, const Headers& headers, std::string_view cacheResult, bool hasBody, std::optional<std::size_t> contentLength, bool keepAlive) = 0;
    virtual asio::awaitable<bool> respondChunk(std::string_view chunk) = 0;
    virtual asio::awaitable<bool> respondEnd() = 0;
    [[nodiscard]] virtual std::size_t bytesWritten() const = 0;
    [[nodiscard]] virtual bool connectionReusable() const noexcept = 0;
};

// A status-only response: no headers, no body, no age. `cacheResult` is the
// label the access log records -- "ERROR" for a failure the edge generated,
// "MISS" for a request the cache could not answer and was forbidden to fetch.
// Whether the connection survives is the caller's decision, not the status's.
[[nodiscard]] inline asio::awaitable<bool> respondStatusOnly(ResponseWriter& writer, std::uint16_t status, std::string_view cacheResult, bool keepAlive) {
    const Headers noHeaders;
    co_return co_await writer.respond(status, noHeaders, {}, cacheResult, std::nullopt, false, keepAlive);
}

}  // namespace ruvia::edge
