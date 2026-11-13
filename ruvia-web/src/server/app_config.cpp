#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <openssl/crypto.h>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/move_only_function.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/worker_runtime.h"

#include "http/operation_interrupt.h"
#include "server/app_state.h"
#include "server/connection_lifecycle.h"
#include "server/http_server_options.h"
#include "server/route_table.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "db/db_pipeline.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "redis/redis_pool.h"
#endif

namespace ruvia {

application& application::load_dotenv(dotenv_options options) & {
    return detail::mutate_stopped_app(
        *this, *state_, "cannot load dotenv while app is running", [&](detail::app_state& state_value) {
            state_value.env_.load_dotenv(options);
        });
}

application& application::load_dotenv(const std::filesystem::path& path, dotenv_options options) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot load dotenv while app is running",
        [&](detail::app_state& state_value) { state_value.env_.load_dotenv(path, options); });
}

namespace {
void cleanse_secret(std::string& secret) noexcept {
    OPENSSL_cleanse(secret.data(), secret.size());
    secret.clear();
}

void cleanse_identity(tls_identity_config& identity) noexcept {
    cleanse_secret(identity.private_key_password_);
    cleanse_secret(identity.private_key_pem_);
    cleanse_secret(identity.certificate_chain_pem_);
}

[[nodiscard]] asio::ip::address normalize_listen_address(std::string_view address) {
    if (address.empty()) {
        throw std::invalid_argument("listen address must not be empty");
    }
    const auto normalized = ruvia::parse_ip_address(address);
    if (!normalized) {
        throw std::invalid_argument("listen address must be a numeric IP address");
    }
    return *normalized;
}

[[nodiscard]] bool has_tls_configuration(const tls_config& config) noexcept {
    const auto& identity = config.identity_;
    return !identity.certificate_chain_file_.empty() || !identity.private_key_file_.empty() ||
           !identity.certificate_chain_pem_.empty() || !identity.private_key_pem_.empty() ||
           !identity.private_key_password_.empty() || config.client_certificates_.ca_file_.has_value() ||
           config.client_certificates_.requirement_ != tls_client_certificate_requirement::optional ||
           !config.sni_.empty() || !config.local_addresses_.empty() ||
           config.certificate_store_ != nullptr || static_cast<bool>(config.select_certificate_) ||
           config.default_certificate_ != 0;
}

// listen() and replace_tls_identities() own their tls_config, the only copy
// of each private-key password and in-memory PEM text the application ever
// holds: credentials are loaded from it and every such secret is cleansed
// when the call returns or throws.
class tls_secret_cleanser final {
public:
    explicit tls_secret_cleanser(tls_config& config) noexcept
        : config_(config) {}
    ~tls_secret_cleanser() {
        cleanse_identity(config_.identity_);
        for (auto& sni : config_.sni_) {
            cleanse_identity(sni.identity_);
        }
        for (auto& local : config_.local_addresses_) {
            cleanse_identity(local.identity_);
        }
    }

    tls_secret_cleanser(const tls_secret_cleanser&) = delete;
    tls_secret_cleanser& operator=(const tls_secret_cleanser&) = delete;
    tls_secret_cleanser(tls_secret_cleanser&&) = delete;
    tls_secret_cleanser& operator=(tls_secret_cleanser&&) = delete;

private:
    tls_config& config_;
};

// Validates listen_config::dispatch_ and tls_.alpn_. For a connection-dispatch
// listener, returns the ALPN offer its HTTPS service makes, in preference
// order, as an RFC 7301 §3.1 ProtocolNameList without its length prefix.
[[nodiscard]] std::optional<std::string> connection_dispatch_alpn(const listen_config& config) {
    const auto& protocols = config.tls_.alpn_;
    switch (config.dispatch_.value_or(listener_dispatch::routes)) {
        case listener_dispatch::routes:
        case listener_dispatch::request_stream:
            if (!protocols.empty()) {
                throw std::invalid_argument("tls_config::alpn_ requires listener_dispatch::connection");
            }
            return std::nullopt;
        case listener_dispatch::connection:
            break;
        default:
            throw std::invalid_argument("listener dispatch is invalid");
    }
    if (config.http3_.has_value()) {
        throw std::invalid_argument("listener_dispatch::connection serves no HTTP/3 port");
    }
    if (config.auto_https_redirect_) {
        throw std::invalid_argument("listener_dispatch::connection cannot redirect HTTP to HTTPS");
    }
    if (!config.https_.has_value() && !protocols.empty()) {
        throw std::invalid_argument("TLS config requires an HTTPS listen port");
    }
    if (protocols.empty()) {
        return std::string("\x02h2\x08http/1.1", 12);
    }
    std::string encoded;
    for (const auto& protocol : protocols) {
        if (protocol.empty() || protocol.size() > 255) {
            throw std::invalid_argument("ALPN protocol names must be 1 to 255 bytes");
        }
        encoded.push_back(static_cast<char>(protocol.size()));
        encoded.append(protocol);
    }
    return encoded;
}

void append_listen_config(
    std::vector<detail::http_server_listener_definition>& replacement, const listen_config& config) {
    const auto address = normalize_listen_address(config.address_.view());
    if (config.ipv6_only_ && !address.is_v6()) {
        throw std::invalid_argument("IPV6_V6ONLY requires an IPv6 listen address");
    }
    if (!config.http_.has_value() && !config.https_.has_value()) {
        throw std::invalid_argument("listen config must enable HTTP, HTTPS, or both");
    }
    detail::ensure_non_zero_optional_port(config.http_, "HTTP listen port must not be zero");
    detail::ensure_non_zero_optional_port(config.https_, "HTTPS listen port must not be zero");
    const bool mixed = config.http_.has_value() && config.http_ == config.https_;
    if (config.auto_https_redirect_ &&
        (!config.http_.has_value() || !config.https_.has_value())) {
        throw std::invalid_argument(
            "automatic HTTPS redirect requires both HTTP and HTTPS ports");
    }

    if (!config.https_.has_value() && has_tls_configuration(config.tls_)) {
        throw std::invalid_argument("TLS config requires an HTTPS listen port");
    }
    if (config.proxy_protocol_ && config.http3_) {
        throw std::invalid_argument("PROXY protocol is not supported on HTTP/3 listeners");
    }
    const auto connection_alpn = connection_dispatch_alpn(config);
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

    if (config.http_.has_value() && !mixed) {
        const auto endpoint = asio::ip::tcp::endpoint(address, config.http_->value());
        ensure_unique_endpoint(endpoint);
        if (config.auto_https_redirect_) {
            replacement.emplace_back(endpoint,
                detail::http_server_listener_definition::redirect_http_to_https{config.https_->value()});
        } else {
            replacement.emplace_back(endpoint);
            replacement.back().request_stream_ = config.dispatch_ == listener_dispatch::request_stream;
        }
        if (connection_alpn.has_value()) {
            replacement.back().connection_.emplace(std::string_view{});
        }
        replacement.back().ipv6_only_ = config.ipv6_only_;
        replacement.back().proxy_protocol_ = config.proxy_protocol_;
    }
    if (config.https_.has_value()) {
        const auto endpoint = asio::ip::tcp::endpoint(address, config.https_->value());
        ensure_unique_endpoint(endpoint);
        if (http3.has_value()) {
            ensure_unique_http3_endpoint(http3->port_);
        }
        // Credentials load once, here; the definition keeps no identity file
        // name, in-memory PEM or password.
        auto tls = detail::load_http_server_tls(config.tls_);
        if (mixed) {
            tls.tcp_mode_ = detail::http_server_listener_definition::tls_tcp_mode::detect_plain_or_tls;
            if (config.auto_https_redirect_) {
                tls.plaintext_redirect_.emplace(
                    detail::http_server_listener_definition::redirect_http_to_https{config.https_->value()});
            }
        }
        replacement.emplace_back(endpoint, std::move(tls), std::move(http3));
        replacement.back().ipv6_only_ = config.ipv6_only_;
        replacement.back().proxy_protocol_ = config.proxy_protocol_;
        replacement.back().request_stream_ = config.dispatch_ == listener_dispatch::request_stream;
        if (connection_alpn.has_value()) {
            replacement.back().connection_.emplace(*connection_alpn);
        }
    }
}

class tls_secret_span_cleanser final {
public:
    explicit tls_secret_span_cleanser(std::span<const listen_config> configs) noexcept
        : configs_(configs) {}

    ~tls_secret_span_cleanser() {
        for (const auto& config : configs_) {
            tls_secret_cleanser cleanser(const_cast<tls_config&>(config.tls_));
        }
    }

    tls_secret_span_cleanser(const tls_secret_span_cleanser&) = delete;
    tls_secret_span_cleanser& operator=(const tls_secret_span_cleanser&) = delete;

private:
    std::span<const listen_config> configs_;
};

// Every listen_config::inherited_sockets_ handle of `configs`, in order.
template <typename visitor_type>
void visit_inherited_sockets(std::span<const listen_config> configs, const visitor_type& visit) {
    for (const auto& config : configs) {
        if (!config.inherited_sockets_.has_value()) {
            continue;
        }
        const auto& sockets = *config.inherited_sockets_;
        for (const auto* service : {&sockets.http_, &sockets.https_, &sockets.http3_}) {
            for (const auto socket : *service) {
                visit(socket);
            }
        }
    }
}

// A handle given twice would be served, and closed, twice.
void ensure_distinct_inherited_sockets(std::span<const listen_config> configs) {
    visit_inherited_sockets(configs, [configs](native_socket socket) {
        std::size_t occurrences = 0;
        visit_inherited_sockets(configs, [socket, &occurrences](native_socket other) {
            occurrences += other == socket ? 1 : 0;
        });
        if (occurrences != 1) {
            throw std::invalid_argument("a listener socket was handed over twice");
        }
    });
}

void validate_owned_inherited_sockets(std::span<const listen_config> configs) {
    ensure_distinct_inherited_sockets(configs);
    for (const auto& config : configs) {
        if (!config.inherited_sockets_) {
            continue;
        }
        const auto address = normalize_listen_address(config.address_.view());
        const auto& sockets = *config.inherited_sockets_;
        const auto tcp = [&](const auto& handles, const auto& port) {
            if (!handles.empty() && !port) {
                throw std::invalid_argument("inherited TCP sockets require their service");
            }
            for (const auto socket : handles) {
                detail::adopt_inherited_tcp_socket(socket, {address, port->value()},
                    "inherited TCP socket does not serve its endpoint", config.ipv6_only_);
            }
        };
        tcp(sockets.http_, config.http_);
        tcp(sockets.https_, config.https_);
        if (!sockets.http3_.empty() && !config.http3_) {
            throw std::invalid_argument("inherited UDP sockets require HTTP/3");
        }
        for (const auto socket : sockets.http3_) {
            detail::adopt_inherited_udp_socket(socket, {address, config.http3_->value()},
                "inherited UDP socket does not serve its endpoint", config.ipv6_only_);
        }
    }
}

[[nodiscard]] bool holds_socket(std::span<const native_socket> sockets, native_socket socket) noexcept {
    return std::ranges::find(sockets, socket) != sockets.end();
}

// A failed listen() or add_listener() closes every handle it was given that
// no definition it built owns -- those close with their definition -- each
// handle once. No run has claimed any of them yet.
void close_unowned_inherited_sockets(std::span<const listen_config> configs,
    std::span<const detail::http_server_listener_definition> built) noexcept {
    std::size_t position = 0;
    visit_inherited_sockets(configs, [&](native_socket socket) {
        const auto current = position++;
        std::size_t earlier = 0;
        bool repeated = false;
        visit_inherited_sockets(configs, [&](native_socket other) {
            repeated = repeated || (earlier++ < current && other == socket);
        });
        const bool owned = std::ranges::any_of(built, [socket](const auto& definition) {
            const auto& inherited = definition.inherited_sockets_;
            return inherited != nullptr && (holds_socket(inherited->tcp(), socket) || holds_socket(inherited->udp(), socket));
        });
        if (!repeated && !owned) {
            detail::close_native_socket(socket);
        }
    });
}

// Hands config.inherited_sockets_ to the definitions append_listen_config()
// built from it, once each socket proved to serve its service.
void adopt_inherited_sockets(std::span<detail::http_server_listener_definition> built, const listen_config& config) {
    if (!config.inherited_sockets_.has_value()) {
        return;
    }
    const auto& sockets = *config.inherited_sockets_;
    if (config.http_.has_value() && config.http_ == config.https_ && !sockets.http_.empty()) {
        throw std::invalid_argument("a mixed HTTP/HTTPS listener inherits its single TCP socket group in https_, not http_");
    }
    if ((!sockets.http_.empty() && !config.http_.has_value()) || (!sockets.https_.empty() && !config.https_.has_value()) ||
        (!sockets.http3_.empty() && !config.http3_.has_value())) {
        throw std::invalid_argument("inherited listener sockets require their service's listen port");
    }
#if defined(_WIN32)
    // The acceptor thread owns one socket per service.
    for (const auto* service : {&sockets.http_, &sockets.https_, &sockets.http3_}) {
        if (service->size() > 1) {
            throw std::invalid_argument("Windows adopts one listener socket per service");
        }
    }
#endif
    for (auto& definition : built) {
        const bool https = std::holds_alternative<detail::http_server_listener_definition::tls>(definition.transport_);
        const std::span<const native_socket> tcp = https ? sockets.https_ : sockets.http_;
        const auto udp = definition.http3_.has_value() ? std::span<const native_socket>(sockets.http3_)
                                                       : std::span<const native_socket>();
        if (tcp.empty() && udp.empty()) {
            continue;
        }
        for (const auto socket : tcp) {
            detail::adopt_inherited_tcp_socket(socket, definition.endpoint_,
                https ? "inherited HTTPS sockets must listen on the configured address and port"
                      : "inherited HTTP sockets must listen on the configured address and port",
                config.ipv6_only_);
        }
        if (!udp.empty()) {
            const asio::ip::udp::endpoint endpoint(definition.endpoint_.address(), definition.http3_->port_);
            for (const auto socket : udp) {
                detail::adopt_inherited_udp_socket(
                    socket, endpoint, "inherited HTTP/3 sockets must be UDP sockets bound to the configured address and port", config.ipv6_only_);
            }
        }
        definition.inherited_sockets_ = std::make_shared<detail::inherited_listener_sockets>(
            std::vector<native_socket>(tcp.begin(), tcp.end()),
            std::vector<native_socket>(udp.begin(), udp.end()));
    }
}

// The listener definitions `configs` describe, with their inherited sockets.
void append_listen_configs(
    std::vector<detail::http_server_listener_definition>& replacement, std::span<const listen_config> configs) {
    ensure_distinct_inherited_sockets(configs);
    std::size_t endpoint_count = 0;
    for (const auto& config : configs) {
        endpoint_count += config.http_.has_value() && config.http_ == config.https_
                              ? 1
                              : static_cast<std::size_t>(config.http_.has_value()) +
                                    static_cast<std::size_t>(config.https_.has_value());
    }
    replacement.reserve(replacement.size() + endpoint_count);
    for (const auto& config : configs) {
        const auto first = replacement.size();
        append_listen_config(replacement, config);
        adopt_inherited_sockets(std::span(replacement).subspan(first), config);
    }
}

// The caller holds the application lock. Awaited controls may originate on
// any worker, including one of this run's own workers.
[[nodiscard]] detail::app_runtime_graph& running_runtime(
    detail::app_state& state_value, const char* not_running_message) {
    if (!state_value.lifecycle_.running() || state_value.runtime_ == nullptr) {
        throw std::logic_error(not_running_message);
    }
    return *state_value.runtime_;
}

class active_app_control final {
public:
    active_app_control(std::shared_ptr<detail::app_state> state, const char* not_running)
        : state_(std::move(state)) {
        std::lock_guard lock(state_->mutex_);
        runtime_ = &running_runtime(*state_, not_running);
        ++state_->runtime_controls_;
    }
    ~active_app_control() {
        std::lock_guard lock(state_->mutex_);
        --state_->runtime_controls_;
        state_->lifecycle_changed_.notify_all();
    }
    active_app_control(const active_app_control&) = delete;
    active_app_control& operator=(const active_app_control&) = delete;
    [[nodiscard]] detail::app_runtime_graph& runtime() const noexcept {
        return *runtime_;
    }

private:
    std::shared_ptr<detail::app_state> state_;
    detail::app_runtime_graph* runtime_{};
};

struct owned_tls_config final {
    explicit owned_tls_config(tls_config config)
        : config_(std::move(config)) {}
    owned_tls_config(owned_tls_config&&) = default;
    ~owned_tls_config() {
        tls_secret_cleanser cleanser(config_);
    }
    tls_config config_;
};

// run_blocking can stop delivering before its callable finishes. A separate
// receipt joins actual execution/destruction, so no accepted preparation or
// exception escapes the runtime control, even when its caller is stopping.
template <typename function_type>
task<std::invoke_result_t<function_type&>> prepare_runtime_control(worker_ref caller, function_type function) {
    using result_type = std::invoke_result_t<function_type&>;
    detail::runtime_control_completion completion(caller);
    auto result = co_await run_blocking(caller, completion.make_command(
                                                    [function = std::move(function)](const auto& receipt) mutable -> result_type {
                                                        try {
                                                            auto prepared = function();
                                                            receipt.complete();
                                                            return prepared;
                                                        } catch (...) {
                                                            receipt.complete(std::current_exception());
                                                            throw;
                                                        }
                                                    }));
    co_await completion.wait();
    if (auto failure = completion.failure()) {
        std::rethrow_exception(failure);
    }
    co_return std::move(result).take_value();
}

// The stored definition of the listener serving `target`. The caller holds
// the application lock.
[[nodiscard]] std::size_t stored_listener_index(const detail::app_state& state_value, const asio::ip::tcp::endpoint& target) {
    for (std::size_t index = 0; index < state_value.listeners_.size(); ++index) {
        if (state_value.listeners_[index].endpoint_ == target) {
            return index;
        }
    }
    throw std::invalid_argument("no listener serves this endpoint");
}

// The slot every worker of the run serves `target` in. The caller holds the
// application lock, and the stored listeners name `target`.
[[nodiscard]] std::size_t runtime_listener_slot(
    const detail::app_runtime_graph& runtime, const asio::ip::tcp::endpoint& target) {
    for (std::size_t slot = 0; slot < runtime.listener_slots_.size(); ++slot) {
        if (runtime.listener_slots_[slot] == target) {
            return slot;
        }
    }
    throw std::logic_error("the running app serves no slot for a stored listener");
}

// The index of the HTTPS listener `target` names in the running app, whose
// identities `config` may replace. The caller holds the application lock.
[[nodiscard]] std::size_t find_replaceable_tls_listener(
    detail::app_state& state_value, const asio::ip::tcp::endpoint& target, const tls_config& config) {
    (void)running_runtime(state_value, "TLS identities can be replaced only while the app is running");
    const auto index = stored_listener_index(state_value, target);
    const auto* tls = std::get_if<detail::http_server_listener_definition::tls>(&state_value.listeners_[index].transport_);
    if (tls == nullptr) {
        throw std::invalid_argument("the listener on this endpoint does not serve HTTPS");
    }
    detail::ensure_same_http_server_tls_policy(*tls, config);
    return index;
}

using control_list = std::vector<move_only_function<void()>>;

// Builds, before anything is posted, one control per worker that runs
// `run(worker, worker_index)` on that worker's thread for `publication`.
template <typename run_type>
void make_worker_controls(control_list& controls, detail::app_runtime_graph& runtime,
    detail::runtime_control_completion& publication, const run_type& run) {
    controls.reserve(runtime.workers_.size());
    for (std::size_t index = 0; index < runtime.workers_.size(); ++index) {
        controls.emplace_back(publication.make_command(
            [run, worker = runtime.workers_[index].runtime_.get(), index](const auto& receipt) mutable {
                std::exception_ptr failure;
                try {
                    run(*worker, index);
                } catch (...) {
                    failure = std::current_exception();
                }
                receipt.complete(std::move(failure));
            }));
    }
}

// Posts the controls in worker order and then drops the unposted ones, whose
// receipts wait() would otherwise wait for. False when a worker refused one:
// it refuses only once it stops, and a stopping worker stops the whole app.
[[nodiscard]] bool post_worker_controls(detail::app_runtime_graph& runtime, control_list& controls) noexcept {
    bool posted = true;
    for (std::size_t index = 0; index < controls.size() && posted; ++index) {
        posted = runtime.workers_[index].runtime_->runtime().post_control(std::move(controls[index]));
    }
    controls.clear();
    return posted;
}

// A non-routed run materializes the same route plan and worker bindings as
// startup only when its first routed listener is requested. Every throwing
// step precedes adoption; owner controls replace the empty tables in place.
struct runtime_route_candidate final {
    detail::compiled_route_plan_ptr_type plan_;
    std::vector<detail::controller_store> controllers_;
    std::vector<std::unique_ptr<detail::route_table>> tables_;
    std::vector<std::unique_ptr<detail::rate_limiter>> rate_limiters_;
};

[[nodiscard]] runtime_route_candidate prepare_runtime_routes(
    const detail::app_state& state_value, std::size_t worker_count) {
    runtime_route_candidate result;
    const auto registrars = detail::seal_controller_registrars();
    auto& controllers = result.controllers_;
    auto& tables = result.tables_;
    auto& rate_limiters = result.rate_limiters_;
    controllers.reserve(worker_count);
    controllers.emplace_back(registrars);
    result.plan_ = detail::compile_route_plan(registrars, controllers.front(), state_value.configuration_);
    tables.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        if (index != 0) {
            controllers.emplace_back(registrars);
        }
        tables.push_back(std::make_unique<detail::route_table>(*result.plan_, controllers.back()));
    }
    if (tables.front()->has_route_rate_limit() && !state_value.options_.default_rate_limit_per_worker_.has_value()) {
        rate_limiters.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            rate_limiters.push_back(std::make_unique<detail::rate_limiter>(std::nullopt,
                detail::route_rate_limit_presence::present, state_value.options_.rate_limit_capacity_per_worker_));
        }
    }
    return result;
}

task<void> enable_runtime_routes(worker_ref caller, detail::app_runtime_graph& runtime,
    runtime_route_candidate& candidate) {
    detail::runtime_control_completion publication(caller);
    control_list controls;
    make_worker_controls(controls, runtime, publication,
        [&runtime, &candidate](detail::web_worker_runtime& worker, std::size_t index) noexcept {
            auto& controllers = candidate.controllers_;
            auto& tables = candidate.tables_;
            auto& rate_limiters = candidate.rate_limiters_;
            if (!rate_limiters.empty()) {
                worker.adopt_route_rate_limiter(*rate_limiters[index]);
            }
            std::swap(runtime.workers_[index].controllers_, controllers[index]);
            runtime.workers_[index].routes_->swap(*tables[index]);
        });
    // Retain both plans even if stopping makes some workers refuse adoption.
    // Once published, later additions reuse this run's compiled route ability,
    // including when binding the first routed listener subsequently fails.
    runtime.empty_route_plan_ = std::move(runtime.route_plan_);
    runtime.route_plan_ = std::move(candidate.plan_);
    runtime.routes_enabled_ = true;
    const bool posted = post_worker_controls(runtime, controls);
    co_await publication.wait();
    if (auto failure = publication.failure()) {
        std::rethrow_exception(failure);
    }
    if (!posted || publication.completed() != runtime.workers_.size()) {
        throw std::logic_error("the app began stopping before every worker adopted the routes");
    }
}

#if defined(_WIN32)
// Builds an acceptor control before any owner thread prepares new resources.
template <typename run_type>
[[nodiscard]] move_only_function<void()> make_acceptor_control(
    detail::windows_acceptor& acceptor, detail::runtime_control_completion& publication, run_type run) {
    return publication.make_command([&acceptor, run = std::move(run)](const auto& receipt) mutable {
        std::exception_ptr failure;
        try {
            run(acceptor);
        } catch (...) {
            failure = std::current_exception();
        }
        receipt.complete(std::move(failure));
    });
}

// Runs `run(acceptor)` on the acceptor thread for `publication`; false when
// the acceptor stops first.
template <typename run_type>
[[nodiscard]] bool post_acceptor_control(
    detail::windows_acceptor& acceptor, detail::runtime_control_completion& publication, run_type run) {
    return acceptor.post_control(make_acceptor_control(acceptor, publication, std::move(run)));
}
#endif

// The owner threads one control round of the run reaches: every worker, and
// on Windows the acceptor.
[[nodiscard]] std::size_t control_targets(const detail::app_runtime_graph& runtime) noexcept {
#if defined(_WIN32)
    return runtime.workers_.size() + 1;
#else
    return runtime.workers_.size();
#endif
}

}  // namespace

// add_listener() rejects an endpoint a running listener serves, as listen()
// rejects one its call names twice.
void detail::ensure_new_listener_endpoints(std::span<const http_server_listener_definition> existing,
    std::span<const http_server_listener_definition> added) {
    for (const auto& candidate : added) {
        for (const auto& listener : existing) {
            if (listener.endpoint_ == candidate.endpoint_) {
                throw std::invalid_argument("duplicate listen endpoint");
            }
            if (candidate.http3_.has_value() && listener.http3_.has_value() &&
                listener.http3_->port_ == candidate.http3_->port_ &&
                listener.endpoint_.address() == candidate.endpoint_.address()) {
                throw std::invalid_argument("duplicate HTTP/3 listen endpoint");
            }
        }
    }
}

std::vector<detail::http_server_listener_definition> detail::load_server_listeners(
    std::span<const listen_config> configs) {
    const tls_secret_span_cleanser secret_cleanser(configs);
    std::vector<http_server_listener_definition> listeners;
    try {
        if (configs.empty()) {
            throw std::invalid_argument("listen config list must not be empty");
        }
        append_listen_configs(listeners, configs);
    } catch (...) {
        close_unowned_inherited_sockets(configs, listeners);
        throw;
    }
    return listeners;
}

detail::owned_listen_configs::owned_listen_configs(listen_config config) {
    try {
        configs_.reserve(1);
        configs_.push_back(std::move(config));
        own_addresses();
        validate_owned_inherited_sockets(configs_);
    } catch (...) {
        close_unowned_inherited_sockets(configs_.empty() ? std::span<const listen_config>(&config, 1) : std::span<const listen_config>(configs_), {});
        const tls_secret_span_cleanser owned_secrets(configs_);
        const tls_secret_cleanser input_secrets(config.tls_);
        throw;
    }
}

detail::owned_listen_configs::owned_listen_configs(std::span<const listen_config> configs) {
    try {
        configs_.reserve(configs.size());
        for (const auto& config : configs) {
            configs_.push_back(config);
        }
        own_addresses();
        validate_owned_inherited_sockets(configs_);
    } catch (...) {
        close_unowned_inherited_sockets(configs, {});
        const tls_secret_span_cleanser owned_secrets(configs_);
        throw;
    }
}

void detail::owned_listen_configs::own_addresses() {
    addresses_.reserve(configs_.size());
    for (auto& config : configs_) {
        addresses_.emplace_back(config.address_.view());
        config.address_ = addresses_.back();
    }
}

detail::owned_listen_configs::~owned_listen_configs() {
    const tls_secret_span_cleanser secrets(configs_);
    if (inherited_owned_) {
        close_unowned_inherited_sockets(configs_, {});
    }
}

std::vector<detail::http_server_listener_definition> detail::owned_listen_configs::load() {
    inherited_owned_ = false;
    return load_server_listeners(configs_);
}

bool detail::owned_listen_configs::needs_tls_preparation() const noexcept {
    return std::ranges::any_of(configs_, [](const auto& config) { return config.https_.has_value(); });
}

application& application::listen(listen_config config) & {
    return listen(std::span<const listen_config>(&config, 1));
}

application& application::listen(std::span<const listen_config> configs) & {
    const tls_secret_span_cleanser secret_cleanser(configs);
    try {
        std::lock_guard lock(state_->mutex_);
        detail::ensure_app_not_running(state_->lifecycle_.active(), "cannot change listeners while app is running");
    } catch (...) {
        close_unowned_inherited_sockets(configs, {});
        throw;
    }
    auto replacement = detail::load_server_listeners(configs);
    // The replaced definitions retire with `replacement`, after the lock,
    // closing inherited sockets no run consumed.
    detail::mutate_stopped_app(*this, *state_, "cannot change listeners while app is running",
        [&replacement](detail::app_state& state_value) { state_value.listeners_.swap(replacement); });
    return *this;
}

application& application::listen(std::initializer_list<listen_config> configs) & {
    return listen(std::span<const listen_config>(configs.begin(), configs.size()));
}

namespace {
task<void> replace_app_tls_identities(std::shared_ptr<detail::app_state> state, worker_ref caller,
    asio::ip::tcp::endpoint target, owned_tls_config input) {
    std::shared_ptr<const detail::http_server_tls_identities> retired;
    const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
    active_app_control active(state, "TLS identities can be replaced only while the app is running");
    {
        std::lock_guard lock(state->mutex_);
        (void)find_replaceable_tls_listener(*state, target, input.config_);
    }
    auto identities = co_await prepare_runtime_control(caller, [input = std::move(input)]() mutable {
        return detail::load_http_server_tls_identities(input.config_);
    });
    detail::runtime_control_completion completion(caller);
    control_list controls;
    auto& runtime = active.runtime();
    std::size_t index = 0;
    bool posted = false;
    {
        std::lock_guard lock(state->mutex_);
        (void)running_runtime(*state, "TLS identities can be replaced only while the app is running");
        index = stored_listener_index(*state, target);
        const auto slot = runtime_listener_slot(runtime, target);
        make_worker_controls(controls, runtime, completion,
            [slot, identities](detail::web_worker_runtime& worker, std::size_t) {
                worker.adopt_tls_identities(slot, identities);
            });
        posted = post_worker_controls(runtime, controls);
    }
    co_await completion.wait();
    if (auto failure = completion.failure()) {
        std::rethrow_exception(failure);
    }
    {
        std::lock_guard lock(state->mutex_);
        if (!posted || completion.completed() != runtime.workers_.size() || !state->lifecycle_.running()) {
            throw std::logic_error("the app began stopping before every worker adopted the TLS identities");
        }
        auto& stored = std::get<detail::http_server_listener_definition::tls>(state->listeners_[index].transport_).identities_;
        retired = std::exchange(stored, std::move(identities));
    }
}
}  // namespace

task<void> application::replace_tls_identities(
    worker_ref caller, const listener_endpoint& endpoint, tls_config config) & {
    owned_tls_config input(std::move(config));
    const asio::ip::tcp::endpoint target(normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    return replace_app_tls_identities(state_, std::move(caller), target, std::move(input));
}

namespace {
task<bool> close_app_connection(
    std::shared_ptr<detail::app_state> state, worker_ref caller, std::uint64_t connection_id) {
    const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
    active_app_control active(state, "connections can be closed only while the app is running");
    auto& runtime = active.runtime();
    std::atomic<bool> closed{false};
    detail::runtime_control_completion completion(caller);
    control_list controls;
    make_worker_controls(controls, runtime, completion,
        [connection_id, &closed](detail::web_worker_runtime& worker, std::size_t) noexcept {
            if (worker.close_connection(connection_id)) {
                closed.store(true, std::memory_order_relaxed);
            }
        });
    const bool posted = post_worker_controls(runtime, controls);
    co_await completion.wait();
    if (auto failure = completion.failure()) {
        std::rethrow_exception(failure);
    }
    if (!posted || completion.completed() != runtime.workers_.size()) {
        throw std::logic_error("the app began stopping before every worker accepted the connection control");
    }
    co_return closed.load(std::memory_order_relaxed);
}
}  // namespace

task<bool> application::close_connection(worker_ref caller, std::uint64_t connection_id) & {
    return close_app_connection(state_, std::move(caller), connection_id);
}

namespace {
task<void> wait_app_connections(std::shared_ptr<detail::app_state> state, worker_ref caller,
    asio::ip::tcp::endpoint target, operation_options options) {
    detail::operation_steps steps(options);
    active_app_control active(state, "connections can be awaited only while the app is running");
    auto& runtime = active.runtime();
    std::vector<std::shared_ptr<detail::connection_count>> counts;
    std::vector<stop_token> stops;
    {
        const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
        std::lock_guard lock(state->mutex_);
        (void)running_runtime(*state, "connections can be awaited only while the app is running");
        counts.reserve(runtime.workers_.size());
        stops.reserve(runtime.workers_.size());
        for (const auto& worker : runtime.workers_) {
            counts.push_back(worker.runtime_->listener_connections(target));
            stops.push_back(worker.runtime_->cancellation());
        }
    }
    stop_group stopping(stops);
    for (const auto& count : counts) {
        co_await detail::connection_count::wait(count, caller, steps.next("wait_connections_closed"), stopping.token());
    }
}
}  // namespace

task<void> application::wait_connections_closed(worker_ref caller, const listener_endpoint& endpoint, operation_options options) & {
    const asio::ip::tcp::endpoint target(normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    return wait_app_connections(state_, std::move(caller), target, std::move(options));
}

namespace {
struct listener_addition_candidate final {
    std::vector<detail::http_server_listener_definition> added_;
    std::vector<detail::http_server_listener_definition> prepared_;
    std::vector<std::vector<detail::prepared_http_server_session>> sessions_;
    std::optional<runtime_route_candidate> routes_;
};

[[nodiscard]] std::exception_ptr control_round_failure(const detail::runtime_control_completion& completion,
    bool posted, std::size_t targets, const char* message) noexcept {
    if (auto failure = completion.failure()) {
        return failure;
    }
    if (!posted || completion.completed() != targets) {
        try {
            return std::make_exception_ptr(std::logic_error(message));
        } catch (...) {
            return std::current_exception();
        }
    }
    return {};
}

task<void> add_app_listener(
    std::shared_ptr<detail::app_state> state, worker_ref caller, detail::owned_listen_configs input,
    std::shared_ptr<detail::prepared_listener_state> transaction) {
    const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
    active_app_control active(state, "listeners can be added only while the app is running");
    auto& runtime = active.runtime();
    const auto worker_count = runtime.workers_.size();
    const auto key_origin = runtime.tls_key_origin_;
    const bool needs_routes = !runtime.routes_enabled_;
    const bool inexpensive = std::ranges::none_of(input.configs_, [needs_routes](const auto& config) {
        return config.https_.has_value() || (needs_routes && config.dispatch_ == listener_dispatch::routes);
    });
    auto prepare =
        [state, worker_count, key_origin, needs_routes, input = std::move(input)]() mutable {
            listener_addition_candidate result;
            result.added_ = input.load();
            result.prepared_.reserve(result.added_.size());
            for (const auto& definition : result.added_) {
                result.prepared_.push_back(detail::validate_run_listener(
                    definition, state->options_, key_origin));
            }
            result.sessions_.reserve(worker_count);
            for (std::size_t worker = 0; worker < worker_count; ++worker) {
                auto& sessions = result.sessions_.emplace_back();
                sessions.reserve(result.prepared_.size());
                for (const auto& definition : result.prepared_) {
                    if (std::holds_alternative<detail::http_server_listener_definition::tls>(definition.transport_)) {
                        sessions.push_back(detail::prepare_http_server_session_config(
                            definition, static_cast<bool>(state->options_.tls_client_hello_)));
                    } else {
                        sessions.emplace_back();
                    }
                }
            }
            if (needs_routes && std::ranges::any_of(result.added_, [](const auto& listener) {
                    return !listener.connection_.has_value() && !listener.request_stream_ &&
                           !std::holds_alternative<detail::http_server_listener_definition::redirect_http_to_https>(
                               listener.transport_);
                })) {
                result.routes_.emplace(prepare_runtime_routes(*state, worker_count));
            }
            return result;
        };
    // Pure plaintext additions need no file/PEM/route work and still work on
    // callers whose blocking pool is disabled.
    auto candidate = inexpensive ? prepare() : (co_await prepare_runtime_control(caller, std::move(prepare)));
    auto& added = candidate.added_;
    auto& prepared = candidate.prepared_;
    std::vector<std::optional<std::size_t>> http3_slots;
    std::size_t first_slot = 0;
#if defined(RUVIA_ENABLE_HTTP3)
    detail::http3_worker_directory* directory = nullptr;
#endif
    {
        std::lock_guard lock(state->mutex_);
        (void)running_runtime(*state, "listeners can be added only while the app is running");
        detail::ensure_new_listener_endpoints(state->listeners_, added);
        state->listeners_.reserve(state->listeners_.size() + added.size());
        http3_slots.reserve(added.size());
#if defined(RUVIA_ENABLE_HTTP3)
        if (runtime.http3_directory_ == nullptr &&
            std::ranges::any_of(prepared, [](const auto& definition) { return definition.http3_.has_value(); })) {
            auto created = std::make_unique<detail::http3_worker_directory>(runtime.workers_.size());
            for (std::size_t index = 0; index < runtime.workers_.size(); ++index) {
                created->bind_worker(index, runtime.workers_[index].runtime_->runtime().worker());
            }
            runtime.http3_directory_ = std::move(created);
        }
        directory = runtime.http3_directory_.get();
#endif
        first_slot = runtime.listener_slots_.size();
        runtime.listener_slots_.resize(first_slot + added.size());
        for (const auto& definition : prepared) {
            http3_slots.push_back(
                definition.http3_.has_value() ? std::optional<std::size_t>(runtime.http3_slot_count_++) : std::nullopt);
        }
    }
    if (candidate.routes_) {
        co_await enable_runtime_routes(caller, runtime, *candidate.routes_);
    }
    const auto discard = [first_slot, count = prepared.size()](auto& target, const std::shared_ptr<void>& receipt) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            target.discard_listener(first_slot + index, receipt);
        }
    };
    const auto open = [first_slot, count = prepared.size()](auto& target) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            target.open_listener(first_slot + index);
        }
    };
    detail::runtime_control_completion prepared_completion(caller);
    detail::runtime_control_completion discarded_completion(caller);
    detail::runtime_control_completion opened_completion(caller);
    control_list prepare_controls;
    control_list discard_controls;
    control_list open_controls;
    make_worker_controls(prepare_controls, runtime, prepared_completion,
        [&](detail::web_worker_runtime& worker, std::size_t worker_index) {
            for (std::size_t index = 0; index < prepared.size(); ++index) {
                worker.prepare_listener(first_slot + index, http3_slots[index], prepared[index],
                    std::move(candidate.sessions_[worker_index][index])
#if defined(RUVIA_ENABLE_HTTP3)
                        ,
                    directory
#endif
                );
            }
        });
    make_worker_controls(discard_controls, runtime, discarded_completion,
        [discard, receipt = std::make_shared<detail::runtime_control_completion::receipt>(discarded_completion.make_receipt())](detail::web_worker_runtime& worker, std::size_t) { discard(worker, receipt); });
    make_worker_controls(open_controls, runtime, opened_completion,
        [open](detail::web_worker_runtime& worker, std::size_t) { open(worker); });
#if defined(_WIN32)
    auto prepare_acceptor = make_acceptor_control(*runtime.acceptor_, prepared_completion,
        [&](detail::windows_acceptor& acceptor) {
            for (std::size_t index = 0; index < prepared.size(); ++index) {
                acceptor.prepare_listener(first_slot + index, http3_slots[index], prepared[index]
#if defined(RUVIA_ENABLE_HTTP3)
                    ,
                    directory
#endif
                );
            }
        });
    auto discard_acceptor = make_acceptor_control(*runtime.acceptor_, discarded_completion,
        [discard, receipt = std::make_shared<detail::runtime_control_completion::receipt>(discarded_completion.make_receipt())](detail::windows_acceptor& acceptor) { discard(acceptor, receipt); });
    detail::runtime_control_completion accepting_completion(caller);
    auto open_acceptor = make_acceptor_control(*runtime.acceptor_, accepting_completion,
        [open](detail::windows_acceptor& acceptor) { open(acceptor); });
#endif
    bool posted = post_worker_controls(runtime, prepare_controls);
#if defined(_WIN32)
    if (posted) {
        posted = runtime.acceptor_->post_control(std::move(prepare_acceptor));
    }
    prepare_acceptor = nullptr;
#endif
    co_await prepared_completion.wait();
    auto failure = control_round_failure(prepared_completion, posted, control_targets(runtime),
        "the app began stopping before every worker prepared the listeners");
    if (!failure) {
        try {
            co_await transaction->checkpoint(runtime.workers_.front().runtime_->cancellation());
        } catch (...) {
            failure = std::current_exception();
        }
    }
    if (!failure) {
        {
            std::lock_guard lock(state->mutex_);
            posted = state->lifecycle_.running() && post_worker_controls(runtime, open_controls);
        }
        open_controls.clear();
        co_await opened_completion.wait();
        failure = control_round_failure(opened_completion, posted, runtime.workers_.size(),
            "the app began stopping before every worker accepted the listeners");
#if defined(_WIN32)
        if (!failure) {
            {
                std::lock_guard lock(state->mutex_);
                posted = state->lifecycle_.running() && runtime.acceptor_->post_control(std::move(open_acceptor));
            }
            open_acceptor = nullptr;
            co_await accepting_completion.wait();
            failure = control_round_failure(accepting_completion, posted, 1,
                "the app began stopping before the acceptor accepted the listeners");
        }
#endif
    }
    if (!failure) {
        std::lock_guard lock(state->mutex_);
        if (!state->lifecycle_.running()) {
            try {
                failure = std::make_exception_ptr(
                    std::logic_error("the app began stopping before every worker accepted the listeners"));
            } catch (...) {
                failure = std::current_exception();
            }
        } else {
            for (std::size_t index = 0; index < added.size(); ++index) {
                runtime.listener_slots_[first_slot + index] = added[index].endpoint_;
                added[index].inherited_sockets_.reset();
                state->listeners_.push_back(std::move(added[index]));
            }
        }
    }
    if (failure) {
        (void)post_worker_controls(runtime, discard_controls);
#if defined(_WIN32)
        (void)runtime.acceptor_->post_control(std::move(discard_acceptor));
#endif
        co_await discarded_completion.wait();
        if (auto rollback_failure = discarded_completion.failure()) {
            report_unhandled_failure("app listener rollback", rollback_failure);
        }
#if defined(RUVIA_ENABLE_HTTP3) && !defined(_WIN32)
        for (const auto& slot : http3_slots) {
            if (slot) {
                directory->close_unclaimed_listener_sockets(*slot);
            }
        }
#endif
        std::rethrow_exception(failure);
    }
}
}  // namespace

task<prepared_listener> application::prepare_listener(worker_ref caller, listen_config config) & {
    detail::owned_listen_configs input(std::move(config));
    auto transaction = detail::prepared_listener_access::make(caller);
    return detail::prepared_listener_access::launch(transaction,
        add_app_listener(state_, std::move(caller), std::move(input), transaction));
}
task<prepared_listener> application::prepare_listeners(worker_ref caller, std::span<const listen_config> configs) & {
    detail::owned_listen_configs input(configs);
    auto transaction = detail::prepared_listener_access::make(caller);
    return detail::prepared_listener_access::launch(transaction,
        add_app_listener(state_, std::move(caller), std::move(input), transaction));
}
task<void> application::add_listener(worker_ref caller, listen_config config) & {
    return detail::prepared_listener_access::add(prepare_listener(std::move(caller), std::move(config)));
}

namespace {
task<void> remove_app_listener(
    std::shared_ptr<detail::app_state> state, worker_ref caller, asio::ip::tcp::endpoint target, listener_remove_options options) {
    std::optional<detail::http_server_listener_definition> retired;
    const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
    active_app_control active(state, "listeners can be removed only while the app is running");
    auto& runtime = active.runtime();
    std::size_t slot = 0;
    {
        std::lock_guard lock(state->mutex_);
        (void)stored_listener_index(*state, target);
        slot = runtime_listener_slot(runtime, target);
    }
#if defined(_WIN32)
    detail::runtime_control_completion accepting_completion(caller);
    auto accepting_lifetime = std::make_shared<detail::runtime_control_completion::receipt>(
        accepting_completion.make_receipt());
    const bool accepting_posted = post_acceptor_control(*runtime.acceptor_, accepting_completion,
        [slot, receipt = std::move(accepting_lifetime)](detail::windows_acceptor& acceptor) {
            acceptor.remove_listener(slot, receipt);
        });
    co_await accepting_completion.wait();
    if (auto failure = control_round_failure(accepting_completion, accepting_posted, 1,
            "the app began stopping before the acceptor removed the listener")) {
        std::rethrow_exception(failure);
    }
#endif
    detail::runtime_control_completion completion(caller);
    control_list controls;
    auto lifetime = std::make_shared<detail::runtime_control_completion::receipt>(completion.make_receipt());
    make_worker_controls(controls, runtime, completion,
        [slot, receipt = std::move(lifetime), options](detail::web_worker_runtime& worker, std::size_t) {
            worker.remove_listener(slot, receipt, options);
        });
    bool posted = false;
    {
        std::lock_guard lock(state->mutex_);
        posted = state->lifecycle_.running() && post_worker_controls(runtime, controls);
    }
    controls.clear();
    co_await completion.wait();
    if (auto failure = control_round_failure(completion, posted, runtime.workers_.size(),
            "the app began stopping before every worker removed the listener")) {
        std::rethrow_exception(failure);
    }
    {
        std::lock_guard lock(state->mutex_);
        if (!state->lifecycle_.running()) {
            throw std::logic_error("the app began stopping before every worker removed the listener");
        }
        const auto index = stored_listener_index(*state, target);
        retired.emplace(std::move(state->listeners_[index]));
        state->listeners_.erase(state->listeners_.begin() + static_cast<std::ptrdiff_t>(index));
        runtime.listener_slots_[slot].reset();
    }
}
}  // namespace

task<void> application::remove_listener(worker_ref caller, const listener_endpoint& endpoint, listener_remove_options options) & {
    const asio::ip::tcp::endpoint target(normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    return remove_app_listener(state_, std::move(caller), target, options);
}

#if !defined(_WIN32)
namespace {
task<listener_sockets> duplicate_app_listener_sockets(
    std::shared_ptr<detail::app_state> state, worker_ref caller, asio::ip::tcp::endpoint target) {
    const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
    active_app_control active(state, "listener sockets can be duplicated only while the app is running");
    auto& runtime = active.runtime();
    std::vector<detail::listener_socket_duplicates> duplicates;
    detail::runtime_control_completion completion(caller);
    control_list controls;
    bool https = false;
    bool http3 = false;
    {
        std::lock_guard lock(state->mutex_);
        const auto& stored = state->listeners_[stored_listener_index(*state, target)];
        https = std::holds_alternative<detail::http_server_listener_definition::tls>(stored.transport_);
        http3 = stored.http3_.has_value();
        const auto slot = runtime_listener_slot(runtime, target);
        duplicates.resize(runtime.workers_.size());
        make_worker_controls(controls, runtime, completion,
            [slot, &duplicates](detail::web_worker_runtime& worker, std::size_t index) {
                duplicates[index] = worker.duplicate_listener(slot);
            });
    }
    const bool posted = post_worker_controls(runtime, controls);
    co_await completion.wait();
    listener_sockets result;
    auto& tcp = https ? result.https_ : result.http_;
    try {
        if (auto failure = control_round_failure(completion, posted, duplicates.size(),
                "the app began stopping before every worker duplicated its sockets")) {
            std::rethrow_exception(failure);
        }
        tcp.reserve(duplicates.size());
        if (http3) {
            result.http3_.reserve(duplicates.size());
        }
    } catch (...) {
        for (const auto& duplicate : duplicates) {
            detail::close_native_socket(duplicate.tcp_);
            detail::close_native_socket(duplicate.udp_);
        }
        throw;
    }
    for (const auto& duplicate : duplicates) {
        tcp.push_back(duplicate.tcp_);
        if (http3) {
            result.http3_.push_back(duplicate.udp_);
        }
    }
    co_return result;
}
}  // namespace

task<listener_sockets> application::duplicate_listener_sockets(
    worker_ref caller, const listener_endpoint& endpoint) const& {
    const asio::ip::tcp::endpoint target(normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    return duplicate_app_listener_sockets(state_, std::move(caller), target);
}
#endif

application& application::server(server_config config) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change server config while app is running",
        [&config](detail::app_state& state_value) { state_value.options_.set_server(config); });
}

application& application::on_start(app_hook hook) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register on_start hook while app is running",
        [hook = detail::own_callback(std::move(hook))](detail::app_state& state_value) mutable {
            if (!hook) {
                throw std::invalid_argument("on_start hook must not be null");
            }
            state_value.on_start_hooks_.push_back(std::move(hook));
        });
}

application& application::on_start(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register on_start hook while app is running",
        [retired = std::vector<detail::callback_registration<app_hook>>{}](detail::app_state& state_value) mutable {
            // The cleared targets retire with this lambda, after the app lock.
            retired.swap(state_value.on_start_hooks_);
        });
}

application& application::on_stop(app_hook hook) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register on_stop hook while app is running",
        [hook = detail::own_callback(std::move(hook))](detail::app_state& state_value) mutable {
            if (!hook) {
                throw std::invalid_argument("on_stop hook must not be null");
            }
            state_value.on_stop_hooks_.push_back(std::move(hook));
        });
}

application& application::on_stop(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register on_stop hook while app is running",
        [retired = std::vector<detail::callback_registration<app_hook>>{}](detail::app_state& state_value) mutable {
            retired.swap(state_value.on_stop_hooks_);
        });
}

application& application::rate_limit(rate_limit_config config) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change the default rate limit per worker while app is running",
        [config](detail::app_state& state_value) { state_value.options_.set_rate_limit(config); });
}

application& application::rate_limit(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change the default rate limit per worker while app is running",
        [](detail::app_state& state_value) { state_value.options_.clear_rate_limit(); });
}

application& application::trusted_proxies(trusted_proxy_config config) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change trusted proxies while app is running",
        [config = std::move(config)](detail::app_state& state_value) {
            state_value.options_.set_trusted_proxies(config);
        });
}

application& application::on_access(access_log_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register access-log hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            // Captured old target retires with this lambda, after the app lock.
            std::swap(state_value.access_log_callback_, callback_value);
            state_value.options_.access_log_.callback_ = detail::callback_access::ref(state_value.access_log_callback_);
        });
}

application& application::on_connection_failure(connection_failure_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register connection-failure hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.connection_failure_callback_, callback_value);
            state_value.options_.connection_failure_.callback_ =
                detail::callback_access::ref(state_value.connection_failure_callback_);
        });
}

application& application::on_tls_client_hello(tls_client_hello_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register TLS ClientHello hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.tls_client_hello_callback_, callback_value);
            state_value.options_.tls_client_hello_ =
                detail::callback_access::ref(state_value.tls_client_hello_callback_);
        });
}

application& application::on_tls_client_hello(std::nullptr_t) & {
    return on_tls_client_hello(tls_client_hello_callback{});
}

application& application::on_http2_input(http2_input_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register HTTP/2 input hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.http2_input_callback_, callback_value);
            state_value.options_.http2_input_ = detail::callback_access::ref(state_value.http2_input_callback_);
        });
}

application& application::on_http2_input(std::nullptr_t) & {
    return on_http2_input(http2_input_callback{});
}

#if defined(RUVIA_ENABLE_HTTP2)
application& application::on_http2_frame(http2_frame_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register HTTP/2 frame hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.http2_frame_callback_, callback_value);
            state_value.options_.http2_frame_ = detail::callback_access::ref(state_value.http2_frame_callback_);
        });
}

application& application::on_http2_frame(std::nullptr_t) & {
    return on_http2_frame(http2_frame_callback{});
}

application& application::on_http2_headers(http2_headers_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register HTTP/2 headers hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.http2_headers_callback_, callback_value);
            state_value.options_.http2_headers_ = detail::callback_access::ref(state_value.http2_headers_callback_);
        });
}

application& application::on_http2_headers(std::nullptr_t) & {
    return on_http2_headers(http2_headers_callback{});
}
#endif
#if defined(RUVIA_ENABLE_HTTP3)
application& application::on_http3_frame(http3_frame_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register HTTP/3 frame hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.http3_frame_callback_, callback_value);
            state_value.options_.http3_frame_ = detail::callback_access::ref(state_value.http3_frame_callback_);
        });
}

application& application::on_http3_frame(std::nullptr_t) & {
    return on_http3_frame(http3_frame_callback{});
}

application& application::on_http3_field(http3_field_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register HTTP/3 field hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.http3_field_callback_, callback_value);
            state_value.options_.http3_field_ = detail::callback_access::ref(state_value.http3_field_callback_);
        });
}

application& application::on_http3_field(std::nullptr_t) & {
    return on_http3_field(http3_field_callback{});
}
#endif

application& application::on_connection(connection_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register connection handler while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.connection_callback_, callback_value);
            state_value.options_.connection_ = detail::callback_access::ref(state_value.connection_callback_);
        });
}

application& application::on_connection(std::nullptr_t) & {
    return on_connection(connection_callback{});
}

application& application::on_connection_admission(connection_admission_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register connection admission hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.connection_admission_callback_, callback_value);
            state_value.options_.connection_admission_ = detail::callback_access::ref(state_value.connection_admission_callback_);
        });
}

application& application::on_connection_admission(std::nullptr_t) & {
    return on_connection_admission(connection_admission_callback{});
}

application& application::on_connection_open(connection_open_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register connection open hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.connection_open_callback_, callback_value);
            state_value.options_.connection_open_ = detail::callback_access::ref(state_value.connection_open_callback_);
        });
}

application& application::on_connection_open(std::nullptr_t) & {
    return on_connection_open(connection_open_callback{});
}

application& application::on_connection_close(connection_close_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register connection close hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.connection_close_callback_, callback_value);
            state_value.options_.connection_close_ = detail::callback_access::ref(state_value.connection_close_callback_);
        });
}

application& application::on_connection_close(std::nullptr_t) & {
    return on_connection_close(connection_close_callback{});
}

application& application::on_request_stream(request_stream_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register request stream handler while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.request_stream_callback_, callback_value);
            state_value.options_.request_stream_ = detail::callback_access::ref(state_value.request_stream_callback_);
        });
}

application& application::on_request_stream(std::nullptr_t) & {
    return on_request_stream(request_stream_callback{});
}

#ifdef RUVIA_ENABLE_DATABASE
application& application::database(db_registration_config config) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure database while app is running", [&](detail::app_state& state_value) {
            detail::validate_capability_alias(config.alias_.view(), "database alias must not be empty");
            detail::db_definition replacement{std::string(config.alias_.view()),
                detail::db_config_storage(config.config_)};
            if (config.query_cache_) {
#ifdef RUVIA_ENABLE_REDIS
                replacement.query_cache_.emplace(*config.query_cache_);
#else
                throw std::invalid_argument("database query cache requires Redis support");
#endif
            }
            detail::upsert_named_capability_definition(state_value.databases_, std::move(replacement));
        });
}

application& application::database(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure database while app is running",
        [](detail::app_state& state_value) { state_value.databases_.clear(); });
}
#endif

#ifdef RUVIA_ENABLE_REDIS
application& application::redis(redis_registration_config config) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure redis while app is running",
        [&](detail::app_state& state_value) {
            detail::upsert_named_capability_definition(
                state_value.redis_, config.alias_.view(), config.config_, "redis alias must not be empty");
        });
}

application& application::redis(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure redis while app is running",
        [](detail::app_state& state_value) { state_value.redis_.clear(); });
}
#endif

application& application::client(http_client_config config) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure HTTP client while app is running", [&](detail::app_state& state_value) {
            detail::upsert_named_capability_definition(state_value.http_clients_, config.alias_.view(), config,
                "HTTP client alias must not be empty");
        });
}

application& application::client(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure HTTP client while app is running",
        [](detail::app_state& state_value) { state_value.http_clients_.clear(); });
}

}  // namespace ruvia
