#pragma once

#include <system_error>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/task.h"
#include "ruvia/http/http_response_file.h"

#include "server/connection_scanner.h"

namespace ruvia::detail {

// Whether plain TCP can hand a file range to the kernel (sendfile on Linux,
// overlapped TransmitFile on Windows). Other platforms, and TLS connections,
// read file bodies through response_file_source.
#if defined(__linux__) || defined(_WIN32)
inline constexpr bool native_response_file_write_available = true;

// Touches `progress` after every transfer the kernel reports.
task<std::error_code> write_http_response_file(asio::ip::tcp::socket& socket, http_response_file_view file,
    connection_scanner::entry_type& progress);
#else
inline constexpr bool native_response_file_write_available = false;
#endif

}  // namespace ruvia::detail
