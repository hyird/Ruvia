#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/web_worker.h"

#include "http/operation_interrupt.h"
#include "server/connection_lifecycle.h"
#include "server/http_server_options.h"
#include "server/route_table.h"

#if defined(RUVIA_ENABLE_HTTP3)
#include "http3/http3_session.h"
#endif

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

#if defined(_WIN32)
#endif

namespace ruvia {
class worker_runtime;
}

namespace ruvia::detail {

using tcp_socket_type = asio::ip::tcp::socket;
class accepted_connection_lease;
class context_services;
template <typename stream_type>
struct http1_connection_session;
class route_table;
struct server_connection_fields;
class worker_capabilities;

// What serve_requests hands back to run_stream when the connection stays open:
// no input is left buffered (the next request is awaited without a work set),
// or a complete prior-knowledge HTTP/2 preface is pending in the work set's
// read buffer (RFC 9113 §3.3).
enum class http1_serve_handoff : std::uint8_t {
    await_request,
    cleartext_http2,
};

#if !defined(_WIN32)
// One worker's dup()s of a listener's sockets; udp_ is invalid without an
// HTTP/3 service.
struct listener_socket_duplicates final {
    native_socket tcp_{invalid_native_socket};
    native_socket udp_{invalid_native_socket};
};
#endif

// Worker-local listeners, connection resources and session assembly. The
// surrounding runtime owns lifecycle phases and the scope joining every task.
// Admission runs open -> stop, and stopping closes every connection at once:
// there is no drain. On POSIX each worker binds every TCP listener with
// SO_REUSEPORT and accepts on its own event loop; on Windows the
// application's acceptor accepts and hands each socket over (adopt_socket).
// A TLS listener with an HTTP/3 service also gets a QUIC endpoint
// (http3_endpoint), which admits and stops the same way.
//
// Listener slots are numbered alike on every worker (and in the Windows
// acceptor) and never reused within a run, so a hand-over queued for a
// removed listener cannot reach another. A listener added while running is
// prepared (bound, not admitting) on every worker before any opens it; a
// removed one stops admitting at once and retires -- slot emptied, its
// session configuration destroyed -- once every task it owns has ended.
class worker_connections final {
public:
    worker_connections(asio::io_context& io, const worker_ref& worker_value,
        const route_table& routes_value, worker_capabilities& capabilities,
        http_server_options& options, const stop_token& stop_token_value,
        http_server_worker_state& state_value, task_scope& tasks,
        std::span<const http_server_listener_definition> listeners, worker_failure_sink failure, std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_placement http3_placement
#endif
    );
    worker_connections(const worker_connections&) = delete;
    worker_connections& operator=(const worker_connections&) = delete;
    void prepare();
    void open_admission() noexcept;
    // Worker thread, once the worker state left running: stops admission and
    // closes every connection at once.
    void stop() noexcept;
    void retire_tls() noexcept;
    // Worker thread: listener `listener_index` -- TLS over TCP and its
    // HTTP/3 service alike -- selects every later handshake's identity from
    // `identities`.
    void adopt_tls_identities(
        std::size_t listener_index, std::shared_ptr<const http_server_tls_identities> identities) noexcept;
    // Worker thread, the controls of application::add_listener(): prepares
    // `definition` in slot `index` as prepare() would -- TLS contexts, bound
    // or adopted sockets, the HTTP/3 endpoint in slot `http3_index` -- without
    // admitting, or throws with the slot left empty.
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index,
        const http_server_listener_definition& definition,
        bool externally_accepted = false
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_directory* directory = nullptr
#endif
    );
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index,
        const http_server_listener_definition& definition, bool externally_accepted,
        prepared_http_server_session session
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_directory* directory = nullptr
#endif
    );
    // 唤醒准备阶段已经创建的接入任务；提交不再分配监听资源。
    void open_listener(std::size_t index) noexcept;
    // Closes the sockets of a listener prepared but never opened and retires
    // it; an empty slot is left alone.
    void discard_listener(std::size_t index, std::shared_ptr<void> receipt = {}) noexcept;
    // application::remove_listener(): stops admitting listener `index` and
    // closes its TCP socket at once, holding `receipt` until its accept loop
    // exited. Its connections are served on; its HTTP/3 endpoint stops
    // admitting and closes its socket after its last connection.
    void remove_listener(std::size_t index, std::shared_ptr<void> receipt, listener_remove_options options = {}) noexcept;
#if !defined(_WIN32)
    // application::duplicate_listener_sockets(): this worker's sockets of
    // listener `index`, duplicated. Throws std::logic_error when it no longer
    // admits.
    [[nodiscard]] listener_socket_duplicates duplicate_listener(std::size_t index);
#endif
    // Takes an already accepted native socket, including ownership on failure.
    // It first binds that socket to this worker's execution context.
    void adopt_connection(std::size_t index, native_socket socket, adopt_options options = {});
    [[nodiscard]] http_server_stats stats() const;
    [[nodiscard]] std::shared_ptr<connection_count> listener_connections(const asio::ip::tcp::endpoint& endpoint) const;
    // Owning worker only; false for an unknown or retired connection ID.
    [[nodiscard]] bool close_connection(std::uint64_t id) noexcept;
#if defined(_WIN32)
    // This worker as the acceptor's hand-over target.
    [[nodiscard]] windows_acceptor::business_worker acceptor_target() noexcept;
    void bind_acceptor(windows_acceptor& acceptor) noexcept {
#if defined(RUVIA_ENABLE_HTTP3)
        http3_placement_.acceptor_ = &acceptor;
        http3_services_.placement_.acceptor_ = &acceptor;
#else
        (void)acceptor;
#endif
    }
#endif

private:
    struct listener final {
        listener(asio::io_context& io, const worker_ref& worker_value,
            const http_server_listener_definition& definition,
            prepared_http_server_session session = {})
            : session_owner_(std::move(session)),
              session_(session_owner_ ? *session_owner_ : inline_session_.emplace(definition)),
              endpoint_(definition.endpoint_),
              tasks_(worker_value),
              retired_(worker_value),
              activated_(worker_value)
#if !defined(_WIN32)
              ,
              socket_(io),
              inherited_(definition.inherited_sockets_)
#endif
        {
#if defined(_WIN32)
            (void)io;
#endif
        }

        prepared_http_server_session session_owner_;
        std::optional<http_server_session_config> inline_session_;
        http_server_session_config& session_;
        asio::ip::tcp::endpoint endpoint_;
        // Its accept loop, sessions and HTTP/3 endpoint tasks, all joined
        // before the listener is destroyed: they borrow session_.
        task_scope tasks_;
        // Notified once removal or the worker's stop ended admission.
        worker_signal retired_;
        worker_signal activated_;
        // 取消准备持有至该监听的全部任务已 join；强制移除不等待调用它的会话自身。
        std::shared_ptr<void> retirement_receipt_;
#if defined(RUVIA_ENABLE_HTTP3)
        std::optional<std::size_t> http3_index_;
#endif
#if !defined(_WIN32)
        asio::ip::tcp::acceptor socket_;
        // Until preparation claimed this worker's sockets.
        std::shared_ptr<inherited_listener_sockets> inherited_;
        // A removal's, until the accept loop exited.
        std::shared_ptr<void> removal_receipt_;
        bool accepting_{false};
#endif
        // From preparation on, a task awaits retired_ to retire the listener.
        bool prepared_{false};
        // From opening until removal or the worker's stop.
        bool admitting_{false};
        bool externally_accepted_{false};
    };
#if defined(RUVIA_ENABLE_HTTP3)
    void add_http3_endpoint(std::size_t listener_index, std::size_t http3_index);
    void receive_http3_handoff(std::size_t listener_index, http3_handoff_datagram& datagram) noexcept;
#endif
    // Builds the listener's TLS contexts, binds or adopts its sockets and
    // starts the task that retires it.
    void prepare_listener_resources(std::size_t index);
    void start_listener(std::size_t index) noexcept;
    void close_listener_sockets(listener& configured) noexcept;
    // Awaits the listener's retired_, joins its tasks, then empties its slot.
    task<void> retire_listener(std::size_t index);
#if defined(_WIN32)
    // The worker thread: binds a socket the acceptor accepted to this
    // worker's io_context (its first IOCP association) and serves it.
    void adopt_socket(std::size_t listener_index, windows_accepted_socket accepted);
#else
    void close_listeners() noexcept;
    task<void> run_accept_loop(std::size_t index);
#endif
    void report_accept_failure(std::exception_ptr failure, bool fatal) noexcept;
    task<void> rotate_tls_ticket_keys();
    void accept_socket(std::size_t index, tcp_socket_type socket, adopt_options options = {});
    task<void> prepare_connection(std::size_t index, tcp_socket_type socket,
        std::vector<std::byte> prefix, std::optional<ip_endpoint> remote);
    void admit_socket(std::size_t index, tcp_socket_type socket, std::optional<ip_endpoint> remote,
        std::vector<std::byte> prefix, std::size_t bytes_read = 0);
    // `remote` is the peer accept_socket() read before the socket moved into
    // the lease; absent when the socket no longer knew its peer.
    task<void> run_session(http_server_session_config& listener, accepted_connection_lease connection,
        std::optional<asio::ip::tcp::endpoint> remote, std::uint64_t connection_id, std::vector<std::byte> prefix);
    task<void> run_tls_session(http_server_session_config& listener, accepted_connection_lease connection,
        std::optional<asio::ip::tcp::endpoint> remote, std::uint64_t connection_id, std::vector<std::byte> prefix);
    void report_session_failure(
        tcp_socket_type& socket, std::string_view remote_address, std::exception_ptr failure) noexcept;
    template <typename stream_type>
    task<http_connection_close> run_stream(http_server_session_config& listener_value, stream_type& stream, tcp_socket_type& socket, context_services services);
#if defined(RUVIA_ENABLE_HTTP2)
    template <typename stream_type>
    task<http_connection_close> run_http2(stream_type& stream, tcp_socket_type& socket, context_services services);
#endif
    // A listener_dispatch::connection listener's connection: application::on_connection
    // owns it until its task completes. `fields` names everything but the
    // local endpoint, which this reads from `socket`.
    template <typename stream_type>
    task<void> serve_connection(stream_type& stream, tcp_socket_type& socket, server_connection_fields fields);
    template <typename stream_type>
    task<std::variant<http1_serve_handoff, http_connection_close>> serve_requests(
        http1_connection_session<stream_type>& session, connection_work_set& work_set);
    asio::io_context& io_;
    const worker_ref& worker_;
    const route_table& routes_;
    worker_capabilities& capabilities_;
    http_server_options& options_;
    const stop_token& stop_token_;
    http_server_worker_state& state_;
    task_scope& tasks_;
    worker_failure_sink failure_;
    // This worker's position among the application's workers: the inherited
    // socket of each listener it claims.
    std::size_t worker_index_;
    // Indexed by listener slot; empty once a listener retired or failed to
    // prepare.
    std::vector<std::unique_ptr<listener>> listeners_;
    connection_scanner scanner_;
    // Caller-thread statistics and the Windows acceptor's balancing read it.
    connection_count connection_count_;
    struct listener_gauge final {
        listener_gauge(const asio::ip::tcp::endpoint& endpoint, connection_count& total)
            : endpoint_(endpoint),
              count_(&total) {}
        asio::ip::tcp::endpoint endpoint_;
        connection_count count_;
        std::atomic<bool> serving_{false};
        std::atomic<bool> published_{false};
    };
    void prepare_listener_gauge(std::size_t index, const asio::ip::tcp::endpoint& endpoint);
    mutable std::mutex gauges_mutex_;
    std::vector<std::shared_ptr<listener_gauge>> gauges_{};
    connection_id_sequence connection_ids_;
    connection_registry registry_;
    std::atomic<std::size_t> refused_{};
    std::atomic<std::size_t> failures_{};
    std::atomic<std::size_t> accept_failures_{};
    bool rotating_ticket_keys_{false};
#if defined(RUVIA_ENABLE_HTTP3)
    http3_worker_placement http3_placement_;
    http3_worker_services http3_services_;
    bool http3_attached_{false};
    // Indexed by HTTP/3 slot, like listeners_. Declared last: destroyed
    // before every service its connections borrow.
    std::vector<std::unique_ptr<http3_endpoint>> http3_endpoints_;
#endif
};

}  // namespace ruvia::detail
