#pragma once

#include <cstdint>
#include <string_view>
#include <system_error>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/task.h"
#include "ruvia/http/http_response_file.h"

#include "server/connection_scanner.h"
#include "server/http_native_file.h"

namespace ruvia::detail {

#if defined(_WIN32)
// On Windows plain TCP a single file range longer than this leaves through an
// overlapped TransmitFile from the response's own handle. Shorter ranges,
// multipart bodies, TLS and every other platform read through
// response_file_source, which sends the head and first chunk together.
inline constexpr std::uint64_t transmit_file_min_bytes = std::uint64_t{16} * 1024;

struct http_response_file_write_result final {
    std::error_code error_;
    // Whether every byte of the head reached the socket before error_.
    bool head_written_{false};
};

// Sends `head`, then the range of `file` from `input`, a handle the caller
// already validated against the file's snapshot. The first TransmitFile
// carries the head. Each later slice starts only while `input` still matches
// the snapshot, and the final byte waits for that check, so a file that
// changes while it is sent never completes its framed length. Touches
// `progress` after every transfer.
task<http_response_file_write_result> write_http_response_file(asio::ip::tcp::socket& socket,
    std::string_view head, native_file_type input, http_response_file_view file, connection_scanner::entry& progress);
#endif

}  // namespace ruvia::detail
