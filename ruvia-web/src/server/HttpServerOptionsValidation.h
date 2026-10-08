#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/core/ConfigValidation.h"
#include "ruvia/http/Http3Connection.h"
#include "ruvia/http/HttpAscii.h"

#include "http3/Http3QpackConfigValidation.h"
#include "http3/http3_capacity.h"
#include "server/HttpServerListener.h"
#include "server/HttpServerOptions.h"
#include "tls/TlsHost.h"

namespace ruvia::detail {

void validateHttpServerTlsIdentity(
    const HttpServerListenerDefinition::TlsIdentity& identity);

void validateDocumentRootRuntimeConfig(const HttpServerOptions& options);

void validate_worker_queue_capacity(std::size_t capacity);

void validate_server_limits(const HttpServerOptions& options);

[[nodiscard]] HttpServerOptions normalize_server_options(const server_config& config, HttpServerOptions options);

void validateHttpServerOptions(const HttpServerOptions& options);

void validateHttpServerTlsOptions(const HttpServerListenerDefinition::Tls& tls);

inline constexpr std::size_t kHttp3PeerUnidirectionalStreamAllowance = 64;
inline constexpr std::size_t kHttp3PostGoawayRequestAllowance = 64;
inline constexpr std::size_t kHttp3ServerPushAllowance = Http3ConnectionConfig{}.maxRememberedPushes;

[[nodiscard]] std::size_t http3WorkerTrackedStreamCapacity(
    std::size_t max_requests_per_connection);

[[nodiscard]] std::size_t http3TransportLifetimeStreamCapacity(
    std::size_t max_requests_per_connection);

void validateHttp3ServerLimits(
    std::optional<std::size_t> maxConnections, const Http3ListenConfig& config,
    std::optional<std::size_t> max_requests_per_connection, std::size_t worker_count);

void validate_http3_listen_config(const Http3ListenConfig& config);

void validateHttpServerListener(const HttpServerListenerDefinition& listener);

class ValidatedHttpServerConfiguration final {
public:
    ValidatedHttpServerConfiguration(const ValidatedHttpServerConfiguration&) = delete;
    ValidatedHttpServerConfiguration& operator=(const ValidatedHttpServerConfiguration&) = delete;
    ValidatedHttpServerConfiguration(ValidatedHttpServerConfiguration&&) = default;
    ValidatedHttpServerConfiguration& operator=(ValidatedHttpServerConfiguration&&) = default;

    [[nodiscard]] std::span<const HttpServerListenerDefinition> listeners() const noexcept {
        return listeners_;
    }

    [[nodiscard]] const HttpServerOptions& options() const noexcept {
        return options_;
    }

private:
    ValidatedHttpServerConfiguration(
        std::span<const HttpServerListenerDefinition> listeners, HttpServerOptions&& options);

    friend ValidatedHttpServerConfiguration validateHttpServerConfiguration(
        std::span<const HttpServerListenerDefinition> listeners, HttpServerOptions&& options);

    std::pmr::vector<HttpServerListenerDefinition> listeners_;
    HttpServerOptions options_;
};

[[nodiscard]] ValidatedHttpServerConfiguration validateHttpServerConfiguration(
    std::span<const HttpServerListenerDefinition> listeners, HttpServerOptions&& options);

}  // namespace ruvia::detail
