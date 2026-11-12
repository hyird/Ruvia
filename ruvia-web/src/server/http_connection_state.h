#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/memory/request_memory.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response_head_buffer.h"
#include "ruvia/http/http_status.h"

#include "integration/request_io_metrics.h"
#include "router/route_resolution.h"
#include "server/connection_count.h"
#include "server/connection_lifecycle.h"

namespace ruvia::detail {

class http1_request_buffer_completion;
struct access_log_sink;

// Initial read capacity for an active connection. Growth is bounded by pending
// input, and destruction returns the buffer to the worker resource on idle.
inline constexpr std::size_t initial_read_buffer_bytes = std::size_t{4} * 1024;

// Bytes of the connection-resident buffer an idle connection reads into while
// it holds no work set. Sized so a typical request line arrives in one read;
// longer heads finish in the regular read loop after the work set is
// re-acquired. It lives in every connection's frame, so growing it to cover
// whole browser heads (400-800 bytes with cookies) costs that much per idle
// connection to save one immediately-completing read per such request.
inline constexpr std::size_t idle_resident_read_bytes = 256;

// A connection's buffered request input with its growth and trim policy.
// Consumption advances the window; reads use the writable tail first and
// compact only when the consumed prefix covers the remaining bytes.
class connection_read_buffer final {
public:
    explicit connection_read_buffer(std::pmr::polymorphic_allocator<char> allocator);

    // Unconsumed bytes, starting with the current request.
    [[nodiscard]] std::string_view pending() const& noexcept RUVIA_LIFETIMEBOUND {
        return storage_.view();
    }
    std::string_view pending() const&& = delete;
    [[nodiscard]] bool empty() const noexcept {
        return storage_.empty();
    }

    // Room for the next socket read, bounded by max_http_header_bytes minus
    // pending bytes. Uses spare storage first, growing only when none remains.
    // Empty when pending bytes already fill the header limit.
    [[nodiscard]] std::span<char> prepare_read() & RUVIA_LIFETIMEBOUND;
    std::span<char> prepare_read() && = delete;
    // Appends `bytes` read into the span prepare_read() returned.
    void commit_read(std::size_t bytes) noexcept {
        storage_.commit(bytes);
    }

    // Drops the first `bytes` pending bytes (a served request).
    void consume(std::size_t bytes) noexcept;
    // Replaces the pending bytes with a copy of `bytes`, which never aliases
    // this buffer (an idle landing pad or a body runtime's over-read stash).
    void assign(std::string_view bytes);
    // Applies a reusable request's buffer completion. Installing a handed-over
    // pipeline can grow the storage, so this is the one completion step that
    // may allocate.
    void complete_request(const http1_request_buffer_completion& completion);

    // Gives storage that a handed-over pipeline grew past max_http_header_bytes
    // back once the pending bytes fit the initial size.
    void trim();

private:
    http_io_buffer storage_;
};

// Access-log fields of pipelined responses held back for a coalesced write.
// The request storage they come from is reused before that write, so the
// logged fields are copied; nothing is kept without an access log.
class http1_deferred_access_log final {
public:
    explicit http1_deferred_access_log(std::pmr::polymorphic_allocator<char> allocator);

    // Keeps the record of a response whose head completes at output offset
    // `head_end`. Duration is measured when the record is reported.
    void defer(std::size_t head_end, const http_request& request, std::string_view remote_address,
        std::string_view client_address, std::chrono::steady_clock::time_point start,
        http_status_code status, const request_io_metrics& io_metrics = {});
    // Reports every kept response whose head lies within the first `written`
    // output bytes (in output order), then forgets all of them.
    void complete(const access_log_sink& access_log, std::string_view remote_address, std::size_t written) noexcept;
    // Forgets every kept response and releases grown storage.
    void reset() noexcept;

private:
    struct entry_type final {
        std::size_t head_end_;
        std::size_t text_begin_;
        std::size_t method_bytes_;
        std::size_t path_bytes_;
        std::size_t client_address_bytes_;
        std::chrono::steady_clock::time_point start_;
        request_io_metrics io_metrics_{};
        http_known_method known_method_;
        http_protocol_version protocol_version_;
        http_status_code status_;
    };

    std::pmr::vector<entry_type> entries_;
    std::pmr::string text_;
};

// Heavy request storage exists only while the connection is serving requests.
// A pipelined burst reuses it; idle connections retain only their session frame.
// The work set and its configured initial arena block share one allocation.
struct connection_work_set final {
    explicit connection_work_set(std::span<std::byte> arena_block);

    connection_read_buffer read_buffer_;
    // Serialized response output not yet written: the current response's head
    // (and coalesced small body), preceded by any pipelined responses held
    // back for one write. Empty between writes.
    http_response_head_buffer response_head_;
    http1_deferred_access_log deferred_access_;
    http1_server_request_parse_state parsed_;
    route_resolution route_resolution_;
    // Initial block of the per-request arena (see above).
    std::span<std::byte> arena_block_;
};

struct connection_work_set_deleter final {
    void operator()(connection_work_set* work_set) const noexcept;
};

[[nodiscard]] std::unique_ptr<connection_work_set, connection_work_set_deleter>
make_connection_work_set(std::size_t arena_bytes);

// Owns one admitted socket and its connection_slot as one linear value. The
// slot is acquired only after socket configuration succeeds, then moves into
// the lazy session task before task_scope::spawn. If coroutine allocation or
// spawn throws, destroying that cold ownership chain closes the socket first
// and releases the count second.
class accepted_connection_lease final {
public:
    accepted_connection_lease(asio::ip::tcp::socket socket, connection_count& count,
        connection_lifecycle lifecycle) noexcept
        : slot_(count),
          socket_(std::move(socket)),
          lifecycle_(std::move(lifecycle)) {
        lifecycle_.bind_transport(connection_transport::tcp(socket_));
    }

    accepted_connection_lease(const accepted_connection_lease&) = delete;
    accepted_connection_lease& operator=(const accepted_connection_lease&) = delete;
    accepted_connection_lease(accepted_connection_lease&& other) noexcept
        : slot_(std::move(other.slot_)),
          socket_(std::move(other.socket_)),
          lifecycle_(std::move(other.lifecycle_)) {
        lifecycle_.bind_transport(connection_transport::tcp(socket_));
    }
    accepted_connection_lease& operator=(accepted_connection_lease&&) = delete;

    ~accepted_connection_lease() {
        // Session bodies may close their socket before deferred frame retirement.
        // Keep the count until retirement, and close cold tasks or early exits here.
        if (socket_.is_open()) {
            std::error_code ignored;
            socket_.close(ignored);
        }
    }

    [[nodiscard]] asio::ip::tcp::socket& socket() & noexcept {
        return socket_;
    }
    asio::ip::tcp::socket& socket() && = delete;
    [[nodiscard]] connection_lifecycle& lifecycle() noexcept {
        return lifecycle_;
    }

private:
    // Declared first: released after the socket closes.
    connection_slot slot_;
    asio::ip::tcp::socket socket_;
    connection_lifecycle lifecycle_;
};

}  // namespace ruvia::detail
