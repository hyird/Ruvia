#include <algorithm>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <openssl/crypto.h>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/move_only_function.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/worker_runtime.h"

#include "app/app_runtime_graph.h"
#include "app/app_state.h"
#include "app/env_state.h"
#include "integration/named_capability.h"
#include "router/router_impl.h"
#include "server/http_server_options_validation.h"
#include "server/http_server_tls_identity.h"
#include "util/config_validation.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "db/db_config_storage.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "redis/redis_config_storage.h"
#endif

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
void cleanse_secret(std::pmr::string& secret) noexcept {
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
           !config.sni_.empty() || !config.local_addresses_.empty();
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
    switch (config.dispatch_) {
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
    std::pmr::vector<detail::http_server_listener_definition>& replacement, const listen_config& config) {
    const auto address = normalize_listen_address(config.address_.view());
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

    if (!config.https_.has_value() && has_tls_configuration(config.tls_)) {
        throw std::invalid_argument("TLS config requires an HTTPS listen port");
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

    if (config.http_.has_value()) {
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
            replacement.back().connection_.emplace(std::string_view{}, replacement.get_allocator().resource());
        }
    }
    if (config.https_.has_value()) {
        const auto endpoint = asio::ip::tcp::endpoint(address, config.https_->value());
        ensure_unique_endpoint(endpoint);
        if (http3.has_value()) {
            ensure_unique_http3_endpoint(http3->port_);
        }
        // Credentials load once, here; the definition keeps no identity file
        // name, in-memory PEM or password.
        auto tls = detail::load_http_server_tls(config.tls_, replacement.get_allocator().resource());
        replacement.emplace_back(endpoint, std::move(tls), std::move(http3));
        replacement.back().request_stream_ = config.dispatch_ == listener_dispatch::request_stream;
        if (connection_alpn.has_value()) {
            replacement.back().connection_.emplace(*connection_alpn, replacement.get_allocator().resource());
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
                      : "inherited HTTP sockets must listen on the configured address and port");
        }
        if (!udp.empty()) {
            const asio::ip::udp::endpoint endpoint(definition.endpoint_.address(), definition.http3_->port_);
            for (const auto socket : udp) {
                detail::adopt_inherited_udp_socket(
                    socket, endpoint, "inherited HTTP/3 sockets must be UDP sockets bound to the configured address and port");
            }
        }
        auto* resource = process_resource();
        definition.inherited_sockets_ = std::allocate_shared<detail::inherited_listener_sockets>(
            std::pmr::polymorphic_allocator<detail::inherited_listener_sockets>(resource),
            std::pmr::vector<native_socket>(tcp.begin(), tcp.end(), resource),
            std::pmr::vector<native_socket>(udp.begin(), udp.end(), resource));
    }
}

// The listener definitions `configs` describe, with their inherited sockets.
void append_listen_configs(
    std::pmr::vector<detail::http_server_listener_definition>& replacement, std::span<const listen_config> configs) {
    ensure_distinct_inherited_sockets(configs);
    std::size_t endpoint_count = 0;
    for (const auto& config : configs) {
        endpoint_count +=
            static_cast<std::size_t>(config.http_.has_value()) + static_cast<std::size_t>(config.https_.has_value());
    }
    replacement.reserve(replacement.size() + endpoint_count);
    for (const auto& config : configs) {
        const auto first = replacement.size();
        append_listen_config(replacement, config);
        adopt_inherited_sockets(std::span(replacement).subspan(first), config);
    }
}

// The running app's runtime, for a control that posts to every worker and
// waits, which a worker waiting on itself never would. The caller holds the
// application lock.
[[nodiscard]] detail::app_runtime_graph& running_runtime(
    detail::app_state& state_value, const char* not_running_message, const char* worker_thread_message) {
    if (!state_value.lifecycle_.running() || state_value.runtime_ == nullptr) {
        throw std::logic_error(not_running_message);
    }
    for (auto& worker : state_value.runtime_->workers_) {
        if (worker.runtime_->runtime().handle().is_current()) {
            throw std::logic_error(worker_thread_message);
        }
    }
    return *state_value.runtime_;
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
    (void)running_runtime(state_value, "TLS identities can be replaced only while the app is running",
        "TLS identities cannot be replaced from a business worker thread");
    const auto index = stored_listener_index(state_value, target);
    const auto* tls = std::get_if<detail::http_server_listener_definition::tls>(&state_value.listeners_[index].transport_);
    if (tls == nullptr) {
        throw std::invalid_argument("the listener on this endpoint does not serve HTTPS");
    }
    detail::ensure_same_http_server_tls_policy(*tls, config);
    return index;
}

// Lets a runtime control API wait for the controls it posted to the run's
// owner threads. Each control holds a copy of receipt() and drops it once
// done, or unrun when destroyed first; dropping the last copy wakes wait(),
// so the caller never waits on a control that cannot run. A control that
// ran reports through complete() before dropping its copy. Destruction waits
// as well: controls borrow what their caller declared before this.
class runtime_control_publication final {
public:
    runtime_control_publication()
        : state_(std::allocate_shared<state_type>(std::pmr::polymorphic_allocator<state_type>(process_resource()))),
          receipt_(
              nullptr,
              [state = state_](void*) noexcept {
                  const std::lock_guard lock(state->mutex_);
                  state->released_ = true;
                  state->changed_.notify_all();
              },
              std::pmr::polymorphic_allocator<std::byte>(process_resource())) {}
    ~runtime_control_publication() {
        wait();
    }

    runtime_control_publication(const runtime_control_publication&) = delete;
    runtime_control_publication& operator=(const runtime_control_publication&) = delete;

    [[nodiscard]] std::shared_ptr<void> receipt() const noexcept {
        return receipt_;
    }

    // An owner thread, from a control that ran: `failure` is what it threw.
    void complete(std::exception_ptr failure) noexcept {
        const std::lock_guard lock(state_->mutex_);
        ++state_->completed_;
        if (state_->failure_ == nullptr) {
            state_->failure_ = std::move(failure);
        }
    }

    // Drops this side's copy, then waits until every control dropped its own.
    void wait() noexcept {
        receipt_.reset();
        std::unique_lock lock(state_->mutex_);
        state_->changed_.wait(lock, [this] { return state_->released_; });
    }

    // After wait(): how many controls ran, and the first failure among them.
    [[nodiscard]] std::size_t completed() const noexcept {
        return state_->completed_;
    }
    [[nodiscard]] std::exception_ptr failure() const noexcept {
        return state_->failure_;
    }

private:
    struct state_type final {
        std::mutex mutex_;
        std::condition_variable changed_;
        bool released_{false};
        std::size_t completed_{0};
        std::exception_ptr failure_;
    };

    std::shared_ptr<state_type> state_;
    std::shared_ptr<void> receipt_;
};

using control_list = std::pmr::vector<move_only_function<void()>>;

// Builds, before anything is posted, one control per worker that runs
// `run(worker, worker_index)` on that worker's thread for `publication`.
template <typename run_type>
void make_worker_controls(control_list& controls, detail::app_runtime_graph& runtime,
    runtime_control_publication& publication, const run_type& run) {
    controls.reserve(runtime.workers_.size());
    for (std::size_t index = 0; index < runtime.workers_.size(); ++index) {
        controls.emplace_back([&publication, run, worker = runtime.workers_[index].runtime_.get(), index,
                                  receipt = publication.receipt()]() mutable {
            std::exception_ptr failure;
            try {
                run(*worker, index);
            } catch (...) {
                failure = std::current_exception();
            }
            publication.complete(std::move(failure));
            receipt.reset();
        });
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
void enable_runtime_routes(detail::app_runtime_graph& runtime, const detail::app_state& state_value) {
    if (runtime.routes_enabled_) {
        return;
    }
    const auto registrars = detail::seal_controller_registrars();
    std::pmr::vector<detail::controller_store> controllers(process_resource());
    controllers.reserve(runtime.workers_.size());
    controllers.emplace_back(registrars);
    auto plan = detail::compile_route_plan(registrars, controllers.front(), state_value.configuration_);
    std::pmr::vector<std::unique_ptr<detail::route_table, pmr_object_deleter<detail::route_table>>> tables(process_resource());
    tables.reserve(runtime.workers_.size());
    for (std::size_t index = 0; index < runtime.workers_.size(); ++index) {
        if (index != 0) {
            controllers.emplace_back(registrars);
        }
        tables.push_back(make_pmr_object<detail::route_table>(process_resource(), *plan, controllers.back()));
    }
    std::pmr::vector<std::unique_ptr<detail::rate_limiter, pmr_object_deleter<detail::rate_limiter>>> rate_limiters(process_resource());
    if (tables.front()->has_route_rate_limit() && !state_value.options_.default_rate_limit_per_worker_.has_value()) {
        rate_limiters.reserve(runtime.workers_.size());
        for (std::size_t index = 0; index < runtime.workers_.size(); ++index) {
            rate_limiters.push_back(make_pmr_object<detail::rate_limiter>(process_resource(), std::nullopt,
                detail::route_rate_limit_presence::present, state_value.options_.rate_limit_capacity_per_worker_));
        }
    }
    runtime_control_publication publication;
    control_list controls(process_resource());
    make_worker_controls(controls, runtime, publication,
        [&runtime, &controllers, &tables, &rate_limiters](detail::web_worker_runtime& worker, std::size_t index) noexcept {
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
    runtime.route_plan_ = std::move(plan);
    runtime.routes_enabled_ = true;
    const bool posted = post_worker_controls(runtime, controls);
    publication.wait();
    if (!posted || publication.completed() != runtime.workers_.size()) {
        throw std::logic_error("the app began stopping before every worker adopted the routes");
    }
}

#if defined(_WIN32)
// Builds an acceptor control before any owner thread prepares new resources.
template <typename run_type>
[[nodiscard]] move_only_function<void()> make_acceptor_control(
    detail::windows_acceptor& acceptor, runtime_control_publication& publication, run_type run) {
    return [&publication, &acceptor, run = std::move(run), receipt = publication.receipt()]() mutable {
        std::exception_ptr failure;
        try {
            run(acceptor);
        } catch (...) {
            failure = std::current_exception();
        }
        publication.complete(std::move(failure));
        receipt.reset();
    };
}

// Runs `run(acceptor)` on the acceptor thread for `publication`; false when
// the acceptor stops first.
template <typename run_type>
[[nodiscard]] bool post_acceptor_control(
    detail::windows_acceptor& acceptor, runtime_control_publication& publication, run_type run) {
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

std::pmr::vector<detail::http_server_listener_definition> detail::load_server_listeners(
    std::span<const listen_config> configs) {
    const tls_secret_span_cleanser secret_cleanser(configs);
    std::pmr::vector<http_server_listener_definition> listeners(process_resource());
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

application& application::listen(listen_config config) {
    return listen(std::span<const listen_config>(&config, 1));
}

application& application::listen(std::span<const listen_config> configs) {
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

application& application::listen(std::initializer_list<listen_config> configs) {
    return listen(std::span<const listen_config>(configs.begin(), configs.size()));
}

void application::replace_tls_identities(const listener_endpoint& endpoint, tls_config config) {
    const tls_secret_cleanser secret_cleanser(config);
    const asio::ip::tcp::endpoint target(normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    {
        std::lock_guard lock(state_->mutex_);
        (void)find_replaceable_tls_listener(*state_, target, config);
    }
    const std::lock_guard serialized(state_->runtime_control_mutex_);
    // Credentials load on this thread, outside the application lock, before
    // anything changes.
    auto identities = detail::load_http_server_tls_identities(config, process_resource());
    // Destroyed, with the set it replaced, after the lock.
    std::shared_ptr<const detail::http_server_tls_identities> retired;
    bool published = false;
    {
        runtime_control_publication publication;
        control_list controls(process_resource());
        std::lock_guard lock(state_->mutex_);
        // The app may have stopped, or begun stopping, while credentials loaded.
        const auto index = find_replaceable_tls_listener(*state_, target, config);
        auto& runtime = *state_->runtime_;
        const auto slot = runtime_listener_slot(runtime, target);
        make_worker_controls(controls, runtime, publication,
            [slot, identities](detail::web_worker_runtime& worker, std::size_t) {
                worker.adopt_tls_identities(slot, identities);
            });
        published = post_worker_controls(runtime, controls);
        if (published) {
            // Later run()s serve the set every worker now adopts.
            auto& stored =
                std::get<detail::http_server_listener_definition::tls>(state_->listeners_[index].transport_).identities_;
            retired = std::exchange(stored, std::move(identities));
        }
    }
    if (!published) {
        throw std::logic_error("the app began stopping before every worker adopted the TLS identities");
    }
}

void application::add_listener(listen_config config) {
    const tls_secret_cleanser secret_cleanser(config.tls_);
    const std::span<const listen_config> configs(&config, 1);
    try {
        std::lock_guard lock(state_->mutex_);
        (void)running_runtime(*state_, "listeners can be added only while the app is running",
            "listeners cannot be added from a business worker thread");
    } catch (...) {
        close_unowned_inherited_sockets(configs, {});
        throw;
    }
    // As listen() stores them; the run clones them below.
    auto added = detail::load_server_listeners(configs);
    // The definitions own the inherited sockets from here on. The runtime
    // outlives this lock: run() retires it only while holding it.
    const std::lock_guard serialized(state_->runtime_control_mutex_);
    std::pmr::vector<detail::http_server_listener_definition> prepared(process_resource());
    std::pmr::vector<std::optional<std::size_t>> http3_slots(process_resource());
    detail::app_runtime_graph* runtime = nullptr;
    std::size_t first_slot = 0;
#if defined(RUVIA_ENABLE_HTTP3)
    detail::http3_worker_directory* directory = nullptr;
#endif
    {
        std::lock_guard lock(state_->mutex_);
        runtime = &running_runtime(*state_, "listeners can be added only while the app is running",
            "listeners cannot be added from a business worker thread");
        detail::ensure_new_listener_endpoints(state_->listeners_, added);
        prepared.reserve(added.size());
        // Every allocation for publication is completed before owner threads
        // acquire sockets. Committing the stored definitions then cannot grow.
        state_->listeners_.reserve(state_->listeners_.size() + added.size());
        http3_slots.reserve(added.size());
        for (const auto& definition : added) {
            // Server options are frozen while the app runs, so they describe this run.
            prepared.push_back(
                detail::validate_run_listener(definition, state_->options_, runtime->tls_key_origin_, process_resource()));
        }
#if defined(RUVIA_ENABLE_HTTP3)
        if (runtime->http3_directory_ == nullptr &&
            std::ranges::any_of(prepared, [](const auto& definition) { return definition.http3_.has_value(); })) {
            auto created = make_pmr_object<detail::http3_worker_directory>(process_resource(), runtime->workers_.size());
            for (std::size_t index = 0; index < runtime->workers_.size(); ++index) {
                created->bind_worker(index, runtime->workers_[index].runtime_->runtime().handle());
            }
            // Declared before the workers, so it outlives them like one run() made.
            runtime->http3_directory_ = std::move(created);
        }
        directory = runtime->http3_directory_.get();
#endif
        // Numbered now, whatever the outcome: a slot is never reused.
        first_slot = runtime->listener_slots_.size();
        runtime->listener_slots_.resize(first_slot + added.size());
        for (const auto& definition : prepared) {
            http3_slots.push_back(
                definition.http3_.has_value() ? std::optional<std::size_t>(runtime->http3_slot_count_++) : std::nullopt);
        }
    }
    if (std::ranges::any_of(added, [](const auto& listener) {
            return !listener.connection_.has_value() && !listener.request_stream_ &&
                   !std::holds_alternative<detail::http_server_listener_definition::redirect_http_to_https>(listener.transport_);
        })) {
        enable_runtime_routes(*runtime, *state_);
    }

    // Every owner thread prepares the listeners -- bound or adopted, admitting
    // nothing -- before any opens them. Declared before each round's
    // publication, which waits for the controls borrowing them.
    const auto prepare = [&](auto& target) {
        for (std::size_t index = 0; index < prepared.size(); ++index) {
            target.prepare_listener(first_slot + index, http3_slots[index], prepared[index]
#if defined(RUVIA_ENABLE_HTTP3)
                ,
                directory
#endif
            );
        }
    };
    const auto discard = [first_slot, count = prepared.size()](auto& target) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            target.discard_listener(first_slot + index);
        }
    };
    const auto open = [first_slot, count = prepared.size()](auto& target) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            target.open_listener(first_slot + index);
        }
    };
    // Prebuild all rounds, including rollback and the Windows acceptor
    // controls. No allocation is allowed between preparation and commit, or
    // while discarding a partially prepared addition.
    runtime_control_publication prepared_publication;
    control_list prepare_controls(process_resource());
    make_worker_controls(prepare_controls, *runtime, prepared_publication,
        [&prepare](detail::web_worker_runtime& worker, std::size_t) { prepare(worker); });
    runtime_control_publication discarded_publication;
    control_list discard_controls(process_resource());
    make_worker_controls(discard_controls, *runtime, discarded_publication,
        [&discard](detail::web_worker_runtime& worker, std::size_t) { discard(worker); });
    runtime_control_publication opened_publication;
    control_list open_controls(process_resource());
    make_worker_controls(open_controls, *runtime, opened_publication,
        [&open](detail::web_worker_runtime& worker, std::size_t) { open(worker); });
#if defined(_WIN32)
    auto prepare_acceptor = make_acceptor_control(*runtime->acceptor_, prepared_publication,
        [&prepare](detail::windows_acceptor& acceptor) { prepare(acceptor); });
    auto discard_acceptor = make_acceptor_control(*runtime->acceptor_, discarded_publication,
        [&discard](detail::windows_acceptor& acceptor) { discard(acceptor); });
    runtime_control_publication accepting_publication;
    auto open_acceptor = make_acceptor_control(*runtime->acceptor_, accepting_publication,
        [&open](detail::windows_acceptor& acceptor) { open(acceptor); });
#endif
    try {
        bool posted = post_worker_controls(*runtime, prepare_controls);
#if defined(_WIN32)
        if (posted) {
            posted = runtime->acceptor_->post_control(std::move(prepare_acceptor));
        }
        prepare_acceptor = nullptr;
#endif
        prepared_publication.wait();
        if (const auto failure = prepared_publication.failure()) {
            std::rethrow_exception(failure);
        }
        if (!posted || prepared_publication.completed() != control_targets(*runtime)) {
            throw std::logic_error("the app began stopping before every worker prepared the listeners");
        }
    } catch (...) {
        // A stopping run closes its own resources if it refuses a control.
        (void)post_worker_controls(*runtime, discard_controls);
#if defined(_WIN32)
        (void)runtime->acceptor_->post_control(std::move(discard_acceptor));
#endif
        discarded_publication.wait();
#if defined(RUVIA_ENABLE_HTTP3) && !defined(_WIN32)
        // Sockets opened for workers that failed before claiming theirs.
        for (const auto& slot : http3_slots) {
            if (slot.has_value()) {
                directory->close_unclaimed_listener_sockets(*slot);
            }
        }
#endif
        throw;
    }

    bool published = false;
#if defined(_WIN32)
    // Every worker admits before the acceptor hands it connections. All
    // controls and storage are already allocated before this first open.
    const bool admitting = post_worker_controls(*runtime, open_controls);
    opened_publication.wait();
#endif
    {
        std::lock_guard lock(state_->mutex_);
#if defined(_WIN32)
        published = admitting && state_->lifecycle_.running() &&
                    runtime->acceptor_->post_control(std::move(open_acceptor));
#else
        published = state_->lifecycle_.running() && post_worker_controls(*runtime, open_controls);
#endif
        if (published) {
            // Later run()s serve them too; this run consumed their inherited sockets.
            for (std::size_t index = 0; index < added.size(); ++index) {
                runtime->listener_slots_[first_slot + index] = added[index].endpoint_;
                added[index].inherited_sockets_.reset();
                state_->listeners_.push_back(std::move(added[index]));
            }
        }
    }
    if (!published) {
        throw std::logic_error("the app began stopping before every worker accepted the listeners");
    }
}

void application::remove_listener(const listener_endpoint& endpoint) {
    const asio::ip::tcp::endpoint target(normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    constexpr auto not_running = "listeners can be removed only while the app is running";
    constexpr auto worker_thread = "listeners cannot be removed from a business worker thread";
    {
        std::lock_guard lock(state_->mutex_);
        (void)running_runtime(*state_, not_running, worker_thread);
    }
    const std::lock_guard serialized(state_->runtime_control_mutex_);
    detail::app_runtime_graph* runtime = nullptr;
    std::size_t slot = 0;
    {
        std::lock_guard lock(state_->mutex_);
        runtime = &running_runtime(*state_, not_running, worker_thread);
        (void)stored_listener_index(*state_, target);
        slot = runtime_listener_slot(*runtime, target);
    }
#if defined(_WIN32)
    // The acceptor stops accepting first, so no connection it accepts reaches
    // a worker that no longer admits the listener.
    {
        runtime_control_publication publication;
        if (!post_acceptor_control(*runtime->acceptor_, publication,
                [slot, receipt = publication.receipt()](detail::windows_acceptor& acceptor) {
                    acceptor.remove_listener(slot, receipt);
                })) {
            throw std::logic_error("the app began stopping before every worker removed the listener");
        }
    }
#endif
    // Destroyed after the lock.
    std::optional<detail::http_server_listener_definition> retired;
    bool published = false;
    {
        runtime_control_publication publication;
        control_list controls(process_resource());
        // Each worker holds a receipt until its accept loop exited.
        make_worker_controls(controls, *runtime, publication,
            [slot, receipt = publication.receipt()](detail::web_worker_runtime& worker, std::size_t) {
                worker.remove_listener(slot, receipt);
            });
        std::lock_guard lock(state_->mutex_);
        published = state_->lifecycle_.running() && post_worker_controls(*runtime, controls);
        if (published) {
            // Later run()s no longer serve it.
            const auto index = stored_listener_index(*state_, target);
            retired.emplace(std::move(state_->listeners_[index]));
            state_->listeners_.erase(state_->listeners_.begin() + static_cast<std::ptrdiff_t>(index));
            runtime->listener_slots_[slot].reset();
        }
    }
    if (!published) {
        throw std::logic_error("the app began stopping before every worker removed the listener");
    }
}

#if !defined(_WIN32)
listener_sockets application::duplicate_listener_sockets(const listener_endpoint& endpoint) const {
    const asio::ip::tcp::endpoint target(normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    constexpr auto not_running = "listener sockets can be duplicated only while the app is running";
    constexpr auto worker_thread = "listener sockets cannot be duplicated from a business worker thread";
    {
        std::lock_guard lock(state_->mutex_);
        (void)running_runtime(*state_, not_running, worker_thread);
    }
    const std::lock_guard serialized(state_->runtime_control_mutex_);
    // Written by each worker at its own index.
    std::pmr::vector<detail::listener_socket_duplicates> duplicates(process_resource());
    bool https = false;
    bool http3 = false;
    bool posted = false;
    std::exception_ptr failure;
    {
        runtime_control_publication publication;
        control_list controls(process_resource());
        {
            std::lock_guard lock(state_->mutex_);
            auto& runtime = running_runtime(*state_, not_running, worker_thread);
            const auto& stored = state_->listeners_[stored_listener_index(*state_, target)];
            https = std::holds_alternative<detail::http_server_listener_definition::tls>(stored.transport_);
            http3 = stored.http3_.has_value();
            const auto slot = runtime_listener_slot(runtime, target);
            duplicates.resize(runtime.workers_.size());
            make_worker_controls(controls, runtime, publication,
                [slot, &duplicates](detail::web_worker_runtime& worker, std::size_t index) {
                    duplicates[index] = worker.duplicate_listener(slot);
                });
            posted = post_worker_controls(runtime, controls);
        }
        publication.wait();
        failure = publication.failure();
        if (failure == nullptr && (!posted || publication.completed() != duplicates.size())) {
            failure = std::make_exception_ptr(
                std::logic_error("the app began stopping before every worker duplicated its sockets"));
        }
    }
    const auto close_duplicates = [&duplicates]() noexcept {
        for (const auto& duplicate : duplicates) {
            detail::close_native_socket(duplicate.tcp_);
            detail::close_native_socket(duplicate.udp_);
        }
    };
    listener_sockets result;
    auto& tcp = https ? result.https_ : result.http_;
    try {
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        tcp.reserve(duplicates.size());
        if (http3) {
            result.http3_.reserve(duplicates.size());
        }
    } catch (...) {
        close_duplicates();
        throw;
    }
    for (const auto& duplicate : duplicates) {
        tcp.push_back(duplicate.tcp_);
        if (http3) {
            result.http3_.push_back(duplicate.udp_);
        }
    }
    return result;
}
#endif

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
        [](detail::app_state& state_value) { state_value.options_.clear_rate_limit(); });
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
        "cannot change trusted proxies while app is running",
        [](detail::app_state& state_value) { state_value.options_.clear_trusted_proxies(); });
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

application& application::on_tls_client_hello(tls_client_hello_callback callback_value) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register TLS ClientHello hook while app is running",
        [callback_value = std::move(callback_value)](detail::app_state& state_value) mutable {
            std::swap(state_value.tls_client_hello_callback_, callback_value);
            state_value.options_.tls_client_hello_ =
                detail::callback_access::ref(state_value.tls_client_hello_callback_);
        });
}

application& application::on_tls_client_hello(std::nullptr_t) {
    return on_tls_client_hello(tls_client_hello_callback{});
}

application& application::on_http2_input(http2_input_callback callback_value) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register HTTP/2 input hook while app is running",
        [callback_value = std::move(callback_value)](detail::app_state& state_value) mutable {
            std::swap(state_value.http2_input_callback_, callback_value);
            state_value.options_.http2_input_ = detail::callback_access::ref(state_value.http2_input_callback_);
        });
}

application& application::on_http2_input(std::nullptr_t) {
    return on_http2_input(http2_input_callback{});
}

application& application::on_connection(connection_callback callback_value) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register connection handler while app is running",
        [callback_value = std::move(callback_value)](detail::app_state& state_value) mutable {
            std::swap(state_value.connection_callback_, callback_value);
            state_value.options_.connection_ = detail::callback_access::ref(state_value.connection_callback_);
        });
}

application& application::on_connection(std::nullptr_t) {
    return on_connection(connection_callback{});
}

application& application::on_request_stream(request_stream_callback callback_value) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register request stream handler while app is running",
        [callback_value = std::move(callback_value)](detail::app_state& state_value) mutable {
            std::swap(state_value.request_stream_callback_, callback_value);
            state_value.options_.request_stream_ = detail::callback_access::ref(state_value.request_stream_callback_);
        });
}

application& application::on_request_stream(std::nullptr_t) {
    return on_request_stream(request_stream_callback{});
}

#ifdef RUVIA_ENABLE_DATABASE
application& application::database(db_registration_config config) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure database while app is running", [&](detail::app_state& state_value) {
            auto* resource = process_resource();
            detail::validate_capability_alias(config.alias_.view(), "database alias must not be empty");
            detail::db_definition replacement{std::pmr::string(config.alias_.view(), resource),
                detail::db_config_storage(config.config_, resource)};
            if (config.query_cache_) {
#ifdef RUVIA_ENABLE_REDIS
                replacement.query_cache_.emplace(*config.query_cache_, resource);
#else
                throw std::invalid_argument("database query cache requires Redis support");
#endif
            }
            detail::upsert_named_capability_definition(state_value.databases_, std::move(replacement));
        });
}

application& application::database(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure database while app is running",
        [](detail::app_state& state_value) { state_value.databases_.clear(); });
}
#endif

#ifdef RUVIA_ENABLE_REDIS
application& application::redis(redis_registration_config config) {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure redis while app is running",
        [&](detail::app_state& state_value) {
            detail::upsert_named_capability_definition(
                state_value.redis_, config.alias_.view(), config.config_, "redis alias must not be empty");
        });
}

application& application::redis(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure redis while app is running",
        [](detail::app_state& state_value) { state_value.redis_.clear(); });
}
#endif

application& application::client(http_client_config config) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure HTTP client while app is running", [&](detail::app_state& state_value) {
            detail::upsert_named_capability_definition(state_value.http_clients_, config.alias_.view(), config,
                "HTTP client alias must not be empty");
        });
}

application& application::client(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure HTTP client while app is running",
        [](detail::app_state& state_value) { state_value.http_clients_.clear(); });
}

}  // namespace ruvia
