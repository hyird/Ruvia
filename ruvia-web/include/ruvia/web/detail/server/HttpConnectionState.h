#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>

#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/http/detail/server/HttpResponseHeadBuffer.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace ruvia::detail {

class Http1RequestBufferCompletion;

// Initial bump block for the per-request arena, carried inside a work set. The
// request monotonic_buffer_resource bump-allocates from here before spilling to
// the worker resource, so a typical small request touches no heap at all. Sized
// to the shared kRequestArenaInitialBytes (see MemoryPool.h) so the HTTP/1 and
// HTTP/2 request arenas start from one identical block size.
inline constexpr std::size_t kWorkSetArenaBytes = kRequestArenaInitialBytes;

// All of a connection's heavy per-request working memory bundled into one
// poolable unit: the read buffer, the request arena block, the (reused) parse
// result, the response-head buffer, and the file chunk buffer. A connection
// borrows a work set only while it is actively serving requests and returns it
// the moment it goes idle, so idle keep-alive connections hold none of it and
// memory scales with in-flight request concurrency rather than connection
// count. The connection's small resident identity (socket, scanner entry,
// keep-alive counters) stays in the session coroutine frame.
struct ConnectionWorkSet final {
    explicit ConnectionWorkSet(WorkerMemory& memory);

    std::pmr::string readBuffer;
    ResponseHeadBuffer responseHead;
    std::pmr::string fileChunk;
    Http1ServerRequestParser parser;
    Http1ServerRequestParseState parsed;
    RouteResolution routeResolution;
    alignas(std::max_align_t) std::byte arenaBlock[kWorkSetArenaBytes];
    ConnectionWorkSet* poolNext{nullptr};

    // Return the work set to a clean borrowable state: trim grown read storage
    // back to the initial size and clear per-request scratch.
    void resetForReuse();
};

// Per-worker intrusive free list of work sets. Worker-private and only touched
// from the worker thread (single-threaded cooperative coroutines), so it needs
// no synchronization. Borrow = pop (warm, no allocation); return = reset + push,
// or free to the upstream resource once the cap is reached.
class ConnectionWorkSetPool final {
public:
    explicit ConnectionWorkSetPool(WorkerMemory& memory) noexcept;
    ~ConnectionWorkSetPool();

    ConnectionWorkSetPool(const ConnectionWorkSetPool&) = delete;
    ConnectionWorkSetPool& operator=(const ConnectionWorkSetPool&) = delete;

    [[nodiscard]] ConnectionWorkSet* acquire();
    void release(ConnectionWorkSet* workSet) noexcept;

private:
    WorkerMemory* memory_;
    ConnectionWorkSet* freeHead_{nullptr};
    std::size_t freeCount_{0};
};

void compactConnectionReadBuffer(
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    std::size_t consumedBytes) noexcept;
void applyReusableHttp1RequestBufferCompletion(
    const Http1RequestBufferCompletion& completion,
    std::pmr::string& readBuffer,
    std::size_t& usedBytes) noexcept;
void trimReadBufferStorage(std::pmr::string& readBuffer, std::size_t usedBytes);
void growReadBuffer(std::pmr::string& readBuffer, std::size_t usedBytes);

}  // namespace ruvia::detail
