#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <vector>

#include "ConnectionScanner.h"
#include "HttpResponseWriter.h"
#include "ruvia/http/HttpParser.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

struct ConnectionState final {
    explicit ConnectionState(WorkerMemory& memory);

    ConnectionScanner::Entry scannerEntry;
    std::pmr::string readBuffer;
    ResponseHeadBuffer responseHead;
    std::pmr::vector<char> fileChunk;
    std::size_t usedBytes{0};
    std::size_t requestCount{0};
    HttpParser parser;
};

void compactConnectionReadBuffer(
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    std::size_t consumedBytes) noexcept;
void trimReadBufferStorage(std::pmr::string& readBuffer, std::size_t usedBytes);
void growReadBuffer(std::pmr::string& readBuffer, std::size_t usedBytes, const HttpParseResult& parsed);

}  // namespace ruvia::detail
