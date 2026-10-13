#pragma once

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_io_buffer.h"

#include "client/client_tcp_transport.h"

namespace ruvia::detail {

class http_client_pool;
class http_client_response_state;

// One fixed TCP slot of an http_client_pool: its transport and HTTP/1 parse
// buffers. The pool exclusively leases this slot to one exchange at a time.
// Worker-affine and address-stable.
class http_client_connection final {
public:
    explicit http_client_connection(http_client_pool& pool);
    ~http_client_connection();
    http_client_connection(const http_client_connection&) = delete;
    http_client_connection& operator=(const http_client_connection&) = delete;

    // Called while the pool slot is exclusively leased, under the lease's
    // cancellation; reuses a live connection and replaces one the peer
    // retired while it was idle. An aborted lease never starts a connect.
    [[nodiscard]] task<void> ensure_connected(const operation_timeout& timeout);
    // Starts a pool lease before its cancellation is armed: the previous
    // lease's abort is cleared. A retired connection refuses with closing, so
    // a request admitted after shutdown never reconnects.
    void begin_lease();
    // Close the current transport and discard its buffered wire bytes. Storage
    // is kept: a cancelled read or write may still target it.
    void close() noexcept;
    // Applies the retention policy to both buffers. Only between exchanges,
    // when no transport operation borrows their storage.
    void retain_idle_buffers() noexcept;
    // Pool shutdown: permanently refuses new leases, aborts the connect or
    // HTTP/1 exchange in progress with closing, then closes.
    void shutdown() noexcept;
    // Lease cancellation: aborts the connect or HTTP/1 exchange in progress
    // with `reason` and closes the transport.
    void cancel(client_abort_reason reason) noexcept;
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
    bool connected_{false};
    bool retired_{false};

private:
    http_client_pool& pool_;
};

}  // namespace ruvia::detail
