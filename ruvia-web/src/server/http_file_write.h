#pragma once

#include <memory_resource>
#include <string>
#include <system_error>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_response_file.h"

#include "server/http_file_fallback.h"

namespace ruvia::detail {

// Plain TCP owns the platform-native write and its compile-time fallback here;
// callers never interpret native capability. Other stream types use the same
// operation name and select the portable writer at compile time.
task<std::error_code> write_http_response_file(asio::ip::tcp::socket& socket, worker_memory& memory,
    std::pmr::string* reusable_chunk, http_response_file_view file);

template <typename stream_type>
task<std::error_code> write_http_response_file(
    stream_type& stream, worker_memory& memory, std::pmr::string* reusable_chunk, http_response_file_view file) {
    return write_file_fallback(stream, memory, reusable_chunk, file);
}

}  // namespace ruvia::detail
