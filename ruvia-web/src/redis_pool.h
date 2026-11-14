#pragma once

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct redisReader;

#include <asio/fwd.hpp>
#include <asio/io_context.hpp>
#include <asio/ssl/context.hpp>

#include "http_client_pool.h"
#include "redis_session.h"
#include "slot_pool.h"
#include "startup_config.h"
#include "stop_token.h"
#include "task.h"
#include "task_scope.h"
#include "tcp_socket_policy.h"
#include "worker_ref.h"
#include "worker_wait.h"

namespace ruvia::detail {

inline void validate_redis_config(const redis_config& config) {
    validate_client_tls_config(config.tls_);
    validate_client_origin_host(config.host_, "redis host must not be empty", "redis host is invalid");
    detail::ensure_non_zero_port(config.port_.value(), "redis port must not be zero");
    detail::ensure_positive_size(config.pool_size_per_worker_, "redis pool size must be greater than zero");
    detail::ensure_positive_size(config.blocking_pool_size_per_worker_, "redis blocking pool size must be greater than zero");
    detail::ensure_positive_optional_durations("configured redis timeouts must be greater than zero", config.connect_timeout_, config.command_timeout_, config.acquire_timeout_);
    detail::ensure_positive_size(config.max_array_depth_, "redis max array depth must be greater than zero");
    detail::ensure_positive_optional_size(config.max_reply_bytes_, "configured redis reply byte limit must be greater than zero");
    validate_client_tcp_config(config.tcp_);
    detail::ensure_positive_optional_duration(config.subscription_ping_interval_, "redis subscription ping interval must be greater than zero");
}

// Owning runtime form of the ordinary public redis_config value.
struct redis_config_storage final {
    redis_config_storage(const redis_config& source_value) :
        redis_config_storage(validated_config_tag_type{}, validate(source_value)) {}

    redis_config_storage(const redis_config_storage& source_value) :
        redis_config_storage(validated_config_tag_type{}, source_value) {}

    std::string host_;
    std::uint16_t port_;
    std::string username_;
    std::string password_;
    client_tls_config_storage tls_;
    std::uint32_t database_;
    std::size_t pool_size_per_worker_;
    std::size_t blocking_pool_size_per_worker_;
    std::optional<timeout_duration> connect_timeout_;
    std::optional<timeout_duration> command_timeout_;
    std::optional<timeout_duration> acquire_timeout_;
    std::optional<std::size_t> max_reply_bytes_;
    std::size_t max_array_depth_;
    client_tcp_config tcp_;
    std::optional<timeout_duration> subscription_ping_interval_;

private:
    struct validated_config_tag_type final {};

    [[nodiscard]] static const redis_config& validate(const redis_config& source_value) {
        validate_redis_config(source_value);
        return source_value;
    }

    [[nodiscard]] static std::uint16_t resolved_port(const redis_config& source) noexcept {
        return source.port_.value();
    }
    [[nodiscard]] static std::uint16_t resolved_port(const redis_config_storage& source) noexcept {
        return source.port_;
    }

    // Public configuration parses the TLS files every pool's context
    // installs; allocator-rebound copies share them.
    template <typename source_type>
    [[nodiscard]] static client_tls_config_storage tls_storage(const source_type& source_value) {
        if constexpr (std::same_as<source_type, redis_config>) {
            return {source_value.tls_, client_tls_files::parse};
        } else {
            return {source_value.tls_};
        }
    }

    // Public configuration validation and allocator-rebound copies enter the
    // same owning construction path; no intermediate string copies or views.
    template <typename source_type>
        requires(std::same_as<source_type, redis_config> || std::same_as<source_type, redis_config_storage>)
    redis_config_storage(validated_config_tag_type, const source_type& source_value) :
        host_(source_value.host_),
        port_(resolved_port(source_value)),
        username_(source_value.username_),
        password_(source_value.password_),
        tls_(tls_storage(source_value)),
        database_(source_value.database_),
        pool_size_per_worker_(source_value.pool_size_per_worker_),
        blocking_pool_size_per_worker_(source_value.blocking_pool_size_per_worker_),
        connect_timeout_(source_value.connect_timeout_),
        command_timeout_(source_value.command_timeout_),
        acquire_timeout_(source_value.acquire_timeout_),
        max_reply_bytes_(source_value.max_reply_bytes_),
        max_array_depth_(source_value.max_array_depth_),
        tcp_(source_value.tcp_),
        subscription_ping_interval_(source_value.subscription_ping_interval_) {}
};

using redis_definition_type = named_capability_definition<redis_config_storage>;

struct redis_reader_deleter {
    void operator()(redisReader* reader) const noexcept;
};

class redis_reply_builder;

inline constexpr std::size_t redis_read_buffer_bytes = 8192;
// Serialized request capacity retained after a write's transport borrow ends.
// Larger buffers are released, including on failure.
inline constexpr std::size_t redis_retained_write_buffer_bytes = std::size_t{64} * 1024;

// A credential request (AUTH) is wiped from the write buffer once written, so
// the retained buffer never holds a password after the write completes.
enum class redis_request_content : bool { ordinary,
    credential };

// One Redis connection: the shared outbound transport plus the RESP request
// and reply buffers. Pool slots and subscriptions each own one. Address-stable:
// the transport's pending I/O and timers and the reader's object functions
// borrow it. The owner serializes requests into write_buffer(), sends them
// with write_request() and reads replies one at a time; a subscription keeps
// reading pushed replies without writing.
//
// Every failed transport result is mapped to redis_error here. An owner
// abort (cancelled or closing) is recorded on the transport and checked
// after each wait, so cancellation and shutdown never surface as I/O errors;
// an aborted result without an owner reason is an expired deadline.
class redis_connection final {
public:
    redis_connection(asio::io_context& io_context, asio::ssl::context& tls_context, const worker_ref& worker_value, const redis_config_storage& config, client_wire_counters& counters);
    redis_connection(asio::io_context&, asio::ssl::context&, worker_ref&&, const redis_config_storage&, client_wire_counters&) = delete;
    ~redis_connection();

    redis_connection(const redis_connection&) = delete;
    redis_connection& operator=(const redis_connection&) = delete;

    [[nodiscard]] bool connected() const noexcept {
        return connected_;
    }

    // Connects when closed. A connected connection whose peer closed it or
    // sent unsolicited bytes would desynchronize the next reply, so it is
    // replaced. The optional operation deadline bounds establishment together
    // with connect_timeout; AUTH and SELECT replies also honor command_timeout.
    task<void> connect(const operation_deadline* operation_timeout_value, std::optional<timeout_duration> command_timeout);
    // Serialized request storage: append RESP commands, then write_request().
    [[nodiscard]] std::string& write_buffer() noexcept {
        return write_buffer_;
    }
    // Sends and then releases the serialized request, wiping it first when it
    // carries a credential, whether or not the write succeeded.
    task<void> write_request(const operation_deadline& timeout, redis_request_content content = redis_request_content::ordinary);
    // Reads one complete reply into owned nodes. Throws
    // redis_error(io_error) when the connection is or becomes closed. A
    // non-null `input_time` receives the time of every socket read that
    // delivered bytes, so a subscription's idle clock restarts on partial
    // replies too.
    task<redis_value> read_reply(const operation_deadline& timeout, std::chrono::steady_clock::time_point* input_time = nullptr);

    void close() noexcept;
    // Cancellation/shutdown: records the reason so a resolve completing after
    // this point starts no connect or handshake, then closes the connection.
    void abort(client_abort_reason reason) noexcept;
    // Throws the redis_error of a recorded owner abort (cancelled or closing).
    void throw_if_aborted() const;

private:
    void ensure_reader();
    task<void> authenticate(const operation_deadline& connect_timeout, std::optional<timeout_duration> command_timeout);
    // One connection-setup command (AUTH, SELECT) and its reply.
    task<redis_value> exchange_control_command(std::span<const std::string_view> args, operation_deadline deadline, redis_request_content content);

    const redis_config_storage& config_;
    client_tcp_transport transport_;
    std::string write_buffer_;
    std::array<char, redis_read_buffer_bytes> read_buffer_;
    // Created on first connect; the reader's object functions address the
    // builder, so it outlives every reader.
    std::unique_ptr<redis_reply_builder> reply_builder_;
    std::unique_ptr<redisReader, redis_reader_deleter> reader_;
    std::size_t reply_bytes_{0};
    bool connected_{false};
};

struct redis_command_args_view final {
    std::span<const std::string> args_;
};

class redis_pool final {
public:
    // `tls_context` is borrowed from the owning runtime and shared by every
    // pool of the alias; it is configured only when TLS is enabled.
    redis_pool(asio::io_context& io_context, const redis_config_storage& config, asio::ssl::context& tls_context, std::optional<timeout_duration> command_timeout, std::size_t pool_size, const worker_ref& worker_value);
    redis_pool(asio::io_context&, redis_config_storage&&, asio::ssl::context&, std::optional<timeout_duration>, std::size_t, const worker_ref&) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&&, asio::ssl::context&, std::optional<timeout_duration>, std::size_t, const worker_ref&) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&, asio::ssl::context&, std::optional<timeout_duration>, std::size_t, worker_ref&&) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&, asio::ssl::context&, std::optional<timeout_duration>, std::size_t, const worker_ref&&) = delete;
    ~redis_pool();

    redis_pool(const redis_pool&) = delete;
    redis_pool& operator=(const redis_pool&) = delete;

    task<void> connect();
    void close_now() noexcept;
    // `reply_timeout` starts when the command is written, so a server-side
    // block window is not consumed by slot acquisition or a lazy connect.
    // `io_metrics` is the request's counter block, or null outside a request.
    task<redis_value> execute_owned(redis_owned_command command, operation_options options = {}, std::optional<timeout_duration> reply_timeout = std::nullopt, request_io_metrics* io_metrics = nullptr, std::optional<operation_deadline> deadline = std::nullopt);
    task<redis_value> execute_owned(std::vector<std::string> args, operation_options options = {}, std::optional<timeout_duration> reply_timeout = std::nullopt, request_io_metrics* io_metrics = nullptr, std::optional<operation_deadline> deadline = std::nullopt);
    // The caller keeps `args` alive until the operation completes. `deadline`
    // is copied, so repeated commands share the operation's absolute bound.
    task<redis_value> execute_borrowed(redis_command_args_view args, operation_options options, operation_deadline deadline);
    task<std::vector<redis_value>> execute_pipeline(std::vector<redis_owned_command> commands, operation_options options, bool wrap_transaction = false);

private:
    // Every deadline belongs to a connection's transport; the slot pool only
    // leases slots and routes cancellation.
    struct slot_policy final {
        using owner_type = redis_pool;
        enum class deadline_kind_type : std::uint8_t { none };

        [[noreturn]] static void throw_acquire_failure(const redis_pool& pool, worker_wait_status status);
        [[noreturn]] static void throw_aborted(const redis_pool& pool, pool_slot_abort_reason reason);
        static void expire_slot(redis_pool&, std::size_t, deadline_kind_type) noexcept {}
        static void cancel_slot(redis_pool& pool, std::size_t slot) noexcept;
    };
    using slot_pool_type = slot_pool<slot_policy>;
    using connection_owner_type = std::unique_ptr<redis_connection>;

    template <typename result_type, typename command_source_type>
    task<result_type> execute_commands(command_source_type source, operation_options options, std::optional<timeout_duration> reply_timeout, std::optional<operation_deadline> deadline, request_io_metrics* io_metrics = nullptr, bool wrap_transaction = false);

    const redis_config_storage& config_;
    std::optional<timeout_duration> command_timeout_;
    client_wire_counters wire_counters_;
    slot_pool_type slots_;
    // Declared after the slot pool, whose worker every transport borrows.
    std::vector<connection_owner_type> connections_;
};

class redis_subscription_set;

// The connection behind one redis_subscription. Address-stable and
// worker-affine; the set links every live connection so shutdown and the
// subscription's stop token can abort it, and the connection unlinks itself
// when destroyed. Closing is terminal: a failed, aborted or cancelled
// connection never reconnects.
//
// While read() waits, subscription_ping_interval drives an idle probe: an
// interval without input sends PING unless a request written since the last
// input still awaits its reply, and a further interval without input closes
// the connection with reason timeout. The probe timer and every PING write
// belong to the read that started them and end before it returns.
class redis_subscription_connection final {
public:
    redis_subscription_connection(redis_subscription_set& owner, operation_options options);
    ~redis_subscription_connection();

    redis_subscription_connection(const redis_subscription_connection&) = delete;
    redis_subscription_connection& operator=(const redis_subscription_connection&) = delete;

    // Writes one subscription command, connecting first when needed. The
    // write is bound by the operation timeout and command_timeout, and waits
    // for a PING write of the idle probe that holds the transport's write lane.
    task<void> send(redis_owned_command command);
    // Reads the next pushed reply, consuming PING replies. Views of the
    // returned event borrow current_reply_ until the next read() releases it.
    // `operation` tightens the read deadline; its stop request closes the
    // connection and fails this read with cancelled.
    task<std::optional<redis_pubsub_event>> read(operation_options operation);
    void abort(client_abort_reason reason) noexcept;

    [[nodiscard]] const worker_ref& worker() const noexcept;
    [[nodiscard]] bool has_active_operations() const noexcept {
        return read_active_ || write_active_;
    }

private:
    friend class redis_subscription_set;

    using clock_type = std::chrono::steady_clock;

    void throw_if_closed() const;
    void fail() noexcept;
    // Idle probe of one read(); `ping_writes` is that read's scope and
    // `read_timeout` its overall deadline, both borrowed until
    // end_idle_probe().
    void begin_idle_probe(task_scope& ping_writes, const operation_deadline& read_timeout) noexcept;
    void end_idle_probe() noexcept;
    void arm_idle_probe(clock_type::time_point deadline) noexcept;
    void idle_probe_expired() noexcept;
    // Records the exception being handled for the read, then closes.
    void fail_idle_probe() noexcept;
    // Bounded by `timeout`: one interval, capped by the joining read's deadline.
    task<void> write_ping(operation_deadline timeout);
    static void cancel_read(void* connection) noexcept;

    redis_subscription_set& owner_;
    operation_options options_;
    redis_connection connection_;
    redis_value current_reply_;
    redis_subscription_connection* previous_{nullptr};
    redis_subscription_connection* next_{nullptr};
    std::uint64_t cancellation_id_{0};
    bool read_active_{false};
    bool write_active_{false};
    // The first subscribe() has authenticated the connection.
    bool ready_{false};
    bool closed_{false};
    // A PING write holds the transport's write lane; send() waits on
    // ping_written_ until it ends.
    bool ping_writing_{false};
    // PING replies still owed by the server, consumed by read() in wire order.
    std::size_t pending_pings_{0};
    // The read()'s scope its PING writes join and that read's deadline; null
    // while no read waits.
    task_scope* ping_writes_{nullptr};
    const operation_deadline* read_timeout_{nullptr};
    // Idle clock: the latest received bytes, the start of the waiting read,
    // the latest request written (a command or PING whose reply is owed when
    // it is newer than the input) and the start of the current probe.
    clock_type::time_point last_input_{};
    clock_type::time_point read_started_{};
    clock_type::time_point last_request_{};
    std::optional<clock_type::time_point> probe_started_{};
    // An internal probe failure no caller observed yet; the waiting read
    // rethrows it.
    std::exception_ptr probe_failure_{};
    worker_signal ping_written_;
    // Why the connection closed: none for a failed operation, timeout for an
    // unanswered idle probe.
    client_abort_reason closed_reason_{client_abort_reason::none};
    // Armed only while a read() waits and cancelled before it returns; its
    // completion addresses this connection.
    worker_timer_registration idle_probe_timer_;
    // Routes the stop request of the waiting read().
    operation_interrupt_source read_stops_;
    // Declared last: it is armed with the stop token once the connection is
    // linked and is released before any other member is destroyed.
    worker_cancellation_registration<worker_cancellation_target<redis_subscription_set>> cancellation_;
};

// The live Pub/Sub connections of one alias on one worker. Owned by
// redis_client_runtime next to the command pools; close_now() aborts every
// connection, and each connection is joined by the scope that owns its
// redis_subscription. Destroying the set with live connections terminates.
class redis_subscription_set final {
public:
    redis_subscription_set(asio::io_context& io_context, asio::ssl::context& tls_context, const redis_config_storage& config, const worker_ref& worker_value);
    redis_subscription_set(asio::io_context&, asio::ssl::context&, const redis_config_storage&, worker_ref&&) = delete;
    ~redis_subscription_set();

    redis_subscription_set(const redis_subscription_set&) = delete;
    redis_subscription_set& operator=(const redis_subscription_set&) = delete;

    using connection_owner_type = std::unique_ptr<redis_subscription_connection>;

    // A closed set opens connections that are already closing, so a late
    // subscription observes shutdown instead of connecting.
    [[nodiscard]] connection_owner_type open(operation_options options);
    void close_now() noexcept;

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }
    // worker_cancellation_target hook; runs on the worker.
    void cancel_operation_by_id(std::uint64_t cancellation_id) noexcept;

private:
    friend class redis_subscription_connection;

    void link(redis_subscription_connection& connection) noexcept;
    void unlink(redis_subscription_connection& connection) noexcept;

    asio::io_context& io_context_;
    asio::ssl::context& tls_context_;
    const redis_config_storage& config_;
    worker_ref worker_;
    client_wire_counters wire_counters_;
    std::shared_ptr<worker_cancellation_target<redis_subscription_set>> cancellation_target_;
    redis_subscription_connection* head_{nullptr};
    bool closing_{false};
};

// The single backend owner used by standalone clients and application registrations.
// Both pools and the Pub/Sub connections borrow this stable owner's normalized
// configuration, TLS context and worker.
class redis_client_runtime final {
public:
    redis_client_runtime(asio::io_context& io_context, const worker_ref& worker_value, redis_config_storage config);
    redis_client_runtime(asio::io_context&, worker_ref&&, redis_config_storage) = delete;
    ~redis_client_runtime();

    redis_client_runtime(const redis_client_runtime&) = delete;
    redis_client_runtime& operator=(const redis_client_runtime&) = delete;

    [[nodiscard]] task<void> connect();
    void close_now() noexcept;
    // An application-registry handle; `io_metrics` is the request's counter
    // block, or null outside a request.
    [[nodiscard]] redis_session session(::ruvia::operation_scope& scope, request_io_metrics* io_metrics);
    // A standalone client handle, which never belongs to a request.
    [[nodiscard]] redis_session session(::ruvia::operation_scope& scope, operation_options options);

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }
    [[nodiscard]] const redis_config_storage& config() const noexcept {
        return config_;
    }
    // The configuration of the runtime behind an active handle, for a
    // consumer that sizes its own use of that store (the DB query cache).
    [[nodiscard]] static const redis_config_storage& session_config(const redis_session& handle) noexcept {
        return handle.runtime_->config();
    }
    [[nodiscard]] redis_pool& general() noexcept {
        return general_;
    }
    [[nodiscard]] redis_pool& blocking() noexcept {
        return blocking_;
    }
    [[nodiscard]] redis_subscription_set& subscriptions() noexcept {
        return subscriptions_;
    }

private:
    redis_config_storage config_;
    worker_ref worker_;
    // One client TLS context per alias, borrowed by every connection of both
    // pools and every subscription; configured only when TLS is enabled.
    asio::ssl::context tls_context_;
    redis_pool general_;
    redis_pool blocking_;
    redis_subscription_set subscriptions_;
};

}  // namespace ruvia::detail

#ifdef RUVIA_ENABLE_REDIS

namespace ruvia::detail {

class redis_registry final {
public:
    redis_registry(asio::io_context& io_context, std::span<const redis_definition_type> redis, worker_ref worker_value);
    ~redis_registry();

    redis_registry(const redis_registry&) = delete;
    redis_registry& operator=(const redis_registry&) = delete;

    task<void> connect();
    void close_now() noexcept;

    [[nodiscard]] bool empty() const noexcept;
    // `io_metrics` is the request's counter block, or null outside a request.
    [[nodiscard]] redis_session get(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;
    [[nodiscard]] redis_session get(std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;

private:
    using entry_type = std::unique_ptr<redis_client_runtime>;

    std::vector<entry_type> runtimes_;
    named_capability_index alias_index_;
};

}  // namespace ruvia::detail

#endif
