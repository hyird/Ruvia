#pragma once
#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <vector>

#include "ruvia/http/borrowed_text.h"

#include "server/http_server_listener.h"
#include "server/http_server_options.h"

namespace ruvia {
struct listen_config;
}

namespace ruvia::detail {

// Owns every inherited socket and cleanses every TLS secret, including when
// configuration validation or allocation fails.
[[nodiscard]] std::pmr::vector<http_server_listener_definition> load_server_listeners(
    std::span<const listen_config> configs);

// Rejects collisions in their separate TCP and HTTP/3 UDP endpoint namespaces.
void ensure_new_listener_endpoints(std::span<const http_server_listener_definition> existing,
    std::span<const http_server_listener_definition> added);

void validate_server_limits(const server_config& config);

// `listener` prepared for a run with `options`: a clone sharing its loaded
// credentials and inherited sockets, with a ticket key ring whose epochs
// start at `key_origin`, the run's origin for every TLS listener, and with
// HTTP/3 its connection ID secret. Throws std::invalid_argument for a
// listener the run cannot serve, including a connection or request_stream
// listener whose application callback `options` lacks.
[[nodiscard]] http_server_listener_definition validate_run_listener(const http_server_listener_definition& listener,
    const http_server_options& options, std::chrono::steady_clock::time_point key_origin,
    std::pmr::memory_resource* resource);

class validated_http_server_configuration final {
public:
    validated_http_server_configuration(const validated_http_server_configuration&) = delete;
    validated_http_server_configuration& operator=(const validated_http_server_configuration&) = delete;
    validated_http_server_configuration(validated_http_server_configuration&&) = default;
    validated_http_server_configuration& operator=(validated_http_server_configuration&&) = default;

    [[nodiscard]] std::span<const http_server_listener_definition> listeners() const& noexcept RUVIA_LIFETIMEBOUND {
        return listeners_;
    }
    std::span<const http_server_listener_definition> listeners() const&& = delete;

    [[nodiscard]] const http_server_options& options() const& noexcept RUVIA_LIFETIMEBOUND {
        return options_;
    }
    const http_server_options& options() const&& = delete;

    // Where the ticket key epochs of every TLS listener of the run start,
    // including listeners added while it runs.
    [[nodiscard]] std::chrono::steady_clock::time_point tls_key_origin() const noexcept {
        return tls_key_origin_;
    }

private:
    validated_http_server_configuration(
        std::span<const http_server_listener_definition> listeners, http_server_options&& options);

    friend validated_http_server_configuration validate_http_server_configuration(
        std::span<const http_server_listener_definition> listeners, http_server_options&& options);

    std::pmr::vector<http_server_listener_definition> listeners_;
    http_server_options options_;
    std::chrono::steady_clock::time_point tls_key_origin_;
};

[[nodiscard]] validated_http_server_configuration validate_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options);

}  // namespace ruvia::detail
