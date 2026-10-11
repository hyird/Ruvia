#pragma once

#include <system_error>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/task.h"
#include "ruvia/http/http_response_file.h"

namespace ruvia::detail {

// Whether plain TCP can hand a file range to the kernel (sendfile on Linux,
// overlapped TransmitFile on Windows). Elsewhere, and on every non-TCP stream,
// file bodies go through response_file_source.
#if defined(__linux__) || defined(_WIN32)
inline constexpr bool native_response_file_write_available = true;

task<std::error_code> write_http_response_file(asio::ip::tcp::socket& socket, http_response_file_view file);
#else
inline constexpr bool native_response_file_write_available = false;
#endif

}  // namespace ruvia::detail
