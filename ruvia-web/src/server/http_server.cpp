#include "ruvia/web/http_server.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/web/detail/controller/controller_descriptors.h"

#include "integration/worker_capabilities.h"
#include "router/compiled_route_plan.h"
#include "router/route_table.h"
#include "server/http_server_options_validation.h"
#include "server/http_server_tls_identity.h"
#include "server/inbound_buffer_resource.h"
#include "server/worker_codecs.h"
#include "server/worker_connections.h"

namespace ruvia::detail {
namespace {

http_server_options standalone_options(server_config config, const http_server_callbacks& callbacks) {
    // Scheduling belongs to the supplied loop, not this domain host.
    config.worker_count_ = 1;
    config.process_signal_handlers_ = process_signal_handler_policy::external_owner;
    http_server_options options;
    options.set_server(config);
    options.connection_ = callback_access::ref(callbacks.connection_);
    options.request_stream_ = callback_access::ref(callbacks.request_stream_);
    options.access_log_.callback_ = callback_access::ref(callbacks.access_);
    options.connection_failure_.callback_ = callback_access::ref(callbacks.connection_failure_);
    options.tls_client_hello_ = callback_access::ref(callbacks.tls_client_hello_);
    options.http2_input_ = callback_access::ref(callbacks.http2_input_);
    return options;
}

asio::ip::tcp::endpoint service_endpoint(const listener_endpoint& endpoint) {
    const auto address = parse_ip_address(endpoint.address_.view());
    if (!address || endpoint.port_.value() == 0) {
        throw std::invalid_argument("connection service requires a numeric address and nonzero port");
    }
    return {*address, endpoint.port_.value()};
}

}  // namespace

class http_server_state final : public std::enable_shared_from_this<http_server_state> {
    struct service final {
        http_server_listener_definition definition_;
        bool external_;
        bool active_{true};
        std::optional<std::size_t> http3_slot_;
    };

public:
    http_server_state(event_loop loop, http_server_callbacks callbacks, server_config config)
        : loop_(require_loop(std::move(loop))),
          worker_(loop_.handle()),
          callbacks_(std::move(callbacks)),
          options_(standalone_options(config, callbacks_)),
          inbound_budget_(options_.server_.max_inbound_buffer_bytes_per_worker_),
          controllers_(std::span<const controller_registrar>{}),
          routes_(plan_, controllers_),
          tasks_(worker_, {.resource_ = process_resource()}),
          capabilities_(loop_.io_context(), worker_, {},
              {.max_decoded_body_bytes_ = options_.server_.max_buffered_body_bytes_,
                  .http_client_result_budget_ = options_.server_.http_client_result_budget_,
                  .file_io_timeout_ = options_.server_.write_timeout_,
                  .connection_failure_ = &options_.connection_failure_}),
          connections_(loop_.io_context(), worker_, routes_, capabilities_, options_, token_, phase_, tasks_, {},
              {.target_ = this, .invoke_ = &fail}, 0
#if defined(RUVIA_ENABLE_HTTP3)
              ,
              {}
#endif
              ),
          services_(process_resource()),
          changed_(worker_) {
        options_.inbound_budget_ = &inbound_budget_;
        options_.codecs_ = &codecs_;
    }

    ~http_server_state() {
        if (failure_ && !failure_observed_) {
            report_unhandled_failure("HTTP server", failure_);
        }
    }

    void bind_stop() {
        std::weak_ptr<http_server_state> weak = shared_from_this();
        stop_registration_ = loop_.on_stop([weak = std::move(weak)]() -> task<void> {
            if (auto state = weak.lock()) {
                co_await shutdown_owned(std::move(state));
            }
        });
    }

    void add(listen_config config, bool external) {
        // The shared loader owns inherited handles and cleans secrets on every
        // outcome, including an invalid lifecycle or service dispatch below.
        auto loaded = load_server_listeners(std::span<const listen_config>(&config, 1));
        require_current();
        require_open();
        if (starting_) {
            throw std::logic_error("cannot add a service while the server is starting");
        }
        for (const auto& definition : loaded) {
            if (!definition.connection_ && !definition.request_stream_ &&
                !std::holds_alternative<http_server_listener_definition::redirect_http_to_https>(definition.transport_)) {
                throw std::invalid_argument("http_server requires request_stream or connection dispatch");
            }
            if (external && (definition.http3_ || definition.inherited_sockets_ ||
                                std::holds_alternative<http_server_listener_definition::redirect_http_to_https>(definition.transport_))) {
                throw std::invalid_argument("an externally accepted service cannot own a listener or redirect");
            }
        }
        for (const auto& existing : services_) {
            if (existing.active_) {
                ensure_new_listener_endpoints(std::span<const http_server_listener_definition>(&existing.definition_, 1), loaded);
            }
        }
        std::pmr::vector<http_server_listener_definition> prepared(process_resource());
        prepared.reserve(loaded.size());
        for (const auto& definition : loaded) {
            prepared.push_back(validate_run_listener(definition, options_, key_origin_, process_resource()));
        }
        const auto first = services_.size();
        services_.reserve(first + prepared.size());
#if defined(_WIN32)
        std::pmr::vector<acceptor_control> prepare_controls(process_resource());
        std::pmr::vector<acceptor_control> discard_controls(process_resource());
        std::pmr::vector<acceptor_control> open_controls(process_resource());
        if (started_ && !external) {
            prepare_controls.reserve(prepared.size());
            discard_controls.reserve(prepared.size());
            open_controls.reserve(prepared.size());
            for (std::size_t offset = 0; offset != prepared.size(); ++offset) {
                const auto index = first + offset;
                prepare_controls.emplace_back([this, index] { prepare_acceptor_listener(index); });
                discard_controls.emplace_back([this, index] { acceptor_->discard_listener(index); });
                open_controls.emplace_back([this, index] { acceptor_->open_listener(index); });
            }
        }
#endif
        for (auto& definition : prepared) {
            const auto http3 = definition.http3_ ? std::optional<std::size_t>(http3_slots_++) : std::nullopt;
            services_.push_back({std::move(definition), external, true, http3});
        }
        if (!started_) {
            return;
        }
        try {
            for (auto index = first; index != services_.size(); ++index) {
                prepare(index
#if defined(_WIN32)
                    ,
                    external ? nullptr : &prepare_controls[index - first]
#endif
                );
            }
        } catch (...) {
            for (auto index = first; index != services_.size(); ++index) {
                connections_.discard_listener(index);
#if defined(_WIN32)
                if (acceptor_ && !services_[index].external_) {
                    try {
                        discard_controls[index - first].run(*acceptor_);
                    } catch (...) {
                        request_close();
                    }
                }
#endif
                services_[index].active_ = false;
            }
            throw;
        }
        for (auto index = first; index != services_.size(); ++index) {
            open(index
#if defined(_WIN32)
                ,
                external ? nullptr : &open_controls[index - first]
#endif
            );
        }
    }

    [[nodiscard]] task<void> start() {
        return start_owned(shared_from_this());
    }

    void adopt(const listener_endpoint& endpoint, native_socket socket) {
        std::size_t index;
        try {
            require_current();
            require_running();
            index = find(endpoint);
        } catch (...) {
            close_native_socket(socket);
            throw;
        }
        connections_.adopt_connection(index, socket);
    }

    void remove(const listener_endpoint& endpoint) {
        require_current();
        require_running();
        const auto index = find(endpoint);
#if defined(_WIN32)
        if (!services_[index].external_) {
            on_acceptor([this, index] { acceptor_->remove_listener(index, {}); });
        }
#endif
        connections_.remove_listener(index, {});
        services_[index].active_ = false;
    }

    void replace(const listener_endpoint& endpoint, tls_config config) {
        // Loading through the shared entry also erases every owned PEM/password
        // if affinity, endpoint lookup or policy validation subsequently fails.
        listen_config replacement{.address_ = endpoint.address_, .https_ = endpoint.port_, .tls_ = std::move(config), .dispatch_ = listener_dispatch::connection};
        auto loaded = load_server_listeners(std::span<const listen_config>(&replacement, 1));
        require_current();
        require_running();
        const auto index = find(endpoint);
        auto* current = std::get_if<http_server_listener_definition::tls>(&services_[index].definition_.transport_);
        if (!current) {
            throw std::invalid_argument("the connection service does not use TLS");
        }
        ensure_same_http_server_tls_policy(*current, replacement.tls_);
        auto identities = std::get<http_server_listener_definition::tls>(loaded.front().transport_).identities_;
        connections_.adopt_tls_identities(index, identities);
        current->identities_ = std::move(identities);
    }

#ifndef _WIN32
    [[nodiscard]] listener_sockets duplicate(const listener_endpoint& endpoint) {
        require_current();
        require_running();
        const auto index = find(endpoint);
        auto result = connections_.duplicate_listener(index);
        listener_sockets sockets;
        try {
            auto& tcp = std::holds_alternative<http_server_listener_definition::tls>(services_[index].definition_.transport_) ? sockets.https_ : sockets.http_;
            tcp.push_back(result.tcp_);
            if (result.udp_ != invalid_native_socket) {
                sockets.http3_.push_back(result.udp_);
            }
        } catch (...) {
            close_native_socket(result.tcp_);
            close_native_socket(result.udp_);
            throw;
        }
        return sockets;
    }
#endif

    void request_close() noexcept {
        const auto self = shared_from_this();
        stop_.request_stop();
        if (closing_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (worker_.is_current()) {
            begin_close();
            return;
        }
        try {
            if (!loop_.defer_cleanup([self = shared_from_this()] { self->begin_close(); })) {
                // A retired loop already ran its registered shutdown callback.
                if (!closed_.load(std::memory_order_acquire)) {
                    std::terminate();
                }
            }
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] task<void> shutdown() {
        return shutdown_owned(shared_from_this());
    }
    [[nodiscard]] http_server_stats stats() const noexcept {
        auto stats = connections_.stats();
        stats.worker_failures_ = fatal_count_.load(std::memory_order_relaxed);
        return stats;
    }
    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }

private:
    static event_loop require_loop(event_loop loop) {
        if (!loop.valid()) {
            throw std::invalid_argument("HTTP server requires a valid event loop");
        }
        return loop;
    }
    void require_current() const {
        if (!worker_.is_current()) {
            throw std::logic_error("HTTP server operation must run on its bound event loop");
        }
    }
    void require_open() const {
        if (closing_.load(std::memory_order_acquire)) {
            throw std::logic_error("HTTP server is closing");
        }
    }
    void require_running() const {
        require_open();
        if (!started_ || starting_) {
            throw std::logic_error("HTTP server is not started");
        }
    }
    std::size_t find(const listener_endpoint& endpoint) const {
        const auto target = service_endpoint(endpoint);
        for (std::size_t index = 0; index != services_.size(); ++index) {
            if (services_[index].active_ && services_[index].definition_.endpoint_ == target) {
                return index;
            }
        }
        throw std::invalid_argument("no connection service serves this endpoint");
    }
    static void fail(void* target, const std::exception_ptr& error) noexcept {
        auto& self = *static_cast<http_server_state*>(target);
        {
            std::lock_guard lock(self.failure_mutex_);
            if (!self.failure_) {
                self.failure_ = error;
            }
        }
        self.fatal_count_.fetch_add(1, std::memory_order_relaxed);
        self.request_close();
    }

#if defined(_WIN32)
    class acceptor_control final {
    public:
        template <typename callable_type>
        explicit acceptor_control(callable_type callable) {
            std::promise<void> completion;
            result_ = completion.get_future();
            callback_ = [callable = std::move(callable), completion = std::move(completion)]() mutable {
                try {
                    callable();
                    completion.set_value();
                } catch (...) {
                    completion.set_exception(std::current_exception());
                }
            };
        }
        acceptor_control(acceptor_control&&) noexcept = default;
        acceptor_control(const acceptor_control&) = delete;
        void run(windows_acceptor& acceptor) {
            if (!acceptor.post_control(std::move(callback_))) {
                throw std::logic_error("HTTP server acceptor is stopping");
            }
            result_.get();
        }

    private:
        std::future<void> result_;
        move_only_function<void()> callback_;
    };

    template <typename callable_type>
    void on_acceptor(callable_type callable) {
        acceptor_control control(std::move(callable));
        control.run(*acceptor_);
    }

    void prepare_acceptor_listener(std::size_t index) {
        auto& entry = services_[index];
        acceptor_->prepare_listener(index, entry.http3_slot_, entry.definition_
#if defined(RUVIA_ENABLE_HTTP3)
            ,
            directory_.get()
#endif
        );
    }
#endif

    void prepare(std::size_t index
#if defined(_WIN32)
        ,
        acceptor_control* control = nullptr
#endif
    ) {
        auto& configured = services_[index];
#if defined(RUVIA_ENABLE_HTTP3)
        if (configured.http3_slot_ && !directory_) {
            directory_ = make_pmr_object<http3_worker_directory>(process_resource(), 1);
            directory_->bind_worker(0, worker_);
        }
#endif
#if defined(_WIN32)
        if (!configured.external_ && !acceptor_) {
            acceptor_ = make_pmr_object<windows_acceptor>(process_resource(), std::span<const http_server_listener_definition>{}, 1,
#if defined(RUVIA_ENABLE_HTTP3)
                directory_.get(),
#endif
                worker_failure_sink{.target_ = this, .invoke_ = &fail});
            connections_.bind_acceptor(*acceptor_);
            acceptor_->bind_worker(0, connections_.acceptor_target());
            if (!acceptor_->start() || !acceptor_->serve()) {
                throw std::runtime_error("HTTP server acceptor could not start");
            }
        }
#endif
        connections_.prepare_listener(index, configured.http3_slot_, configured.definition_, configured.external_
#if defined(RUVIA_ENABLE_HTTP3)
            ,
            directory_.get()
#endif
        );
#if defined(_WIN32)
        if (!configured.external_) {
            if (control) {
                control->run(*acceptor_);
            } else {
                on_acceptor([this, index] { prepare_acceptor_listener(index); });
            }
        }
#endif
        configured.definition_.inherited_sockets_.reset();
    }

    void open(std::size_t index
#if defined(_WIN32)
        ,
        acceptor_control* control = nullptr
#endif
    ) {
        connections_.open_listener(index);
#if defined(_WIN32)
        if (!services_[index].external_) {
            if (control) {
                control->run(*acceptor_);
            } else {
                on_acceptor([this, index] { acceptor_->open_listener(index); });
            }
        }
#endif
    }

    static task<void> start_owned(std::shared_ptr<http_server_state> self) {
        self->require_current();
        self->require_open();
        if (self->started_ || self->starting_) {
            throw std::logic_error("HTTP server can only start once");
        }
        self->starting_ = true;
        self->phase_ = http_server_worker_state::running;
        std::exception_ptr failure;
        try {
            self->connections_.prepare();
            for (std::size_t index = 0; index != self->services_.size(); ++index) {
                if (self->services_[index].active_) {
                    self->prepare(index);
                }
            }
            co_await self->capabilities_.start();
            self->require_open();
            for (std::size_t index = 0; index != self->services_.size(); ++index) {
                if (self->services_[index].active_) {
                    self->open(index);
                }
            }
            self->require_open();
            self->started_ = true;
        } catch (...) {
            failure = std::current_exception();
        }
        self->starting_ = false;
        self->changed_.notify();
        if (failure) {
            self->request_close();
            co_await shutdown_owned(self);
            std::rethrow_exception(failure);
        }
    }

    void begin_close() noexcept {
        require_current();
        closing_.store(true, std::memory_order_release);
        stop_.request_stop();
        if (close_started_) {
            return;
        }
        close_started_ = true;
#if defined(_WIN32)
        if (acceptor_) {
            acceptor_->stop_admission();
        }
#endif
        phase_ = http_server_worker_state::stopped;
        connections_.stop();
        capabilities_.close_now();
        try {
            asio::co_spawn(loop_.executor(), ruvia::as_awaitable(cleanup()),
                [self = shared_from_this()](std::exception_ptr failure) noexcept {
                    if (failure) {
                        std::lock_guard lock(self->failure_mutex_);
                        if (!self->failure_) {
                            self->failure_ = std::move(failure);
                        }
                    }
                    self->closed_.store(true, std::memory_order_release);
                    self->changed_.notify();
                });
        } catch (...) {
            std::terminate();
        }
    }

    task<void> cleanup() {
        while (starting_) {
            co_await changed_.wait();
        }
        std::exception_ptr failure;
        try {
            co_await tasks_.join();
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            co_await capabilities_.stop();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        connections_.retire_tls();
#if defined(_WIN32)
        if (acceptor_) {
            acceptor_->request_stop();
            acceptor_->join();
        }
#endif
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    static task<void> shutdown_owned(std::shared_ptr<http_server_state> self) {
        self->require_current();
        self->begin_close();
        while (!self->closed_.load(std::memory_order_acquire)) {
            co_await self->changed_.wait();
        }
        std::exception_ptr failure;
        {
            std::lock_guard lock(self->failure_mutex_);
            failure = self->failure_;
        }
        self->failure_observed_ = true;
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    event_loop loop_;
    worker_handle worker_;
    http_server_callbacks callbacks_;
    http_server_options options_;
    inbound_buffer_budget inbound_budget_;
    worker_codecs codecs_;
    compiled_route_plan plan_;
    controller_store controllers_;
    route_table routes_;
    stop_source stop_;
    stop_token token_{stop_.token()};
    http_server_worker_state phase_{http_server_worker_state::fresh};
    task_scope tasks_;
    worker_capabilities capabilities_;
#if defined(RUVIA_ENABLE_HTTP3)
    std::unique_ptr<http3_worker_directory, pmr_object_deleter<http3_worker_directory>> directory_{nullptr, pmr_object_deleter<http3_worker_directory>{process_resource()}};
#endif
#if defined(_WIN32)
    std::unique_ptr<windows_acceptor, pmr_object_deleter<windows_acceptor>> acceptor_{nullptr, pmr_object_deleter<windows_acceptor>{process_resource()}};
#endif
    worker_connections connections_;
    std::pmr::vector<service> services_;
    std::size_t http3_slots_{};
    std::chrono::steady_clock::time_point key_origin_{std::chrono::steady_clock::now()};
    worker_signal changed_;
    bool starting_{};
    bool started_{};
    bool close_started_{};
    std::atomic<bool> closing_{};
    std::atomic<bool> closed_{};
    std::mutex failure_mutex_;
    std::exception_ptr failure_;
    bool failure_observed_{};
    std::atomic<std::size_t> fatal_count_{};
    event_loop_stop_registration stop_registration_;
};

}  // namespace ruvia::detail

namespace ruvia {

http_server::http_server(event_loop loop, http_server_callbacks callbacks, server_config config)
    : state_(std::allocate_shared<detail::http_server_state>(
          std::pmr::polymorphic_allocator<detail::http_server_state>(process_resource()),
          std::move(loop), std::move(callbacks), config)) {
    state_->bind_stop();
}
http_server::~http_server() {
    state_->request_close();
}
void http_server::listen(listen_config config) & {
    state_->add(std::move(config), false);
}
void http_server::add_connection_service(listen_config config) & {
    state_->add(std::move(config), true);
}
void http_server::adopt(const listener_endpoint& service, native_socket socket) & {
    state_->adopt(service, socket);
}
task<void> http_server::start() & {
    return state_->start();
}
void http_server::remove_listener(const listener_endpoint& endpoint) & {
    state_->remove(endpoint);
}
void http_server::replace_tls_identities(const listener_endpoint& endpoint, tls_config config) & {
    state_->replace(endpoint, std::move(config));
}
#ifndef _WIN32
listener_sockets http_server::duplicate_listener_sockets(const listener_endpoint& endpoint) const& {
    return state_->duplicate(endpoint);
}
#endif
void http_server::close() noexcept {
    state_->request_close();
}
task<void> http_server::shutdown() & {
    return state_->shutdown();
}
http_server_stats http_server::stats() const noexcept {
    return state_->stats();
}
const worker_handle& http_server::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
