#pragma once

#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/socket_base.hpp>
#include <asio/write.hpp>

#include "borrowed_text.h"
#include "connection_scanner.h"
#include "event_loop.h"
#include "http1_connection_plan.h"
#include "http1_server_request.h"
#include "http1_server_response.h"
#include "http_io_buffer.h"
#include "http_request.h"
#include "io_operation.h"
#include "ip_address.h"
#include "route_table.h"
#include "server_socket_stream.h"

#if defined(__linux__)
#include <sys/ioctl.h>
#endif

// HTTP/1 connection session state: non-blocking plain TCP writes, the
// connection read buffer, deferred access logging, the per-connection work
// set, and the terminal completion of one dispatched request.
namespace ruvia::detail {

// Puts an accepted plain TCP socket into user non-blocking mode once, before
// its first I/O, so try_plain_tcp_sync_write() can offer bytes without
// blocking the worker. Asio's own internal non-blocking state is not enough:
// write_some() polls indefinitely unless the user mode is set. Asynchronous
// operations are unaffected on every platform: the reactor already runs the
// descriptor non-blocking, and on Windows asio issues overlapped
// WSASend/WSARecv/TransmitFile, which ignore FIONBIO, while a synchronous
// send() on a full send buffer returns WSAEWOULDBLOCK (asio::error::would_block)
// instead of blocking. A socket left blocking (the call failed) keeps every
// write on the asynchronous path.
inline void enable_plain_tcp_sync_write(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
#if defined(__linux__)
    // This socket is freshly accepted TCP, not an external descriptor. The
    // public command sets Asio's user mode as well as the kernel flag, without
    // the assigned-descriptor F_GETFL/F_SETFL round trip.
    struct non_blocking_command final {
        int enabled_{1};
        [[nodiscard]] int name() const noexcept {
            return FIONBIO;
        }
        [[nodiscard]] int* data() noexcept {
            return &enabled_;
        }
    } command;
    socket.io_control(command, ignored);
#else
    socket.non_blocking(true, ignored);
#endif
}

// Try one non-blocking plain TCP write before suspending. Returns true when
// the write completed or hit a hard error (outcome in ec/bytes_written), and
// false when the remainder must go through async_write.
template <typename socket_type, typename const_buffer_sequence_type>
[[nodiscard]] inline bool try_plain_tcp_sync_write(socket_type& socket, const const_buffer_sequence_type& buffers, std::size_t total_bytes, asio::socket_base::message_flags flags, std::error_code& ec, std::size_t& bytes_written) noexcept {
    ec.clear();
    bytes_written = 0;
    if (!socket.non_blocking()) {
        return false;
    }
    bytes_written = socket.send(buffers, flags, ec);
    if (!ec) {
        return bytes_written == total_bytes;
    }
    if (ec == asio::error::would_block || ec == asio::error::try_again || ec == asio::error::interrupted) {
        ec.clear();
        return false;
    }
    return true;
}

// Appends `parts` to `output` so a TLS stream encrypts them as one record. A
// gather write of the same parts is equivalent on the wire, so failing to
// allocate only selects that fallback: false leaves `output` unchanged. The
// reservation is the only allocation; the appends then fit.
[[nodiscard]] inline bool try_coalesce_tls_frame(http_response_head_buffer& output, std::initializer_list<std::string_view> parts) noexcept {
    std::size_t total = 0;
    for (const auto part : parts) {
        total += part.size();
    }
    try {
        output.reserve_additional(total);
    } catch (const std::bad_alloc&) {
        return false;
    }
    for (const auto part : parts) {
        output.append(part);
    }
    return true;
}

// A write either accepts every byte or ends with an error.
struct connection_write_outcome final {
    std::error_code error_;
    // Prefix of the written bytes the transport accepted.
    std::size_t bytes_{};
};

// Initiates the async_write of what follows the first `written` bytes of
// `head` then `body`. Every partial transfer touches `progress`.
template <typename stream_type, typename handler_type>
void async_write_after(stream_type& stream, std::string_view head, std::string_view body, std::size_t written, connection_scanner::entry& progress, handler_type&& handler) {
    if (written >= head.size()) {
        asio::async_write(stream, asio::buffer(body.substr(written - head.size())), connection_write_progress(progress), std::forward<handler_type>(handler));
        return;
    }
    const std::array<asio::const_buffer, 2> remaining{asio::buffer(head.substr(written)), asio::buffer(body)};
    asio::async_write(stream, remaining, connection_write_progress(progress), std::forward<handler_type>(handler));
}

// Awaitable write of `head` then `body`. Plain TCP first offers both to one
// non-blocking send; only a remainder suspends, as an async_write awaited in
// place, so a write the socket accepts at once costs no coroutine frame.
// Every transfer, synchronous or not, touches `progress`. The views must stay
// valid until the co_await completes.
template <typename stream_type>
class connection_gather_write final {
public:
    connection_gather_write(stream_type& stream, std::string_view head, std::string_view body, connection_scanner::entry& progress) noexcept :
        stream_(&stream),
        head_(head),
        body_(body),
        progress_(&progress) {}

    connection_gather_write(const connection_gather_write&) = delete;
    connection_gather_write& operator=(const connection_gather_write&) = delete;
    connection_gather_write* operator&() = delete;
    [[nodiscard]] bool await_ready() noexcept {
        if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, inbound_socket_stream>) {
            const std::array<asio::const_buffer, 2> buffers{asio::buffer(head_), asio::buffer(body_)};
            const bool settled = try_plain_tcp_sync_write(*stream_, buffers, head_.size() + body_.size(), 0, written_.error_, written_.bytes_);
            if (written_.bytes_ != 0) {
                progress_->note_write_progress();
            }
            return settled;
        } else {
            return false;
        }
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) {
        return remainder_.emplace(remainder_initiation{this}).await_suspend(handle);
    }

    [[nodiscard]] connection_write_outcome await_resume() {
        if (remainder_) {
            const auto completion = remainder_->await_resume();
            written_.error_ = completion.error_code();
            written_.bytes_ += completion.result();
        }
        return written_;
    }

private:
    struct remainder_initiation final {
        connection_gather_write* owner_;

        template <typename handler_type>
        void operator()(handler_type&& handler) const {
            async_write_after(*owner_->stream_, owner_->head_, owner_->body_, owner_->written_.bytes_, *owner_->progress_, std::forward<handler_type>(handler));
        }
    };

    stream_type* stream_;
    std::string_view head_;
    std::string_view body_;
    connection_scanner::entry* progress_;
    connection_write_outcome written_;
    std::optional<asio_completion_awaiter<std::size_t, remainder_initiation>> remainder_;
};

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
    void defer(std::size_t head_end, const http_request& request, ip_address client_address, std::chrono::steady_clock::time_point start, http_status_code status, const request_io_metrics& io_metrics = {});
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

    explicit constexpr http1_request_buffer_compaction(std::size_t consumed_bytes) noexcept :
        consumed_bytes_(consumed_bytes) {}

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

    explicit constexpr http1_request_buffer_pipeline_restore(std::string_view pipeline) noexcept :
        pipeline_(pipeline) {}

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

    [[nodiscard]] constexpr const http1_request_buffer_pipeline_restore* pipeline_restore() const& noexcept {
        return std::get_if<http1_request_buffer_pipeline_restore>(&value_);
    }
    [[nodiscard]] constexpr const http1_request_buffer_pipeline_restore* pipeline_restore() const&& = delete;

private:
    friend class http1_session_request_completion;

    using value_type = std::variant<http1_request_buffer_discarded, http1_request_buffer_compaction, http1_request_buffer_pipeline_restore>;

    template <typename alternative_type>
    explicit constexpr http1_request_buffer_completion(alternative_type alternative) noexcept :
        value_(std::move(alternative)) {}

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

    constexpr http1_committed_stream_response(http_status_code status, bool truncated) noexcept :
        status_(status),
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

    explicit http1_rejected_final_response(::ruvia::http1_response_head_error error) noexcept :
        error_(std::move(error)) {}

    ::ruvia::http1_response_head_error error_;
};

// One terminal result for a dispatched HTTP/1 request. Wire ownership,
// connection reuse, and read-buffer cleanup are committed together so the
// session never reconstructs them from an optional status plus scalar flags.
class http1_session_request_completion final {
public:
    [[nodiscard]] static http1_session_request_completion make_buffered_closing(http1_request_connection_plan connection_plan) noexcept {
        if (connection_plan.disposition() != http1_close_policy::close_after_response) {
            std::terminate();
        }
        return http1_session_request_completion(http1_buffered_response_ready{}, connection_plan, http1_request_buffer_completion(http1_request_buffer_discarded{}));
    }

    [[nodiscard]] static http1_session_request_completion make_buffered_unrestored(http1_request_connection_plan connection_plan, std::size_t consumed_bytes) noexcept {
        return http1_session_request_completion(http1_buffered_response_ready{}, connection_plan, unshifted_buffer_completion(connection_plan, consumed_bytes));
    }

    [[nodiscard]] static http1_session_request_completion make_buffered_pipeline_restore(http1_request_connection_plan connection_plan, borrowed_text pipeline) noexcept {
        if (connection_plan.disposition() != http1_close_policy::allow_reuse) {
            std::terminate();
        }
        return http1_session_request_completion(http1_buffered_response_ready{}, connection_plan, http1_request_buffer_completion(http1_request_buffer_pipeline_restore(pipeline.view())));
    }

    [[nodiscard]] static http1_session_request_completion make_committed_stream(http1_request_connection_plan connection_plan, http_status_code status, std::size_t consumed_bytes) noexcept {
        return http1_session_request_completion(http1_committed_stream_response(status, false), connection_plan, unshifted_buffer_completion(connection_plan, consumed_bytes));
    }

    [[nodiscard]] static http1_session_request_completion make_committed_stream_pipeline_restore(http1_request_connection_plan connection_plan, http_status_code status, borrowed_text pipeline) noexcept {
        if (connection_plan.disposition() != http1_close_policy::allow_reuse) {
            std::terminate();
        }
        return http1_session_request_completion(http1_committed_stream_response(status, false), connection_plan, http1_request_buffer_completion(http1_request_buffer_pipeline_restore(pipeline.view())));
    }

    [[nodiscard]] static http1_session_request_completion make_truncated_stream(http1_request_connection_plan connection_plan, http_status_code status) noexcept {
        return http1_session_request_completion(http1_committed_stream_response(status, true), connection_plan.require_close(), http1_request_buffer_completion(http1_request_buffer_discarded{}));
    }

    [[nodiscard]] static http1_session_request_completion make_rejected_final_response(http1_request_connection_plan connection_plan, ::ruvia::http1_response_head_error error) noexcept {
        return http1_session_request_completion(http1_rejected_final_response(std::move(error)), connection_plan.require_close(), http1_request_buffer_completion(http1_request_buffer_discarded{}));
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
    [[nodiscard]] http1_session_request_completion with_buffered_connection_plan(http1_request_connection_plan connection_plan) const& noexcept {
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
    using value_type = std::variant<http1_buffered_response_ready, http1_committed_stream_response, http1_rejected_final_response>;

    [[nodiscard]] static http1_request_buffer_completion unshifted_buffer_completion(http1_request_connection_plan connection_plan, std::size_t consumed_bytes) noexcept {
        if (connection_plan.disposition() == http1_close_policy::close_after_response) {
            return http1_request_buffer_completion(http1_request_buffer_discarded{});
        }
        return http1_request_buffer_completion(http1_request_buffer_compaction(consumed_bytes));
    }

    template <typename alternative_type>
    http1_session_request_completion(alternative_type alternative, http1_request_connection_plan connection_plan, http1_request_buffer_completion buffer_completion) noexcept :
        value_(std::move(alternative)),
        connection_plan_(connection_plan),
        buffer_completion_(std::move(buffer_completion)) {}

    value_type value_;
    http1_request_connection_plan connection_plan_;
    http1_request_buffer_completion buffer_completion_;
};

}  // namespace ruvia::detail
