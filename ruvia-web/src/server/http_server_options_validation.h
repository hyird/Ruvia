#pragma once
#include <span>
#include <vector>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http_ascii.h"

#include "http3/http3_capacity.h"
#include "http3/http3_qpack_config_validation.h"
#include "server/http_server_listener.h"
#include "server/http_server_options.h"

namespace ruvia::detail {

void validate_server_limits(const server_config& config);
inline constexpr std::size_t http3_peer_unidirectional_stream_allowance = 64;
inline constexpr std::size_t http3_post_goaway_request_allowance = 64;
inline constexpr std::size_t http3_server_push_allowance = http3_connection_config{}.max_remembered_pushes_;

[[nodiscard]] std::size_t http3_worker_tracked_stream_capacity(
    std::size_t max_requests_per_connection);

[[nodiscard]] std::size_t http3_transport_lifetime_stream_capacity(
    std::size_t max_requests_per_connection);

void validate_http3_server_limits(
    std::optional<std::size_t> max_connections, const http3_listen_config& config,
    std::optional<std::size_t> max_requests_per_connection, std::size_t worker_count);

void validate_http3_listen_config(const http3_listen_config& config);

void validate_http_server_listener(const http_server_listener_definition& listener_value);

class validated_http_server_configuration final {
public:
    validated_http_server_configuration(const validated_http_server_configuration&) = delete;
    validated_http_server_configuration& operator=(const validated_http_server_configuration&) = delete;
    validated_http_server_configuration(validated_http_server_configuration&&) = default;
    validated_http_server_configuration& operator=(validated_http_server_configuration&&) = default;

    [[nodiscard]] std::span<const http_server_listener_definition> listeners() const noexcept {
        return listeners_;
    }

    [[nodiscard]] const http_server_options& options() const noexcept {
        return options_;
    }

private:
    validated_http_server_configuration(
        std::span<const http_server_listener_definition> listeners, http_server_options&& options);

    friend validated_http_server_configuration validate_http_server_configuration(
        std::span<const http_server_listener_definition> listeners, http_server_options&& options);

    std::pmr::vector<http_server_listener_definition> listeners_;
    http_server_options options_;
};

[[nodiscard]] validated_http_server_configuration validate_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options);

}  // namespace ruvia::detail
