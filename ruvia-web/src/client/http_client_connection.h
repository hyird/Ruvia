#pragma once

#include <cstddef>
#include <cstdint>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_io_buffer.h"
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

    // Called while the pool slot is exclusively leased; reuses a live connection
    // and replaces one the peer retired while it was idle. The connect is
    // published as `state_value`'s cancellable operation so an abandoned
    // response stops a connect in progress.
    [[nodiscard]] task<void> ensure_connected(
        const operation_timeout& timeout, stop_token stop_token_value, http_client_response_state& state_value);
    // Starts a connect or HTTP/1 exchange: the previous operation's abort is
    // cleared and a new generation opens. A retired connection refuses with
    // closing, so a request admitted after shutdown never reconnects.
    void begin_operation();
    // Close the current transport and discard its buffered wire bytes. Storage
    // is kept: a cancelled read or write may still target it.
    void close() noexcept;
    // Applies the retention policy to both buffers. Only between exchanges,
    // when no transport operation borrows their storage.
    void retain_idle_buffers() noexcept;
    // Pool shutdown: permanently refuses new connects and HTTP/1 exchanges,
    // then closes.
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
    // Both buffers allocate on first use and never initialize the bytes a
    // read or the request head writer is about to overwrite.
    http_io_buffer read_buffer_;
    http_io_buffer write_buffer_;
    // Borrowed only while one serialized HTTP/1 exchange is active.
    http_client_response_state* http1_response_{nullptr};
    // Bumped around every connect and HTTP/1 exchange so stale cancellations
    // cannot reach a later operation.
    std::uint64_t generation_{0};
    std::uint64_t cancellation_id_{0};
    bool connected_{false};
    bool retired_{false};

private:
    http_client_pool& pool_;
    std::size_t index_;
};

}  // namespace ruvia::detail
