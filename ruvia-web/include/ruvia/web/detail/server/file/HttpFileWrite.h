#pragma once

#include <memory_resource>
#include <string>
#include <system_error>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/Task.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/response/HttpResponseFileBody.h"
#include "ruvia/web/detail/server/file/HttpFileFallback.h"

namespace ruvia::detail {

// Plain TCP owns the platform-native write and its compile-time fallback here;
// callers never interpret native capability. Other stream types use the same
// operation name and select the portable writer at compile time.
Task<std::error_code> writeHttpResponseFile(asio::ip::tcp::socket& socket, WorkerMemory& memory,
    std::pmr::string* reusableChunk, ResponseFileBody file);

template <typename Stream>
Task<std::error_code> writeHttpResponseFile(
    Stream& stream, WorkerMemory& memory, std::pmr::string* reusableChunk, ResponseFileBody file) {
    return writeFileFallback(stream, memory, reusableChunk, file);
}

}  // namespace ruvia::detail
