#pragma once

#include <cstddef>
#include <exception>
#include <optional>
#include <span>

#include <asio/buffer.hpp>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_io_buffer.h"

#include "client/client_tcp_transport.h"

namespace ruvia {
class http1_request_content_writer;
}  // namespace ruvia

namespace ruvia::detail {

class http_client_connection;
class http_client_output_queue;
class http_client_pool;
class http_client_request_storage;
class http_client_response_state;
class tls_tunnel_output;

// One serialized HTTP/1 exchange on a connected http_client_connection: the
// request head and content, a full-duplex streaming upload, the response head,
// body and Content-Encoding, or an accepted CONNECT tunnel. The caller holds
// the connection's pool lease, and its cancellation, for the whole run.
class http1_client_exchange final {
public:
    http1_client_exchange(http_client_pool& pool, http_client_connection& connection) noexcept
        : pool_(pool),
          connection_(connection) {}

    http1_client_exchange(const http1_client_exchange&) = delete;
    http1_client_exchange& operator=(const http1_client_exchange&) = delete;

    [[nodiscard]] task<void> run(const http_client_request_storage& request, const operation_timeout& timeout,
        stop_token stop_token_value, http_client_response_state& state_value);

private:
    // Receives the final response head, then the body or an accepted tunnel.
    [[nodiscard]] task<void> receive(const http_client_request_storage& request, const operation_timeout& timeout,
        http_client_response_state& state_value, http1_client_response_parser& parser);
    // Parses the buffered bytes up to the final response head, retaining every
    // interim head; empty while more input is needed.
    [[nodiscard]] std::optional<http1_parsed_client_response_head> take_final_head(
        http_client_response_state& state_value, http1_client_response_parser& parser);
    // Transfers parsed fields from the same response PMR without copying them,
    // publishes status/body plan and cookies, then consumes the buffered head.
    void publish_final_head(const http_client_request_storage& request, http_client_response_state& state_value,
        http1_parsed_client_response_head& head);
    // Waits until the consumer drains the producer queue below the byte limit.
    [[nodiscard]] task<void> wait_for_body_space(
        http_client_response_state& state_value, const operation_timeout& timeout);
    void expire_tunnel() noexcept;
    [[nodiscard]] task<void> write_tunnel_output(http_client_output_queue& output, const operation_timeout& timeout,
        tls_tunnel_output* tls, std::exception_ptr& failure);
    [[nodiscard]] task<void> run_tunnel(http_client_response_state& state_value, const operation_timeout& timeout);
    [[nodiscard]] task<void> write_upload(http_client_response_state& state_value, const operation_timeout& timeout,
        http1_request_content_writer writer, http1_client_response_parser& parser, std::exception_ptr& failure);
    // One transport write of all `parts` (a head, or a framed upload chunk:
    // prefix, payload, suffix) under the write timeout.
    [[nodiscard]] task<void> write(std::span<const asio::const_buffer> parts, const operation_timeout& timeout);
    // Appends at most `max_bytes` read from the transport to `target`; returns
    // the read, which is not ok() at the peer's end of stream, and throws any
    // other transport failure.
    [[nodiscard]] task<client_transport_result> read_append(
        http_io_buffer& target, std::size_t max_bytes, const operation_timeout& timeout);

    http_client_pool& pool_;
    http_client_connection& connection_;
};

}  // namespace ruvia::detail
