#pragma once

#include <cstddef>

#include <asio/buffer.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/task.h"

#include "server/connection_scanner.h"
#include "server/http_connection_state.h"

namespace ruvia::detail {

// Reads more of a cleartext connection's first bytes, which so far match the
// prior-knowledge client preface (RFC 9113 §3.4), under the request-header
// deadlines. Returns false when the read ends the connection. Once the preface
// is complete, the HTTP/1 connection hands its pending bytes to the HTTP/2
// session and returns its working memory first.
template <typename stream_type>
task<bool> read_more_cleartext_http2_preface(
    stream_type& stream, connection_scanner::entry& scanner_entry, connection_read_buffer& read_buffer) {
    scanner_entry.read_request_head(connection_scanner::phase::reading_initial);
    const auto space = read_buffer.prepare_read();
    auto read_completion = co_await ruvia::async_asio<std::size_t>([&stream, space](auto handler) mutable {
        stream.async_read_some(asio::buffer(space), std::move(handler));
    });
    if (read_completion.error_code()) {
        co_return false;
    }
    read_buffer.commit_read(read_completion.result());
    scanner_entry.touch();
    co_return true;
}

}  // namespace ruvia::detail
