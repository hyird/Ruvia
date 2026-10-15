#pragma once
#include <span>
#include <vector>

#include "server/http_server_listener.h"
#include "server/http_server_options.h"

namespace ruvia::detail {

void validate_server_limits(const server_config& config);

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
