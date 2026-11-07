#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

#include "app/app_config_mutation.h"
#include "app/app_listener_options.h"
#include "app/env_state.h"
#include "server/http_server_options_validation.h"
#include "server/http_server_tls_identity.h"
#include "util/config_validation.h"

namespace ruvia {

application& application::load_dotenv(dotenv_options options) {
    return detail::mutate_stopped_app(
        *this, *state_, "cannot load dotenv while app is running", [&](detail::app_state& state_value) {
            detail::load_env_from_executable_directory(state_value.env_, options);
        });
}

application& application::load_dotenv(const std::filesystem::path& path, dotenv_options options) {
    return detail::mutate_stopped_app(*this, *state_, "cannot load dotenv while app is running",
        [&](detail::app_state& state_value) { detail::load_env_from_file(state_value.env_, path, options); });
}

namespace {

void append_listen_config(
    std::pmr::vector<detail::http_server_listener_definition>& replacement, const listen_config& config) {
    const auto address = detail::normalize_listen_address(config.address_.view());
    if (!config.http_.has_value() && !config.https_.has_value()) {
        throw std::invalid_argument("listen config must enable HTTP, HTTPS, or both");
    }
    detail::ensure_non_zero_optional_port(config.http_, "HTTP listen port must not be zero");
    detail::ensure_non_zero_optional_port(config.https_, "HTTPS listen port must not be zero");
    if (config.http_.has_value() && config.http_ == config.https_) {
        throw std::invalid_argument("HTTP and HTTPS listen ports must be different");
    }
    if (config.auto_https_redirect_ &&
        (!config.http_.has_value() || !config.https_.has_value())) {
        throw std::invalid_argument(
            "automatic HTTPS redirect requires both HTTP and HTTPS ports");
    }

    if (!config.https_.has_value() && detail::has_tls_configuration(config.tls_)) {
        throw std::invalid_argument("TLS config requires an HTTPS listen port");
    }
    std::optional<detail::http_server_listener_definition::http3_service> http3;
    if (config.http3_.has_value()) {
#if defined(RUVIA_ENABLE_HTTP3)
        detail::ensure_non_zero_optional_port(config.http3_, "HTTP/3 listen port must not be zero");
        if (!config.https_.has_value()) {
            throw std::invalid_argument("HTTP/3 requires an HTTPS listen port and its TLS config");
        }
        if (config.http3_alt_svc_max_age_.count() <= 0) {
            throw std::invalid_argument("HTTP/3 Alt-Svc max age must be greater than zero");
        }
        // The secret is generated per validated configuration.
        http3.emplace(detail::http_server_listener_definition::http3_service{
            .port_ = config.http3_->value(),
            .alt_svc_max_age_ = config.http3_alt_svc_max_age_,
            .early_data_ = config.http3_early_data_,
            .retry_ = config.http3_retry_,
            .secret_ = {}});
#else
        throw std::invalid_argument("HTTP/3 listen port requires a build with RUVIA_ENABLE_HTTP3");
#endif
    }

    const auto ensure_unique_endpoint = [&](const asio::ip::tcp::endpoint& endpoint) {
        for (const auto& existing : replacement) {
            if (existing.endpoint_ == endpoint) {
                throw std::invalid_argument("duplicate listen endpoint");
            }
        }
    };
    // UDP endpoints are a separate namespace from the TCP ones above.
    const auto ensure_unique_http3_endpoint = [&](std::uint16_t port) {
        for (const auto& existing : replacement) {
            if (existing.http3_.has_value() && existing.http3_->port_ == port &&
                existing.endpoint_.address() == address) {
                throw std::invalid_argument("duplicate HTTP/3 listen endpoint");
            }
        }
    };

    if (config.http_.has_value()) {
        const auto endpoint = asio::ip::tcp::endpoint(address, config.http_->value());
        ensure_unique_endpoint(endpoint);
        if (config.auto_https_redirect_) {
            replacement.emplace_back(endpoint,
                detail::http_server_listener_definition::redirect_http_to_https{config.https_->value()});
        } else {
            replacement.emplace_back(endpoint);
        }
    }
    if (config.https_.has_value()) {
        const auto endpoint = asio::ip::tcp::endpoint(address, config.https_->value());
        ensure_unique_endpoint(endpoint);
        if (http3.has_value()) {
            ensure_unique_http3_endpoint(http3->port_);
        }
        // Credentials load once, here; the definition keeps no file name or password.
        auto tls = detail::load_http_server_tls(config.tls_, replacement.get_allocator().resource());
        replacement.emplace_back(endpoint, std::move(tls), std::move(http3));
    }
}

class tls_password_span_cleanser final {
public:
    explicit tls_password_span_cleanser(std::span<const listen_config> configs) noexcept
        : configs_(configs) {}

    ~tls_password_span_cleanser() {
        for (const auto& config : configs_) {
            detail::tls_password_cleanser cleanser(const_cast<tls_config&>(config.tls_));
        }
    }

    tls_password_span_cleanser(const tls_password_span_cleanser&) = delete;
    tls_password_span_cleanser& operator=(const tls_password_span_cleanser&) = delete;

private:
    std::span<const listen_config> configs_;
};

}  // namespace

application& application::listen(listen_config config) {
    const detail::tls_password_cleanser password_cleanser(config.tls_);
    return detail::mutate_stopped_app(*this, *state_, "cannot change listeners while app is running",
        [&config](detail::app_state& state_value) {
            auto* const resource = process_resource();
            std::pmr::vector<detail::http_server_listener_definition> replacement(resource);
            replacement.reserve(config.http_.has_value() && config.https_.has_value() ? 2 : 1);
            append_listen_config(replacement, config);
            state_value.listeners_ = std::move(replacement);
        });
}

application& application::listen(std::span<const listen_config> configs) {
    const tls_password_span_cleanser password_cleanser(configs);
    return detail::mutate_stopped_app(*this, *state_, "cannot change listeners while app is running",
        [configs](detail::app_state& state_value) {
            if (configs.empty()) {
                throw std::invalid_argument("listen config list must not be empty");
            }
            auto* const resource = process_resource();
            std::pmr::vector<detail::http_server_listener_definition> replacement(resource);
            std::size_t endpoint_count = 0;
            for (const auto& config : configs) {
                endpoint_count += static_cast<std::size_t>(config.http_.has_value()) +
                                  static_cast<std::size_t>(config.https_.has_value());
            }
            replacement.reserve(endpoint_count);
            for (const auto& config : configs) {
                append_listen_config(replacement, config);
            }
            state_value.listeners_ = std::move(replacement);
        });
}

application& application::listen(std::initializer_list<listen_config> configs) {
    return listen(std::span<const listen_config>(configs.begin(), configs.size()));
}

application& application::server(server_config config) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change server config while app is running",
        [&config](detail::app_state& state_value) { state_value.options_.set_server(config); });
}

application& application::on_start(app_hook hook) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register on_start hook while app is running",
        [&hook](detail::app_state& state_value) {
            if (!hook) {
                throw std::invalid_argument("on_start hook must not be null");
            }
            state_value.on_start_hooks_.push_back(std::move(hook));
        });
}

application& application::on_start(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register on_start hook while app is running",
        [retired = std::pmr::vector<app_hook>(process_resource())](detail::app_state& state_value) mutable {
            // The cleared targets retire with this lambda, after the app lock.
            retired.swap(state_value.on_start_hooks_);
        });
}

application& application::on_stop(app_hook hook) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register on_stop hook while app is running",
        [&hook](detail::app_state& state_value) {
            if (!hook) {
                throw std::invalid_argument("on_stop hook must not be null");
            }
            state_value.on_stop_hooks_.push_back(std::move(hook));
        });
}

application& application::on_stop(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register on_stop hook while app is running",
        [retired = std::pmr::vector<app_hook>(process_resource())](detail::app_state& state_value) mutable {
            retired.swap(state_value.on_stop_hooks_);
        });
}

application& application::rate_limit(rate_limit_config config) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change the default rate limit per worker while app is running",
        [config](detail::app_state& state_value) { state_value.options_.set_rate_limit(config); });
}

application& application::rate_limit(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change the default rate limit per worker while app is running",
        [](detail::app_state& state_value) { state_value.options_.default_rate_limit_per_worker_.reset(); });
}

application& application::trusted_proxies(trusted_proxy_config config) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change trusted proxies while app is running",
        [config = std::move(config)](detail::app_state& state_value) {
            state_value.options_.set_trusted_proxies(config);
        });
}

application& application::trusted_proxies(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change trusted proxies while app is running", [](detail::app_state& state_value) {
            state_value.options_.trusted_proxies_ = detail::trusted_proxy_set(process_resource());
        });
}

application& application::on_access(access_log_callback callback_value) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register access-log hook while app is running",
        [callback_value = std::move(callback_value)](detail::app_state& state_value) mutable {
            // Captured old target retires with this lambda, after the app lock.
            std::swap(state_value.access_log_callback_, callback_value);
            state_value.options_.access_log_.callback_ = detail::callback_access::ref(state_value.access_log_callback_);
        });
}

application& application::on_connection_failure(connection_failure_callback callback_value) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register connection-failure hook while app is running",
        [callback_value = std::move(callback_value)](detail::app_state& state_value) mutable {
            std::swap(state_value.connection_failure_callback_, callback_value);
            state_value.options_.connection_failure_.callback_ =
                detail::callback_access::ref(state_value.connection_failure_callback_);
        });
}

}  // namespace ruvia
