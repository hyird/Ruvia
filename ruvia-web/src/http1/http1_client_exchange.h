#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"

namespace ruvia {
class http1_client_response_parser;
class http1_request_content_writer;
}  // namespace ruvia

namespace ruvia::detail {

class http_client_connection;
class http_client_pool;
class http_client_request_storage;
class http_client_response_state;

// One serialized HTTP/1 exchange on a connected http_client_connection: the
// request head and content, a full-duplex streaming upload, the response head,
// body and Content-Encoding, or an accepted CONNECT tunnel. The caller holds
// the connection's HTTP/1 exchange slot for the whole run.
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
    [[nodiscard]] task<void> exchange(const http_client_request_storage& request, const operation_timeout& timeout,
        http_client_response_state& state_value);
    [[nodiscard]] task<void> receive(const http_client_request_storage& request, const operation_timeout& timeout,
        http_client_response_state& state_value, http1_client_response_parser& parser);
    [[nodiscard]] task<void> run_tunnel(http_client_response_state& state_value, const operation_timeout& timeout);
    [[nodiscard]] task<void> write_upload(http_client_response_state& state_value, const operation_timeout& timeout,
        http1_request_content_writer writer, http1_client_response_parser& parser, std::exception_ptr& failure);
    [[nodiscard]] task<void> write(std::string_view bytes, const operation_timeout& timeout);
    // Appends at most `max_bytes` read from the transport to `target`; returns
    // 0 at the peer's end of stream.
    [[nodiscard]] task<std::size_t> read_append(
        std::pmr::string& target, std::size_t max_bytes, const operation_timeout& timeout);

    http_client_pool& pool_;
    http_client_connection& connection_;
};

}  // namespace ruvia::detail
