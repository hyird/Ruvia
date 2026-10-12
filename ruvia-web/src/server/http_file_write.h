#pragma once

#include <cstdint>
#include <string_view>
#include <system_error>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/task.h"
#include "ruvia/http/http_response_file.h"
#include "ruvia/http/http_response_head_buffer.h"

#include "server/connection_scanner.h"

namespace ruvia::detail {

// Whether plain TCP can hand a file range to the kernel (sendfile on Linux,
// overlapped TransmitFile on Windows). Other platforms, and TLS connections,
// read file bodies through response_file_source.
#if defined(__linux__) || defined(_WIN32)
inline constexpr bool native_response_file_write_available = true;

// A whole-body file range up to this size is read into the head buffer behind
// the serialized head and leaves in one buffered send: for small files one
// positional read costs less than a kernel file transfer and its completion,
// and it never queues behind TransmitFile's per-SKU concurrency limit.
inline constexpr std::uint64_t inline_response_file_max_bytes = std::uint64_t{16} * 1024;

struct http_response_file_write_result final {
    std::error_code error_;
    // Whether every byte of the head reached the socket before error_.
    bool head_written_{false};
};

// Appends the bytes of `file` (at most inline_response_file_max_bytes) to
// `head`, reading through the response's opened handle or, without one, a
// handle opened and identity-validated here. A failure leaves nothing sent.
[[nodiscard]] std::error_code append_response_file_bytes(
    http_response_head_buffer& head, http_response_file_view file);

// Sends `head`, then the range of `file` from the kernel. The file is opened
// (or its opened handle borrowed) before any byte is sent, so a missing or
// changed file fails with no head written. Windows carries the head in the
// first TransmitFile; Linux corks it with MSG_MORE so it shares the first
// segment with the file bytes. Touches `progress` after every transfer.
task<http_response_file_write_result> write_http_response_file(asio::ip::tcp::socket& socket,
    std::string_view head, http_response_file_view file, connection_scanner::entry_type& progress);
#else
inline constexpr bool native_response_file_write_available = false;
#endif

}  // namespace ruvia::detail
