#include <bit>
#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <utility>
#include <variant>

#include "app/app_config_mutation.h"
#include "app/app_listener_options.h"
#include "app/env_state.h"
#include "server/http_server_options_validation.h"

namespace ruvia {

namespace detail {
void apply_server_config(app_state& state_value, const server_config& config) {
    auto options = normalize_server_options(config, state_value.options_);
    for (const auto& listener : state_value.listeners_) {
        if (listener.http3_) {
            validate_http3_server_limits(config.max_connections_per_worker_,
                *listener.http3_, config.max_requests_per_connection_, config.worker_count_);
        }
    }
    state_value.worker_count_ = config.worker_count_;
    state_value.process_signal_handlers_ = config.process_signal_handlers_;
    state_value.options_ = std::move(options);
}

}  // namespace detail

application& application::load_dotenv(dotenv_options options) {
    return detail::mutate_stopped_app(
        *this, *state_, "cannot load dotenv while app is running", [&](detail::app_state& state_value) {
            (void)detail::load_env_from_executable_directory(state_value.env_, options);
        });
}

application& application::load_dotenv(const std::filesystem::path& path, dotenv_options options) {
    return detail::mutate_stopped_app(*this, *state_, "cannot load dotenv while app is running",
        [&](detail::app_state& state_value) { (void)detail::load_env_from_file(state_value.env_, path, options); });
}

application& application::listen(listen_config config) {
    return detail::mutate_stopped_app(*this, *state_, "cannot change listeners while app is running",
        [&config](detail::app_state& state_value) {
            const auto address = detail::normalize_listen_address(config.address_);
            if (!config.http_.has_value() && !config.https_.has_value()) {
                throw std::invalid_argument("listen config must enable HTTP, HTTPS, or both");
            }
            ruvia::ensure_non_zero_optional_port(config.http_, "HTTP listen port must not be zero");
            ruvia::ensure_non_zero_optional_port(config.https_, "HTTPS listen port must not be zero");
            if (config.http_.has_value() && config.http_ == config.https_) {
                throw std::invalid_argument("HTTP and HTTPS listen ports must be different");
            }
            if (config.auto_https_redirect_ &&
                (!config.http_.has_value() || !config.https_.has_value())) {
                throw std::invalid_argument(
                    "automatic HTTPS redirect requires both HTTP and HTTPS ports");
            }

            std::optional<http3_listen_config> effective_http3;
            switch (config.http3_.mode_) {
                case http3_mode::automatic:
                    if (config.https_.has_value()) {
                        effective_http3 = config.http3_;
                        effective_http3->mode_ = http3_mode::enabled;
                    }
                    break;
                case http3_mode::enabled:
                    if (!config.https_.has_value()) {
                        throw std::invalid_argument("HTTP/3 requires an HTTPS listen port");
                    }
                    effective_http3 = config.http3_;
                    break;
                case http3_mode::disabled:
                    break;
                default:
                    throw std::invalid_argument("HTTP/3 mode is invalid");
            }

            if (!config.https_.has_value() && detail::has_tls_configuration(config.tls_)) {
                throw std::invalid_argument("TLS config requires an HTTPS listen port");
            }
            if (effective_http3.has_value()) {
                detail::validate_http3_listen_config(*effective_http3);
                detail::validate_http3_server_limits(state_value.options_.max_connections_,
                    *effective_http3, state_value.options_.max_requests_per_connection_,
                    state_value.worker_count_);
            }

            auto* const resource = detail::app_resource();
            std::pmr::vector<detail::http_server_listener_definition> replacement(resource);
            replacement.reserve(config.http_.has_value() && config.https_.has_value() ? 2 : 1);
            if (config.http_.has_value()) {
                const auto endpoint = asio::ip::tcp::endpoint(address, *config.http_);
                if (config.auto_https_redirect_) {
                    replacement.emplace_back(endpoint,
                        detail::http_server_listener_definition::redirect_http_to_https_type{*config.https_});
                } else {
                    replacement.emplace_back(endpoint);
                }
            }
            if (config.https_.has_value()) {
                auto tls = detail::normalize_tls_options(config.tls_, resource);
                tls.alt_svc_ = detail::normalize_alt_svc_advertisement(config.alt_svc_,
                    effective_http3.has_value() ? config.https_ : std::nullopt, resource);
                replacement.emplace_back(asio::ip::tcp::endpoint(address, *config.https_),
                    std::move(tls), effective_http3);
            }
            state_value.listeners_ = std::move(replacement);
        });
}

application& application::server(server_config config) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change server config while app is running",
        [config](detail::app_state& state_value) { detail::apply_server_config(state_value, config); });
}

application& application::deadline(deadline_config config) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change the deadline while app is running", [config](detail::app_state& state_value) {
            ruvia::ensure_positive_duration(
                config.handler_, "handler deadline must be greater than zero");
            state_value.options_.deadline_ = config;
        });
}

application& application::deadline(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change the deadline while app is running",
        [](detail::app_state& state_value) { state_value.options_.deadline_.reset(); });
}

application& application::on_start(app_hook_type hook) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register on_start hook while app is running",
        [&hook](detail::app_state& state_value) { state_value.on_start_hooks_.push_back(std::move(hook)); });
}

application& application::on_stop(app_hook_type hook) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register on_stop hook while app is running",
        [&hook](detail::app_state& state_value) { state_value.on_stop_hooks_.push_back(std::move(hook)); });
}

application& application::get_rate_limit(rate_limit_config config) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change the default rate limit per worker while app is running",
        [config](detail::app_state& state_value) mutable {
            detail::validate_rate_limit_rule(config.rule_);
            if (!std::has_single_bit(config.capacity_per_worker_)) {
                throw std::invalid_argument(
                    "rate-limit capacity per worker must be a power of two");
            }
            state_value.options_.default_rate_limit_per_worker_ = config.rule_;
            state_value.options_.rate_limit_capacity_per_worker_ = config.capacity_per_worker_;
        });
}

application& application::get_rate_limit(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change the default rate limit per worker while app is running",
        [](detail::app_state& state_value) { state_value.options_.default_rate_limit_per_worker_.reset(); });
}

application& application::trusted_proxies(trusted_proxy_config config) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change trusted proxies while app is running",
        [config = std::move(config)](detail::app_state& state_value) {
            detail::trusted_proxy_set parsed(detail::app_resource());
            parsed.trust_x_forwarded_proto(config.trust_x_forwarded_proto_);
            for (const auto& cidr : config.cidrs_) {
                const auto block = detail::parse_trusted_proxy_block(cidr);
                if ((block.index() != 0)) {
                    // A typo here would silently trust nothing and leave every
                    // client identified as the proxy, so it fails startup instead.
                    throw std::invalid_argument(
                        "trusted proxy must be an IP address or CIDR block");
                }
                parsed.add(std::get<0>(block));
            }
            state_value.options_.trusted_proxies_ = std::move(parsed);
        });
}

application& application::trusted_proxies(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change trusted proxies while app is running", [](detail::app_state& state_value) {
            state_value.options_.trusted_proxies_ = detail::trusted_proxy_set(detail::app_resource());
        });
}

application& application::on_access(access_log_callback_type callback_value) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register access-log hook while app is running",
        [callback_value = std::move(callback_value)](detail::app_state& state_value) mutable {
            state_value.access_log_callback_ = std::move(callback_value);
            state_value.options_.access_log_.callback_ = detail::callback_access::ref(state_value.access_log_callback_);
        });
}

application& application::on_connection_failure(connection_failure_callback_type callback_value) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register connection-failure hook while app is running",
        [callback_value = std::move(callback_value)](detail::app_state& state_value) mutable {
            state_value.connection_failure_callback_ = std::move(callback_value);
            state_value.options_.connection_failure_.callback_ =
                detail::callback_access::ref(state_value.connection_failure_callback_);
        });
}

}  // namespace ruvia
