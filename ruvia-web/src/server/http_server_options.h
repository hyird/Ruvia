#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/web/rate_limit_rule.h"
#include "ruvia/web/server_config.h"

#include "http/cors_options.h"
#include "server/document_root_binding.h"
#include "server/trusted_proxies.h"

namespace ruvia {
class env;
}

namespace ruvia::detail {

class document_root_publisher;

struct access_log_sink final {
    access_log_callback_ref_type callback_;

    void invoke(const access_log_record& record) const noexcept {
        callback_(record);
    }
};

struct connection_failure_record_access final {
    [[nodiscard]] static connection_failure_record make(
        std::string_view remote_address, std::exception_ptr exception) noexcept {
        return connection_failure_record(remote_address, std::move(exception));
    }
};

// The connection level's failure outlet. An unset callback still reports: the
// failure goes to the shared last-resort reporter rather than being dropped
// with the connection that produced it.
//
// A connection that ends because the peer reset or closed it, or because this
// server cancelled its I/O (stop, drain, or a timeout that closes the socket),
// is a normal end of connection rather than a failure: browsers close SSE
// tabs with a reset, and drain aborts pending reads. Such endings are neither
// reported nor counted, so the sink stays reserved for real faults. Only an
// inbound_transport_error -- thrown by I/O on the connection's own transport,
// possibly propagated through a handler -- qualifies; any other exception is
// reported whatever error code it carries.
struct connection_failure_sink final {
    connection_failure_callback_ref_type callback_;
    // Owned by the web_worker_runtime this sink was configured for; null before one
    // claims it. Counting here rather than at each reporting site keeps the
    // count and the callback from drifting apart as new sites are added.
    std::atomic<std::size_t>* counter_{nullptr};

    void invoke(std::string_view remote_address, std::exception_ptr exception) const noexcept;
};

struct worker_failure_sink final {
    using invoke_type = void (*)(void*, const std::exception_ptr&) noexcept;

    void* target_{nullptr};
    invoke_type invoke_{nullptr};

    void notify(const std::exception_ptr& failure) const noexcept {
        if (invoke_ != nullptr) {
            invoke_(target_, failure);
        }
    }
};

// Fully normalized, worker-owned runtime options. Public callers configure application
// with server_config.h values; only the Web runtime constructs this state.
[[nodiscard]] const server_config& server_config_defaults() noexcept;

struct http_server_options final {
    // The application's document root: one app-scoped publisher shared by
    // every worker, plus this worker's current generation. Validated
    // configuration carries only the publisher; each worker's
    // static_root_runtime publishes its own generation before serving.
    class document_root_type final {
    public:
        document_root_type() noexcept = default;

        [[nodiscard]] static document_root_type refreshing(document_root_publisher& publisher) noexcept {
            return document_root_type(&publisher);
        }

        [[nodiscard]] document_root_publisher* publisher() const noexcept {
            return publisher_;
        }

        // Points new request bindings at the worker's adopted generation. Only
        // the owning worker calls this; earlier bindings keep their own lease.
        void publish(document_root_generation& generation) noexcept {
            if (publisher_ == nullptr) {
                std::terminate();
            }
            current_ = &generation;
        }

        [[nodiscard]] document_root_binding binding() const noexcept {
            return current_ == nullptr ? document_root_binding::none()
                                       : document_root_binding::configured(*current_);
        }

    private:
        explicit document_root_type(document_root_publisher* publisher) noexcept
            : publisher_(publisher) {}

        document_root_publisher* publisher_{nullptr};
        document_root_generation* current_{nullptr};
    };

    // nginx-aligned inactivity timeouts. Absence disables a phase timeout;
    // max_requests_per_connection caps requests per reused connection.
    std::optional<std::chrono::milliseconds> idle_timeout_{server_config_defaults().idle_timeout_};
    std::chrono::milliseconds scan_interval_{server_config_defaults().connection_scan_interval_};
    // Capacity of the explicit cross-thread queue for this Web worker.
    std::size_t worker_queue_capacity_{server_config_defaults().worker_queue_capacity_};
    memory_pool_config memory_config_{server_config_defaults().memory_pool_};
    http_client_result_budget_config http_client_result_budget_{server_config_defaults().http_client_result_budget_};
    std::optional<std::chrono::milliseconds> request_header_timeout_{server_config_defaults().request_header_timeout_};
    std::optional<std::chrono::milliseconds> request_body_timeout_{server_config_defaults().request_body_timeout_};
    // Absolute phase deadlines: progress does not renew these limits.
    std::optional<std::chrono::milliseconds> header_completion_timeout_{server_config_defaults().header_completion_timeout_};
    std::optional<std::chrono::milliseconds> body_completion_timeout_{server_config_defaults().body_completion_timeout_};
    std::optional<std::chrono::milliseconds> write_timeout_{server_config_defaults().write_timeout_};
    // Per worker. Defaults to a bounded cap so an unconfigured server cannot be
    // driven to FD/memory exhaustion by a connection flood; set std::nullopt to
    // opt back into unlimited. Excess accepted sockets are closed before TLS or
    // HTTP protocol detection, so admission never fabricates a response.
    std::optional<std::size_t> max_connections_{server_config_defaults().max_connections_per_worker_};
    std::optional<std::size_t> max_requests_per_connection_{server_config_defaults().max_requests_per_connection_};
    // Buffered routes materialize body data; the same cap applies again after
    // Content-Encoding is decoded. This limit must be greater than 0.
    std::size_t max_buffered_body_bytes_{server_config_defaults().max_buffered_body_bytes_};
    // Live inbound buffer allocations across HTTP bodies and websocket sessions.
    // Includes capacity and reallocation overlap; the limits cannot be disabled.
    std::size_t max_inbound_buffer_bytes_per_worker_{server_config_defaults().max_inbound_buffer_bytes_per_worker_};
    std::size_t max_inbound_buffer_bytes_per_connection_{server_config_defaults().max_inbound_buffer_bytes_per_connection_};
    // Borrowed from the worker runtime, whose shutdown joins every user.
    std::pmr::memory_resource* inbound_buffer_pool_{};
    // Stream routes are explicit; absence disables the stream body limit.
    std::optional<std::size_t> max_stream_body_bytes_{server_config_defaults().max_stream_body_bytes_};
    // websocket messages are assembled before delivery; this must be greater than 0.
    std::size_t max_websocket_message_bytes_{server_config_defaults().max_websocket_message_bytes_};
    // Presence enables the policy; absence bypasses it without retaining an
    // inactive configuration state.
    std::optional<compression_config> compression_{};
    std::optional<cors_options> cors_{};
    document_root_type document_root_{};
    // Peers whose forwarding headers may be believed. Empty by default, so an
    // unconfigured server treats every direct peer as the client.
    // Absent means no handler deadline anywhere, and nothing is armed.
    std::optional<deadline_config> deadline_{};
    trusted_proxy_set trusted_proxies_{};
    access_log_sink access_log_{};
    const env* env_{nullptr};
    // Process-wide, owned by application::run() and shared by every worker. Null only
    // when the app explicitly disabled the default pool; run_blocking() then
    // reports that state instead of blocking the worker.
    blocking_pool* blocking_pool_{nullptr};
    worker_failure_sink failure_{};
    connection_failure_sink connection_failure_{};
    std::optional<rate_limit_rule> default_rate_limit_per_worker_{};
    std::size_t rate_limit_capacity_per_worker_{default_rate_limit_capacity_per_worker};
};

}  // namespace ruvia::detail
