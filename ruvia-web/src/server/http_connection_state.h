#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <asio/ip/tcp.hpp>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http1_request_connection_plan.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http1_server_semantics.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response_head_buffer.h"
#include "ruvia/http/http_status.h"

#include "integration/request_io_metrics.h"
#include "router/route_table.h"
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
    connection_read_buffer() noexcept;

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
    http1_deferred_access_log() noexcept;

    // Keeps the record of a response whose head completes at output offset
    // `head_end`. Duration is measured when the record is reported.
    void defer(std::size_t head_end, const http_request& request, ip_address client_address,
        std::chrono::steady_clock::time_point start, http_status_code status,
        const request_io_metrics& io_metrics = {});
    // Reports every kept response whose head lies within the first `written`
    // output bytes (in output order), then forgets all of them.
    void complete(const access_log_sink& access_log, ip_address remote_address, std::size_t written) noexcept;
    // Forgets every kept response and releases grown storage.
    void reset() noexcept;

private:
    struct entry_type final {
        std::size_t head_end_;
        std::size_t text_begin_;
        std::size_t method_bytes_;
        std::size_t path_bytes_;
        ip_address client_address_;
        std::chrono::steady_clock::time_point start_;
        request_io_metrics io_metrics_{};
        http_known_method known_method_;
        http_protocol_version protocol_version_;
        http_status_code status_;
    };

    std::vector<entry_type> entries_;
    std::string text_;
};

// Heavy request storage exists only while the connection is serving requests.
// A pipelined burst reuses it; idle connections retain only their session frame.
// The work set and its configured initial arena block share one allocation.
struct connection_work_set final {
    connection_work_set() noexcept;

    connection_read_buffer read_buffer_;
    http_response_head_buffer response_head_;
    http1_deferred_access_log deferred_access_;
    http1_server_request_parse_state parsed_;
    route_resolution route_resolution_;
};

[[nodiscard]] std::unique_ptr<connection_work_set> make_connection_work_set();

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

namespace ruvia::detail {

// The connection is closing, so no bytes from this request can be reused.
class http1_request_buffer_discarded final {
private:
    friend class http1_session_request_completion;

    constexpr http1_request_buffer_discarded() noexcept = default;
};

// The session still owns an unshifted read buffer and must remove exactly this
// request prefix before parsing the next pipelined request.
class http1_request_buffer_compaction final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http1_session_request_completion;

    explicit constexpr http1_request_buffer_compaction(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}

    std::size_t consumed_bytes_;
};

// A request-body runtime over-read the next pipelined request into its own
// buffer, which dies with the route, and handed those bytes over in
// request-scoped storage that outlives the completion. The session installs
// them at its single buffer-cleanup point, once the response is written and
// the access log is recorded. A suffix still in the read buffer is a
// compaction instead.
class http1_request_buffer_pipeline_restore final {
public:
    [[nodiscard]] constexpr std::string_view pipeline() const noexcept {
        return pipeline_;
    }

private:
    friend class http1_session_request_completion;

    explicit constexpr http1_request_buffer_pipeline_restore(std::string_view pipeline) noexcept
        : pipeline_(pipeline) {}

    std::string_view pipeline_;
};

class http1_request_buffer_completion final {
public:
    [[nodiscard]] constexpr const http1_request_buffer_discarded* discarded() const& noexcept {
        return std::get_if<http1_request_buffer_discarded>(&value_);
    }
    [[nodiscard]] constexpr const http1_request_buffer_discarded* discarded() const&& = delete;

    [[nodiscard]] constexpr const http1_request_buffer_compaction* compaction() const& noexcept {
        return std::get_if<http1_request_buffer_compaction>(&value_);
    }
    [[nodiscard]] constexpr const http1_request_buffer_compaction* compaction() const&& = delete;

    [[nodiscard]] constexpr const http1_request_buffer_pipeline_restore* pipeline_restore()
        const& noexcept {
        return std::get_if<http1_request_buffer_pipeline_restore>(&value_);
    }
    [[nodiscard]] constexpr const http1_request_buffer_pipeline_restore* pipeline_restore() const&& =
        delete;

private:
    friend class http1_session_request_completion;

    using value_type = std::variant<http1_request_buffer_discarded, http1_request_buffer_compaction,
        http1_request_buffer_pipeline_restore>;

    template <typename alternative_type>
    explicit constexpr http1_request_buffer_completion(alternative_type alternative) noexcept
        : value_(std::move(alternative)) {}

    value_type value_;
};

// A buffered response still needs the session's scatter-gather writer.
class http1_buffered_response_ready final {
private:
    friend class http1_session_request_completion;

    constexpr http1_buffered_response_ready() noexcept = default;
};

// A response-stream route already committed its final head. Only this
// alternative exposes the exact wire status used by access logging.
class http1_committed_stream_response final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    // The handler failed after the head was on the wire. A close-delimited or
    // TLS body ended by a clean close would read as complete (RFC 9112 §9.8),
    // so the connection is aborted rather than closed gracefully.
    [[nodiscard]] constexpr bool truncated() const noexcept {
        return truncated_;
    }

private:
    friend class http1_session_request_completion;

    constexpr http1_committed_stream_response(http_status_code status, bool truncated) noexcept
        : status_(status),
          truncated_(truncated) {}

    http_status_code status_;
    bool truncated_;
};

// A route's buffered response broke the HTTP/1 final-response rules, so it
// cannot be committed. The session replaces it through the exception handler
// on a closing connection before preparing the response for the wire.
class http1_rejected_final_response final {
public:
    [[nodiscard]] http1_response_head_error exception() const noexcept {
        return error_;
    }

private:
    friend class http1_session_request_completion;

    explicit http1_rejected_final_response(::ruvia::http1_response_head_error error) noexcept
        : error_(std::move(error)) {}

    ::ruvia::http1_response_head_error error_;
};

// One terminal result for a dispatched HTTP/1 request. Wire ownership,
// connection reuse, and read-buffer cleanup are committed together so the
// session never reconstructs them from an optional status plus scalar flags.
class http1_session_request_completion final {
public:
    [[nodiscard]] static http1_session_request_completion make_buffered_closing(
        http1_request_connection_plan connection_plan) noexcept {
        if (connection_plan.disposition() != http1_close_policy::close_after_response) {
            std::terminate();
        }
        return http1_session_request_completion(http1_buffered_response_ready{}, connection_plan,
            http1_request_buffer_completion(http1_request_buffer_discarded{}));
    }

    [[nodiscard]] static http1_session_request_completion make_buffered_unrestored(
        http1_request_connection_plan connection_plan, std::size_t consumed_bytes) noexcept {
        return http1_session_request_completion(http1_buffered_response_ready{}, connection_plan,
            unshifted_buffer_completion(connection_plan, consumed_bytes));
    }

    [[nodiscard]] static http1_session_request_completion make_buffered_pipeline_restore(
        http1_request_connection_plan connection_plan, borrowed_text pipeline) noexcept {
        if (connection_plan.disposition() != http1_close_policy::allow_reuse) {
            std::terminate();
        }
        return http1_session_request_completion(http1_buffered_response_ready{}, connection_plan,
            http1_request_buffer_completion(http1_request_buffer_pipeline_restore(pipeline.view())));
    }

    [[nodiscard]] static http1_session_request_completion make_committed_stream(
        http1_request_connection_plan connection_plan, http_status_code status,
        std::size_t consumed_bytes) noexcept {
        return http1_session_request_completion(http1_committed_stream_response(status, false), connection_plan,
            unshifted_buffer_completion(connection_plan, consumed_bytes));
    }

    [[nodiscard]] static http1_session_request_completion make_committed_stream_pipeline_restore(
        http1_request_connection_plan connection_plan, http_status_code status, borrowed_text pipeline) noexcept {
        if (connection_plan.disposition() != http1_close_policy::allow_reuse) {
            std::terminate();
        }
        return http1_session_request_completion(http1_committed_stream_response(status, false), connection_plan,
            http1_request_buffer_completion(http1_request_buffer_pipeline_restore(pipeline.view())));
    }

    [[nodiscard]] static http1_session_request_completion make_truncated_stream(
        http1_request_connection_plan connection_plan, http_status_code status) noexcept {
        return http1_session_request_completion(http1_committed_stream_response(status, true),
            connection_plan.require_close(), http1_request_buffer_completion(http1_request_buffer_discarded{}));
    }

    [[nodiscard]] static http1_session_request_completion make_rejected_final_response(
        http1_request_connection_plan connection_plan, ::ruvia::http1_response_head_error error) noexcept {
        return http1_session_request_completion(http1_rejected_final_response(std::move(error)),
            connection_plan.require_close(), http1_request_buffer_completion(http1_request_buffer_discarded{}));
    }

    [[nodiscard]] constexpr const http1_buffered_response_ready* buffered_response() const& noexcept {
        return std::get_if<http1_buffered_response_ready>(&value_);
    }
    [[nodiscard]] constexpr const http1_buffered_response_ready* buffered_response() const&& = delete;

    [[nodiscard]] constexpr const http1_committed_stream_response* committed_stream() const& noexcept {
        return std::get_if<http1_committed_stream_response>(&value_);
    }
    [[nodiscard]] constexpr const http1_committed_stream_response* committed_stream() const&& = delete;

    [[nodiscard]] constexpr const http1_rejected_final_response* rejected_final_response() const& noexcept {
        return std::get_if<http1_rejected_final_response>(&value_);
    }
    [[nodiscard]] constexpr const http1_rejected_final_response* rejected_final_response() const&& = delete;

    [[nodiscard]] constexpr http1_request_connection_plan connection_plan() const noexcept {
        return connection_plan_;
    }

    [[nodiscard]] constexpr const http1_request_buffer_completion& buffer_completion() const& noexcept {
        return buffer_completion_;
    }
    [[nodiscard]] constexpr const http1_request_buffer_completion& buffer_completion() const&& = delete;

    // A buffered response can be replaced by a pre-commit policy error after
    // representation preparation (for example a forbidden identity fallback).
    // Rebind the final response plan without losing the exact read-buffer
    // cleanup alternative already established by request-body dispatch.
    [[nodiscard]] http1_session_request_completion with_buffered_connection_plan(
        http1_request_connection_plan connection_plan) const& noexcept {
        if (std::get_if<http1_buffered_response_ready>(&value_) == nullptr) {
            std::terminate();
        }
        if (buffer_completion_.discarded() != nullptr) {
            return make_buffered_closing(connection_plan.require_close());
        }
        if (const auto* compaction = buffer_completion_.compaction()) {
            return make_buffered_unrestored(connection_plan, compaction->consumed_bytes());
        }
        if (const auto* pipeline = buffer_completion_.pipeline_restore()) {
            if (connection_plan.disposition() == http1_close_policy::close_after_response) {
                return make_buffered_closing(connection_plan);
            }
            return make_buffered_pipeline_restore(connection_plan, pipeline->pipeline());
        }
        std::terminate();
    }

private:
    using value_type = std::variant<http1_buffered_response_ready, http1_committed_stream_response,
        http1_rejected_final_response>;

    [[nodiscard]] static http1_request_buffer_completion unshifted_buffer_completion(
        http1_request_connection_plan connection_plan, std::size_t consumed_bytes) noexcept {
        if (connection_plan.disposition() == http1_close_policy::close_after_response) {
            return http1_request_buffer_completion(http1_request_buffer_discarded{});
        }
        return http1_request_buffer_completion(http1_request_buffer_compaction(consumed_bytes));
    }

    template <typename alternative_type>
    http1_session_request_completion(alternative_type alternative, http1_request_connection_plan connection_plan,
        http1_request_buffer_completion buffer_completion) noexcept
        : value_(std::move(alternative)),
          connection_plan_(connection_plan),
          buffer_completion_(std::move(buffer_completion)) {}

    value_type value_;
    http1_request_connection_plan connection_plan_;
    http1_request_buffer_completion buffer_completion_;
};

}  // namespace ruvia::detail
