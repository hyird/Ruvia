#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <asio/fwd.hpp>
#include <asio/io_context.hpp>
#include <asio/ssl/context.hpp>

#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/web/redis/redis_session.h"
#include "ruvia/web/redis/redis_types.h"

#include "integration/operation_interrupt.h"
#include "redis/redis_config_storage.h"
#include "redis/redis_pool.h"

namespace ruvia::detail {

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
    redis_subscription_set(asio::io_context& io_context, asio::ssl::context& tls_context,
        const redis_config_storage& config, const worker_ref& worker_value);
    redis_subscription_set(asio::io_context&, asio::ssl::context&, const redis_config_storage&, worker_ref&&) = delete;
    ~redis_subscription_set();

    redis_subscription_set(const redis_subscription_set&) = delete;
    redis_subscription_set& operator=(const redis_subscription_set&) = delete;

    using connection_owner_type =
        std::unique_ptr<redis_subscription_connection>;

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

}  // namespace ruvia::detail

namespace ruvia::detail {

// The single backend owner used by standalone clients and application registrations.
// Both pools and the Pub/Sub connections borrow this stable owner's normalized
// configuration, TLS context and worker.
class redis_client_runtime final {
public:
    redis_client_runtime(asio::io_context& io_context, const worker_ref& worker_value,
        redis_config_storage config);
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
    [[nodiscard]] static const redis_config_storage& handle_config(const redis_session& handle) noexcept {
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
