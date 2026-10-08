#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <variant>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/core/FailureReport.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/web/RateLimitRule.h"
#include "ruvia/web/server_config.h"

#include "http/CorsOptions.h"
#include "http/StaticRootIndex.h"
#include "server/DocumentRootBinding.h"
#include "server/TrustedProxies.h"

namespace ruvia {
class Env;
}

namespace ruvia::detail {

struct AccessLogSink final {
    AccessLogCallbackRef callback;

    void invoke(const AccessLogRecord& record) const noexcept {
        callback(record);
    }
};

struct ConnectionFailureRecordAccess final {
    [[nodiscard]] static ConnectionFailureRecord make(
        std::string_view remoteAddress, std::exception_ptr exception) noexcept {
        return ConnectionFailureRecord(remoteAddress, std::move(exception));
    }
};

// The connection level's failure outlet. An unset callback still reports: the
// failure goes to the shared last-resort reporter rather than being dropped
// with the connection that produced it.
struct ConnectionFailureSink final {
    ConnectionFailureCallbackRef callback;
    // Owned by the WebWorkerRuntime this sink was configured for; null before one
    // claims it. Counting here rather than at each reporting site keeps the
    // count and the callback from drifting apart as new sites are added.
    std::atomic<std::size_t>* counter{nullptr};

    void invoke(std::string_view remoteAddress, std::exception_ptr exception) const noexcept {
        if (exception == nullptr) {
            return;
        }
        if (counter != nullptr) {
            counter->fetch_add(1, std::memory_order_relaxed);
        }
        if (callback) {
            callback(ConnectionFailureRecordAccess::make(remoteAddress, std::move(exception)));
            return;
        }
        ruvia::reportUnhandledFailure("web connection", std::move(exception));
    }
};

struct WorkerFailureSink final {
    using Invoke = void (*)(void*, const std::exception_ptr&) noexcept;

    void* target{nullptr};
    Invoke invoke{nullptr};

    void notify(const std::exception_ptr& failure) const noexcept {
        if (invoke != nullptr) {
            invoke(target, failure);
        }
    }
};

// Fully normalized, worker-owned runtime options. Public callers configure App
// with server_config.h values; only the Web runtime constructs this state.
[[nodiscard]] const server_config& server_config_defaults() noexcept;

struct HttpServerOptions final {
    class DocumentRoot final {
        struct Standalone final {
            const StaticRoot* root;
        };
        struct Refreshing final {
            const StaticRoot* root;
            DocumentRootRuntimeConfig config;
            StaticRootPrecompressionOptions precompression;
        };

    public:
        DocumentRoot() noexcept = default;

        [[nodiscard]] static DocumentRoot standalone(const StaticRoot& root) noexcept {
            return DocumentRoot(Standalone{&root});
        }

        [[nodiscard]] static DocumentRoot refreshing(const StaticRoot& root,
            DocumentRootRuntimeConfig config = {},
            StaticRootPrecompressionOptions precompression = {}) noexcept {
            return DocumentRoot(Refreshing{&root, config, precompression});
        }

        [[nodiscard]] const StaticRoot* root() const noexcept {
            if (const auto* standalone = std::get_if<Standalone>(&state_)) {
                return standalone->root;
            }
            if (const auto* refreshing = std::get_if<Refreshing>(&state_)) {
                return refreshing->root;
            }
            return nullptr;
        }

        [[nodiscard]] const DocumentRootRuntimeConfig* refreshOptions() const noexcept {
            const auto* refreshing = std::get_if<Refreshing>(&state_);
            return refreshing == nullptr ? nullptr : &refreshing->config;
        }

        [[nodiscard]] const StaticRootPrecompressionOptions* precompressionOptions()
            const noexcept {
            const auto* refreshing = std::get_if<Refreshing>(&state_);
            return refreshing == nullptr ? nullptr : &refreshing->precompression;
        }

        // Publishes the next immutable snapshot without changing the ownership
        // state. Calling this on a non-refreshing root is an invariant breach.
        void publish(const StaticRoot& root) noexcept {
            auto* refreshing = std::get_if<Refreshing>(&state_);
            if (refreshing == nullptr) {
                std::terminate();
            }
            refreshing->root = &root;
        }

        [[nodiscard]] DocumentRootBinding binding() const noexcept {
            if (const auto* standalone = std::get_if<Standalone>(&state_)) {
                return DocumentRootBinding::standalone(*standalone->root);
            }
            if (const auto* refreshing = std::get_if<Refreshing>(&state_)) {
                return DocumentRootBinding::configured(*refreshing->root);
            }
            return DocumentRootBinding::none();
        }

    private:
        explicit DocumentRoot(Standalone state) noexcept
            : state_(state) {}
        explicit DocumentRoot(Refreshing state) noexcept
            : state_(state) {}

        std::variant<std::monostate, Standalone, Refreshing> state_;
    };

    // nginx-aligned inactivity timeouts. Absence disables a phase timeout;
    // max_requests_per_connection caps requests per reused connection.
    std::optional<std::chrono::milliseconds> idle_timeout{server_config_defaults().idle_timeout};
    std::chrono::milliseconds scanInterval{server_config_defaults().connection_scan_interval};
    // Capacity of the explicit cross-thread queue for this Web worker.
    std::size_t worker_queue_capacity{server_config_defaults().worker_queue_capacity};
    MemoryPoolConfig memoryConfig{server_config_defaults().memory_pool};
    HttpClientResultBudgetConfig http_client_result_budget{server_config_defaults().http_client_result_budget};
    std::optional<std::chrono::milliseconds> request_header_timeout{server_config_defaults().request_header_timeout};
    std::optional<std::chrono::milliseconds> request_body_timeout{server_config_defaults().request_body_timeout};
    // Absolute phase deadlines: progress does not renew these limits.
    std::optional<std::chrono::milliseconds> header_completion_timeout{server_config_defaults().header_completion_timeout};
    std::optional<std::chrono::milliseconds> body_completion_timeout{server_config_defaults().body_completion_timeout};
    std::optional<std::chrono::milliseconds> write_timeout{server_config_defaults().write_timeout};
    // Per worker. Defaults to a bounded cap so an unconfigured server cannot be
    // driven to FD/memory exhaustion by a connection flood; set std::nullopt to
    // opt back into unlimited. Excess accepted sockets are closed before TLS or
    // HTTP protocol detection, so admission never fabricates a response.
    std::optional<std::size_t> maxConnections{server_config_defaults().max_connections_per_worker};
    std::optional<std::size_t> max_requests_per_connection{server_config_defaults().max_requests_per_connection};
    // Buffered routes materialize body data; the same cap applies again after
    // Content-Encoding is decoded. This limit must be greater than 0.
    std::size_t max_buffered_body_bytes{server_config_defaults().max_buffered_body_bytes};
    // Live inbound buffer allocations across HTTP bodies and WebSocket sessions.
    // Includes capacity and reallocation overlap; the limits cannot be disabled.
    std::size_t max_inbound_buffer_bytes_per_worker{server_config_defaults().max_inbound_buffer_bytes_per_worker};
    std::size_t max_inbound_buffer_bytes_per_connection{server_config_defaults().max_inbound_buffer_bytes_per_connection};
    // Borrowed from the worker runtime, whose shutdown joins every user.
    std::pmr::memory_resource* inbound_buffer_pool{};
    // Stream routes are explicit; absence disables the stream body limit.
    std::optional<std::size_t> max_stream_body_bytes{server_config_defaults().max_stream_body_bytes};
    // WebSocket messages are assembled before delivery; this must be greater than 0.
    std::size_t max_web_socket_message_bytes{server_config_defaults().max_web_socket_message_bytes};
    // Presence enables the policy; absence bypasses it without retaining an
    // inactive configuration state.
    std::optional<CompressionConfig> compression{};
    std::optional<CorsOptions> cors{};
    DocumentRoot documentRoot{};
    // Peers whose forwarding headers may be believed. Empty by default, so an
    // unconfigured server treats every direct peer as the client.
    // Absent means no handler deadline anywhere, and nothing is armed.
    std::optional<DeadlineConfig> deadline{};
    TrustedProxySet trustedProxies{};
    AccessLogSink accessLog{};
    const Env* env{nullptr};
    // Process-wide, owned by App::run() and shared by every worker. Null only
    // when the app explicitly disabled the default pool; runBlocking() then
    // reports that state instead of blocking the worker.
    BlockingPool* blockingPool{nullptr};
    WorkerFailureSink failure{};
    ConnectionFailureSink connectionFailure{};
    std::optional<RateLimitRule> defaultRateLimitPerWorker{};
    std::size_t rateLimitCapacityPerWorker{kDefaultRateLimitCapacityPerWorker};
};

}  // namespace ruvia::detail
