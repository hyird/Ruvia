#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/web/http_client_types.h"

#include "client/client_tcp_transport.h"

namespace ruvia::detail {

class http_client_pool;
class http_client_request_storage;
class http_client_response_state;

// One fixed TCP slot of an http_client_pool: its transport and HTTP/1 parse
// buffers. The pool exclusively leases this slot to one exchange at a time.
// Worker-affine and address-stable.
class http_client_connection final {
public:
    http_client_connection(http_client_pool& pool, std::size_t index);
    ~http_client_connection();
    http_client_connection(const http_client_connection&) = delete;
    http_client_connection& operator=(const http_client_connection&) = delete;

    [[nodiscard]] std::size_t index() const noexcept {
        return index_;
    }

    // Called while the pool slot is exclusively leased; reuses a live connection.
    [[nodiscard]] task<void> ensure_connected(
        const operation_timeout& timeout, stop_token stop_token_value);
    // Close the current transport and discard its buffered wire bytes.
    void close() noexcept;
    // Pool shutdown: refuse new connects and HTTP/1 exchanges, then close.
    void shutdown() noexcept;
    // Cancels the connect or HTTP/1 exchange of `generation`.
    void cancel(std::uint64_t generation, client_abort_reason reason) noexcept;
    // True when the id names an operation of this connection.
    [[nodiscard]] bool cancel_by_id(std::uint64_t cancellation_id) noexcept;
    // Stops the bound HTTP/1 response's output and wakes its producer waits.
    void abort_http1_response() noexcept;
    // Throws the HTTP client error of the transport's abort reason, if any.
    void throw_if_aborted() const;

    client_tcp_transport transport_;
    std::pmr::string read_buffer_;
    std::pmr::string write_buffer_;
    // Borrowed only while one serialized HTTP/1 exchange is active.
    http_client_response_state* http1_response_{nullptr};
    // Bumped around every connect and HTTP/1 exchange so stale cancellations
    // cannot reach a later operation.
    std::uint64_t generation_{0};
    std::uint64_t cancellation_id_{0};
    bool connected_{false};

private:
    http_client_pool& pool_;
    std::size_t index_;
};

}  // namespace ruvia::detail
