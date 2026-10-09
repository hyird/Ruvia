#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <variant>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/web/rate_limit_rule.h"
#include "ruvia/web/server_config.h"

#include "http/cors_options.h"
#include "http/static_root_index.h"
#include "server/document_root_binding.h"
#include "server/trusted_proxies.h"

namespace ruvia {
class env;
}

namespace ruvia::detail {

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
struct connection_failure_sink final {
    connection_failure_callback_ref_type callback_;
    // Owned by the web_worker_runtime this sink was configured for; null before one
    // claims it. Counting here rather than at each reporting site keeps the
    // count and the callback from drifting apart as new sites are added.
    std::atomic<std::size_t>* counter_{nullptr};

    void invoke(std::string_view remote_address, std::exception_ptr exception) const noexcept {
        if (exception == nullptr) {
            return;
        }
        if (counter_ != nullptr) {
            counter_->fetch_add(1, std::memory_order_relaxed);
        }
        if (callback_) {
            callback_(connection_failure_record_access::make(remote_address, std::move(exception)));
            return;
        }
        ruvia::report_unhandled_failure("web connection", std::move(exception));
    }
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
    class document_root_type final {
        struct standalone_type final {
            const static_root* root_;
        };
        struct refreshing_type final {
            const static_root* root_;
            document_root_runtime_config config_;
            static_root_precompression_options precompression_;
        };

    public:
        document_root_type() noexcept = default;

        [[nodiscard]] static document_root_type standalone(const static_root& root) noexcept {
            return document_root_type(standalone_type{&root});
        }

        [[nodiscard]] static document_root_type refreshing(const static_root& root,
            document_root_runtime_config config = {},
            static_root_precompression_options precompression = {}) noexcept {
            return document_root_type(refreshing_type{&root, config, precompression});
        }

        [[nodiscard]] const static_root* root() const noexcept {
            if (const auto* standalone = std::get_if<standalone_type>(&state_)) {
                return standalone->root_;
            }
            if (const auto* refreshing = std::get_if<refreshing_type>(&state_)) {
                return refreshing->root_;
            }
            return nullptr;
        }

        [[nodiscard]] const document_root_runtime_config* refresh_options() const noexcept {
            const auto* refreshing = std::get_if<refreshing_type>(&state_);
            return refreshing == nullptr ? nullptr : &refreshing->config_;
        }

        [[nodiscard]] const static_root_precompression_options* precompression_options()
            const noexcept {
            const auto* refreshing = std::get_if<refreshing_type>(&state_);
            return refreshing == nullptr ? nullptr : &refreshing->precompression_;
        }

        // Publishes the next immutable snapshot without changing the ownership
        // state. Calling this on a non-refreshing root is an invariant breach.
        void publish(const static_root& root) noexcept {
            auto* refreshing = std::get_if<refreshing_type>(&state_);
            if (refreshing == nullptr) {
                std::terminate();
            }
            refreshing->root_ = &root;
        }

        [[nodiscard]] document_root_binding binding() const noexcept {
            if (const auto* standalone = std::get_if<standalone_type>(&state_)) {
                return document_root_binding::standalone(*standalone->root_);
            }
            if (const auto* refreshing = std::get_if<refreshing_type>(&state_)) {
                return document_root_binding::configured(*refreshing->root_);
            }
            return document_root_binding::none();
        }

    private:
        explicit document_root_type(standalone_type state_value) noexcept
            : state_(state_value) {}
        explicit document_root_type(refreshing_type state_value) noexcept
            : state_(state_value) {}

        std::variant<std::monostate, standalone_type, refreshing_type> state_;
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
