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

http_server_options standalone_options(server_config config, const http_server_callbacks& callbacks, std::size_t count) {
    // Scheduling belongs to the supplied loops, not this domain host.
    config.worker_count_ = count;
    config.process_signal_handlers_ = process_signal_handler_policy::external_owner;
    http_server_options options;
    options.set_server(config);
    options.connection_ = callback_access::ref(callbacks.connection_);
    options.request_stream_ = callback_access::ref(callbacks.request_stream_);
    options.access_log_.callback_ = callback_access::ref(callbacks.access_);
    options.connection_failure_.callback_ = callback_access::ref(callbacks.connection_failure_);
    options.tls_client_hello_ = callback_access::ref(callbacks.tls_client_hello_);
    options.http2_input_ = callback_access::ref(callbacks.http2_input_);
#if defined(RUVIA_ENABLE_HTTP2)
    options.http2_frame_ = callback_access::ref(callbacks.http2_frame_);
    options.http2_headers_ = callback_access::ref(callbacks.http2_headers_);
#endif
    options.connection_admission_ = callback_access::ref(callbacks.connection_admission_);
    options.connection_open_ = callback_access::ref(callbacks.connection_open_);
    options.connection_close_ = callback_access::ref(callbacks.connection_close_);
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

void close_server_group(const std::weak_ptr<http_server_group_state>& group) noexcept;
void fail_server_group(const std::weak_ptr<http_server_group_state>& group, const std::exception_ptr& failure) noexcept;

class http_server_state final : public std::enable_shared_from_this<http_server_state> {
    struct service final {
        http_server_listener_definition definition_;
        bool external_;
        bool active_{true};
        std::optional<std::size_t> http3_slot_;
    };

public:
    http_server_state(event_loop loop, std::shared_ptr<http_server_callbacks> callbacks, server_config config,
        std::size_t index = 0, std::size_t count = 1
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        std::shared_ptr<http3_worker_directory> directory = {}
#endif
        )
        : loop_(require_loop(std::move(loop))),
          worker_(loop_.handle()),
          callbacks_(std::move(callbacks)),
          options_(standalone_options(config, *callbacks_, count)),
          inbound_budget_(options_.server_.max_inbound_buffer_bytes_per_worker_),
          controllers_(std::span<const controller_registrar>{}),
          routes_(plan_, controllers_),
          tasks_(worker_, {.resource_ = process_resource()}),
          capabilities_(loop_.io_context(), worker_, {},
              {.max_decoded_body_bytes_ = options_.server_.max_buffered_body_bytes_,
                  .http_client_result_budget_ = options_.server_.http_client_result_budget_,
                  .trusted_proxies_ = &options_.trusted_proxies_,
                  .file_io_timeout_ = options_.server_.write_timeout_,
                  .connection_failure_ = &options_.connection_failure_}),
          connections_(loop_.io_context(), worker_, routes_, capabilities_, options_, token_, phase_, tasks_, {},
              {.target_ = this, .invoke_ = &fail}, index
#if defined(RUVIA_ENABLE_HTTP3)
              ,
              {.directory_ = directory.get(), .worker_index_ = index}
#endif
              ),
          services_(process_resource()),
          changed_(worker_)
#if defined(_WIN32)
          ,
          worker_index_(index)
#endif
    {
        options_.inbound_budget_ = &inbound_budget_;
        options_.codecs_ = &codecs_;
#if defined(RUVIA_ENABLE_HTTP3)
        directory_ = std::move(directory);
#endif
    }

    ~http_server_state() {
        if (failure_ && !failure_observed_) {
            report_unhandled_failure("HTTP server", failure_);
        }
    }

    void trusted_proxies(trusted_proxy_config config) {
        require_current();
        require_open();
        if (started_ || starting_) {
            throw std::logic_error("cannot change trusted proxies after HTTP server startup");
        }
        options_.set_trusted_proxies(config);
    }
    void bind_stop() {
        std::weak_ptr<http_server_state> weak = shared_from_this();
        stop_registration_ = loop_.on_stop([weak = std::move(weak)]() -> task<void> {
            if (auto state = weak.lock()) {
                const bool observe = !state->grouped_;
                co_await shutdown_owned(std::move(state), observe);
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
                services_[index].definition_.inherited_sockets_.reset();
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

    [[nodiscard]] bool close_connection(std::uint64_t connection_id) {
        require_current();
        require_running();
        return connections_.close_connection(connection_id);
    }

    void remove(const listener_endpoint& endpoint) {
        require_current();
        require_running();
        const auto index = find(endpoint);
#if defined(_WIN32)
        if (!services_[index].external_ && (!grouped_ || worker_index_ == 0)) {
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
        auto retired = std::exchange(current->identities_, std::move(identities));
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
        close_server_group(group_);
        stop_.request_stop();
        if (closing_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        group_retirement_ = group_.lock();
        if (worker_.is_current()) {
            begin_close();
            return;
        }
        try {
            if (!loop_.defer_cleanup([self = shared_from_this()] { self->begin_close(); })) {
                // Registration may have lost a race with retirement before
                // startup. A cold host owns no worker-affine I/O to join.
                if (!closed_.load(std::memory_order_acquire)) {
                    if (io_started_.load(std::memory_order_acquire)) {
                        std::terminate();
                    }
                    closed_.store(true, std::memory_order_release);
                    closed_promise_.set_value();
                    group_retirement_.reset();
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
    friend class http_server_group_state;
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
        fail_server_group(self.group_, error);
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
        if (!configured.external_ && (!grouped_ || worker_index_ == 0)) {
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
        if (!services_[index].external_ && (!grouped_ || worker_index_ == 0)) {
            if (control) {
                control->run(*acceptor_);
            } else {
                on_acceptor([this, index] { acceptor_->open_listener(index); });
            }
        }
#endif
    }

    static task<void> prepare_start_owned(std::shared_ptr<http_server_state> self) {
        self->require_current();
        self->require_open();
        if (self->started_ || self->starting_) {
            throw std::logic_error("HTTP server can only start once");
        }
        self->starting_ = true;
        self->phase_ = http_server_worker_state::running;
        try {
            self->io_started_.store(true, std::memory_order_release);
            self->connections_.prepare();
            for (std::size_t index = 0; index != self->services_.size(); ++index) {
                if (self->services_[index].active_) {
                    self->prepare(index);
                }
            }
            co_await self->capabilities_.start();
            self->require_open();
        } catch (...) {
            self->starting_ = false;
            self->changed_.notify();
            throw;
        }
        self->starting_ = false;
        self->changed_.notify();
    }

    void commit_start() {
        require_current();
        require_open();
        for (std::size_t index = 0; index != services_.size(); ++index) {
            if (services_[index].active_) {
                open(index);
            }
        }
        require_open();
        started_ = true;
    }

    static task<void> start_owned(std::shared_ptr<http_server_state> self) {
        self->require_current();
        self->require_open();
        if (self->started_ || self->starting_) {
            throw std::logic_error("HTTP server can only start once");
        }
        std::exception_ptr failure;
        try {
            co_await prepare_start_owned(self);
            self->commit_start();
        } catch (...) {
            failure = std::current_exception();
        }
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
                            self->failure_ = failure;
                        }
                    }
                    if (failure) {
                        fail_server_group(self->group_, failure);
                    }
                    self->closed_.store(true, std::memory_order_release);
                    self->closed_promise_.set_value();
                    self->changed_.notify();
                    self->group_retirement_.reset();
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
        if (!grouped_) {
            for (auto& entry : services_) {
                entry.definition_.inherited_sockets_.reset();
            }
        }
#if defined(_WIN32)
        if (acceptor_) {
            if (!grouped_) {
                acceptor_->request_stop();
                acceptor_->join();
            }
        }
#endif
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    static task<void> shutdown_owned(std::shared_ptr<http_server_state> self, bool observe = true) {
        self->require_current();
        self->request_close();
        while (!self->closed_.load(std::memory_order_acquire)) {
            co_await self->changed_.wait();
        }
        if (!observe) {
            co_return;
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
    std::shared_ptr<http_server_callbacks> callbacks_;
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
    std::shared_ptr<http3_worker_directory> directory_;
#endif
#if defined(_WIN32)
    std::shared_ptr<windows_acceptor> acceptor_;
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
    std::atomic<bool> io_started_{};
    std::mutex failure_mutex_;
    std::exception_ptr failure_;
    bool failure_observed_{};
    std::atomic<std::size_t> fatal_count_{};
    bool grouped_{};
#if defined(_WIN32)
    std::size_t worker_index_{};
#endif
    std::weak_ptr<http_server_group_state> group_;
    std::shared_ptr<http_server_group_state> group_retirement_;
    std::promise<void> closed_promise_;
    std::shared_future<void> closed_future_{closed_promise_.get_future().share()};
    event_loop_stop_registration stop_registration_;
};

class http_server_group_state final : public std::enable_shared_from_this<http_server_group_state> {
public:
    http_server_group_state(std::span<const event_loop> loops, http_server_callbacks callbacks, server_config config) {
        if (loops.empty()) {
            throw std::invalid_argument("HTTP server group requires at least one event loop");
        }
        for (std::size_t index = 0; index != loops.size(); ++index) {
            if (!loops[index].valid() || !loops[index].accepting()) {
                throw std::invalid_argument("HTTP server group requires valid event loops");
            }
            for (std::size_t previous = 0; previous != index; ++previous) {
                if (loops[index].id() == loops[previous].id()) {
                    throw std::invalid_argument("HTTP server group requires distinct event loops");
                }
            }
        }
        auto shared_callbacks = std::allocate_shared<http_server_callbacks>(
            std::pmr::polymorphic_allocator<http_server_callbacks>(process_resource()), std::move(callbacks));
#if defined(RUVIA_ENABLE_HTTP3)
        directory_ = std::allocate_shared<http3_worker_directory>(
            std::pmr::polymorphic_allocator<http3_worker_directory>(process_resource()), loops.size());
#endif
        workers_.reserve(loops.size());
        for (std::size_t index = 0; index != loops.size(); ++index) {
            auto worker = std::allocate_shared<http_server_state>(
                std::pmr::polymorphic_allocator<http_server_state>(process_resource()),
                loops[index], shared_callbacks, config, index, loops.size()
#if defined(RUVIA_ENABLE_HTTP3)
                                                                   ,
                directory_
#endif
            );
            worker->grouped_ = true;
#if defined(RUVIA_ENABLE_HTTP3)
            directory_->bind_worker(index, worker->worker_);
#endif
            workers_.push_back(std::move(worker));
        }
#if defined(_WIN32)
        acceptor_ = std::allocate_shared<windows_acceptor>(
            std::pmr::polymorphic_allocator<windows_acceptor>(process_resource()),
            std::span<const http_server_listener_definition>{}, loops.size(),
#if defined(RUVIA_ENABLE_HTTP3)
            directory_.get(),
#endif
            worker_failure_sink{.target_ = this, .invoke_ = [](void* target, const std::exception_ptr& error) noexcept {
                                    auto& self = *static_cast<http_server_group_state*>(target);
                                    http_server_state::fail(self.workers_.front().get(), error);
                                }});
        for (std::size_t index = 0; index != workers_.size(); ++index) {
            workers_[index]->acceptor_ = acceptor_;
            workers_[index]->connections_.bind_acceptor(*acceptor_);
            acceptor_->bind_worker(index, workers_[index]->connections_.acceptor_target());
        }
#endif
    }

    void bind_stop() {
        try {
            for (auto& worker : workers_) {
                worker->group_ = shared_from_this();
                worker->bind_stop();
            }
        } catch (...) {
            close();
            throw;
        }
    }

    void require_external() const {
        for (const auto& worker : workers_) {
            if (worker->worker_.is_current()) {
                throw std::logic_error("blocking HTTP server group control cannot wait on a member worker");
            }
        }
    }

    // Control completion is carried by the loop's retirement-protected cleanup
    // channel. No business queue slot or worker-owned thread is required.
    template <typename function_type>
    static void control(const std::shared_ptr<http_server_state>& worker, function_type function) {
        std::promise<void> completion;
        auto result = completion.get_future();
        if (!worker->loop_.defer_cleanup([worker, function = std::move(function), completion = std::move(completion)]() mutable {
                try {
                    function(*worker);
                    completion.set_value();
                } catch (...) {
                    completion.set_exception(std::current_exception());
                }
            })) {
            throw std::logic_error("HTTP server group worker has retired");
        }
        result.get();
    }

    void start() {
        require_external();
        std::lock_guard lock(mutex_);
        require_open();
        if (started_) {
            throw std::logic_error("HTTP server group can only start once");
        }
        try {
#if defined(_WIN32)
            if (!acceptor_->start() || !acceptor_->serve()) {
                throw std::runtime_error("HTTP server group acceptor could not start");
            }
#endif
            for (const auto& worker : workers_) {
                std::promise<void> completion;
                auto result = completion.get_future();
                control(worker, [worker, completion = std::move(completion)](http_server_state& state) mutable {
                    asio::co_spawn(state.loop_.executor(), ruvia::as_awaitable(http_server_state::prepare_start_owned(worker)),
                        [completion = std::move(completion)](std::exception_ptr failure) mutable {
                            if (failure) {
                                completion.set_exception(failure);
                            } else {
                                completion.set_value();
                            }
                        });
                });
                result.get();
            }
            require_open();
            for (const auto& worker : workers_) {
                control(worker, [](http_server_state& state) { state.commit_start(); });
            }
            require_open();
            started_ = true;
        } catch (...) {
            const auto failure = std::current_exception();
            close();
            join_closed();
            if (const auto fatal = observe_failure()) {
                std::rethrow_exception(fatal);
            }
            std::rethrow_exception(failure);
        }
    }

    void add(listen_config config, bool external) {
        // Consume inherited socket ownership before rejecting lifecycle/affinity.
        auto loaded = load_server_listeners(std::span<const listen_config>(&config, 1));
        require_external();
        std::lock_guard lock(mutex_);
        require_open();
        const auto stage = [this](const auto& worker, auto function) {
            if (started_) {
                control(worker, std::move(function));
            } else {
                function(*worker);
            }
        };
        std::pmr::vector<http_server_listener_definition> prepared(process_resource());
        stage(workers_.front(), [&](http_server_state& state) {
            state.require_open();
            for (const auto& definition : loaded) {
                if (!definition.connection_ && !definition.request_stream_ &&
                    !std::holds_alternative<http_server_listener_definition::redirect_http_to_https>(definition.transport_)) {
                    throw std::invalid_argument("http_server_group requires request_stream or connection dispatch");
                }
                if (external && (definition.http3_ || definition.inherited_sockets_ ||
                                    std::holds_alternative<http_server_listener_definition::redirect_http_to_https>(definition.transport_))) {
                    throw std::invalid_argument("an externally accepted service cannot own a listener or redirect");
                }
            }
            for (const auto& existing : state.services_) {
                if (existing.active_) {
                    ensure_new_listener_endpoints(std::span<const http_server_listener_definition>(&existing.definition_, 1), loaded);
                }
            }
            prepared.reserve(loaded.size());
            for (const auto& definition : loaded) {
                prepared.push_back(validate_run_listener(definition, state.options_, state.key_origin_, process_resource()));
            }
        });
        const auto first = listener_slots_;
        std::vector<std::pmr::vector<http_server_state::service>> staged;
        staged.reserve(workers_.size());
        [[maybe_unused]] const auto first_http3 = http3_slots_;
        auto next_http3 = http3_slots_;
        for (const auto& worker : workers_) {
            auto& entries = staged.emplace_back(process_resource());
            entries.reserve(prepared.size());
            next_http3 = http3_slots_;
            for (const auto& definition : prepared) {
                const auto slot = definition.http3_ ? std::optional<std::size_t>(next_http3++) : std::nullopt;
                entries.push_back({definition.clone(process_resource()), external, true, slot});
            }
            stage(worker, [&](http_server_state& state) { state.services_.reserve(first + prepared.size()); });
        }
        // Everything that can allocate was prepared before changing any slot.
        for (std::size_t index = 0; index != workers_.size(); ++index) {
            stage(workers_[index], [&](http_server_state& state) {
                for (auto& entry : staged[index]) {
                    state.services_.push_back(std::move(entry));
                }
                state.http3_slots_ = next_http3;
            });
        }
        listener_slots_ += prepared.size();
        http3_slots_ = next_http3;
        if (!started_) {
            return;
        }
        try {
            for (const auto& worker : workers_) {
                control(worker, [&](http_server_state& state) {
                    state.require_running();
                    for (auto index = first; index != listener_slots_; ++index) {
                        state.prepare(index);
                    }
                });
            }
            require_open();
            for (const auto& worker : workers_) {
                control(worker, [&](http_server_state& state) {
                    state.require_running();
                    for (auto index = first; index != listener_slots_; ++index) {
                        state.open(index);
                    }
                });
            }
            require_open();
        } catch (...) {
            const auto failure = std::current_exception();
            for (const auto& worker : workers_) {
                try {
                    control(worker, [&](http_server_state& state) {
                        for (auto index = first; index != listener_slots_; ++index) {
                            state.connections_.discard_listener(index);
                            state.services_[index].active_ = false;
                            state.services_[index].definition_.inherited_sockets_.reset();
#if defined(_WIN32)
                            if (state.worker_index_ == 0 && !external) {
                                state.on_acceptor([&state, index] { state.acceptor_->discard_listener(index); });
                            }
#endif
                        }
                    });
                } catch (...) {
                    close();
                }
            }
#if defined(RUVIA_ENABLE_HTTP3) && !defined(_WIN32)
            for (auto index = first_http3; index < http3_slots_; ++index) {
                directory_->close_unclaimed_listener_sockets(index);
            }
#endif
            std::rethrow_exception(failure);
        }
    }

    void trusted_proxies(trusted_proxy_config config) {
        require_external();
        std::lock_guard lock(mutex_);
        require_open();
        if (started_) {
            throw std::logic_error("cannot change trusted proxies after HTTP server group startup");
        }
        http_server_options validated;
        validated.set_trusted_proxies(config);
        std::vector<trusted_proxy_set> candidates;
        candidates.reserve(workers_.size());
        for (std::size_t index = 0; index != workers_.size(); ++index) {
            candidates.emplace_back(process_resource());
            candidates.back() = validated.trusted_proxies_;
        }
        for (std::size_t index = 0; index != workers_.size(); ++index) {
            workers_[index]->options_.trusted_proxies_ = std::move(candidates[index]);
        }
    }

    void remove(const listener_endpoint& endpoint) {
        require_external();
        std::lock_guard lock(mutex_);
        require_running();
        for (const auto& worker : workers_) {
            control(worker, [&](http_server_state& state) { state.remove(endpoint); });
        }
    }

    [[nodiscard]] bool close_connection(std::uint64_t connection_id) {
        require_external();
        std::lock_guard lock(mutex_);
        require_running();
        for (const auto& worker : workers_) {
            bool closed = false;
            control(worker, [&](http_server_state& state) { closed = state.close_connection(connection_id); });
            if (closed) {
                return true;
            }
        }
        return false;
    }

    void replace(const listener_endpoint& endpoint, tls_config config) {
        listen_config replacement{.address_ = endpoint.address_, .https_ = endpoint.port_, .tls_ = std::move(config), .dispatch_ = listener_dispatch::connection};
        auto loaded = load_server_listeners(std::span<const listen_config>(&replacement, 1));
        require_external();
        struct replacement_target final {
            std::size_t slot_;
            std::shared_ptr<const http_server_tls_identities> retired_;
        };
        // Retain callbacks until publication finishes and the control lock exits.
        std::pmr::vector<replacement_target> targets(process_resource());
        std::lock_guard lock(mutex_);
        require_running();
        auto identities = std::get<http_server_listener_definition::tls>(loaded.front().transport_).identities_;
        targets.reserve(workers_.size());
        for (const auto& worker : workers_) {
            control(worker, [&](http_server_state& state) {
                state.require_running();
                const auto index = state.find(endpoint);
                const auto* current = std::get_if<http_server_listener_definition::tls>(&state.services_[index].definition_.transport_);
                if (!current) {
                    throw std::invalid_argument("the connection service does not use TLS");
                }
                ensure_same_http_server_tls_policy(*current, replacement.tls_);
                targets.push_back({index, current->identities_});
            });
        }
        for (std::size_t index = 0; index != workers_.size(); ++index) {
            control(workers_[index], [&](http_server_state& state) {
                state.connections_.adopt_tls_identities(targets[index].slot_, identities);
                std::get<http_server_listener_definition::tls>(state.services_[targets[index].slot_].definition_.transport_).identities_ = identities;
            });
        }
    }

#ifndef _WIN32
    listener_sockets duplicate(const listener_endpoint& endpoint) {
        require_external();
        std::lock_guard lock(mutex_);
        require_running();
        listener_sockets result;
        result.http_.reserve(workers_.size());
        result.https_.reserve(workers_.size());
        result.http3_.reserve(workers_.size());
        try {
            for (const auto& worker : workers_) {
                control(worker, [&](http_server_state& state) {
                    auto sockets = state.duplicate(endpoint);
                    result.http_.insert(result.http_.end(), sockets.http_.begin(), sockets.http_.end());
                    result.https_.insert(result.https_.end(), sockets.https_.begin(), sockets.https_.end());
                    result.http3_.insert(result.http3_.end(), sockets.http3_.begin(), sockets.http3_.end());
                });
            }
        } catch (...) {
            for (const auto socket : result.http_) {
                close_native_socket(socket);
            }
            for (const auto socket : result.https_) {
                close_native_socket(socket);
            }
            for (const auto socket : result.http3_) {
                close_native_socket(socket);
            }
            throw;
        }
        return result;
    }
#endif

    void adopt(std::size_t index, const listener_endpoint& endpoint, native_socket socket) {
        // Guard ownership across affinity, index, posting and worker failures.
        struct socket_owner final {
            native_socket socket_;
            ~socket_owner() {
                close_native_socket(socket_);
            }
        } owner{socket};
        require_external();
        std::lock_guard lock(mutex_);
        require_running();
        if (index >= workers_.size()) {
            throw std::out_of_range("HTTP server group worker index");
        }
        control(workers_[index], [&](http_server_state& state) {
            state.adopt(endpoint, std::exchange(owner.socket_, invalid_native_socket));
        });
    }

    void record_failure(const std::exception_ptr& failure) noexcept {
        {
            std::lock_guard lock(failure_mutex_);
            if (!failure_) {
                failure_ = failure;
            }
        }
        close();
    }

    void close() noexcept {
        if (closing_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        for (const auto& worker : workers_) {
            worker->request_close();
        }
#if defined(_WIN32)
        acceptor_->request_stop();
#endif
    }

    void shutdown() {
        require_external();
        std::lock_guard lock(mutex_);
        close();
        join_closed();
        if (const auto failure = observe_failure()) {
            std::rethrow_exception(failure);
        }
    }

    [[nodiscard]] http_server_stats stats() const noexcept {
        http_server_stats result;
        for (const auto& worker : workers_) {
            const auto value = worker->stats();
            result.active_connections_ += value.active_connections_;
            result.connections_refused_ += value.connections_refused_;
            result.connection_failures_ += value.connection_failures_;
            result.accept_failures_ += value.accept_failures_;
            result.worker_failures_ += value.worker_failures_;
        }
        return result;
    }
    std::size_t worker_count() const noexcept {
        return workers_.size();
    }
    const worker_handle& worker(std::size_t index) const {
        return workers_.at(index)->worker_;
    }

private:
    std::exception_ptr observe_failure() noexcept {
        std::exception_ptr failure;
        {
            std::lock_guard lock(failure_mutex_);
            if (!failure_observed_) {
                failure = failure_;
            }
            failure_observed_ = true;
        }
        for (const auto& worker : workers_) {
            std::lock_guard failure_lock(worker->failure_mutex_);
            if (!worker->failure_observed_ && !failure) {
                failure = worker->failure_;
            }
            worker->failure_observed_ = true;
        }
        return failure;
    }
    void require_open() const {
        if (closing_.load(std::memory_order_acquire)) {
            throw std::logic_error("HTTP server group is closing");
        }
        for (const auto& worker : workers_) {
            if (!worker->loop_.valid() || !worker->loop_.accepting()) {
                throw std::logic_error("HTTP server group worker is stopping");
            }
        }
    }
    void require_running() const {
        require_open();
        if (!started_) {
            throw std::logic_error("HTTP server group is not started");
        }
    }
    void join_closed() {
        for (const auto& worker : workers_) {
            worker->closed_future_.wait();
        }
        for (const auto& worker : workers_) {
            for (auto& entry : worker->services_) {
                entry.definition_.inherited_sockets_.reset();
            }
        }
#if defined(RUVIA_ENABLE_HTTP3) && !defined(_WIN32)
        for (std::size_t index = 0; index != http3_slots_; ++index) {
            directory_->close_unclaimed_listener_sockets(index);
        }
#endif
#if defined(_WIN32)
        acceptor_->join();
#endif
    }
    std::vector<std::shared_ptr<http_server_state>> workers_;
#if defined(RUVIA_ENABLE_HTTP3)
    std::shared_ptr<http3_worker_directory> directory_;
#endif
#if defined(_WIN32)
    std::shared_ptr<windows_acceptor> acceptor_;
#endif
    mutable std::mutex mutex_;
    std::atomic<bool> closing_{};
    std::mutex failure_mutex_;
    std::exception_ptr failure_;
    bool failure_observed_{};
    bool started_{};
    std::size_t listener_slots_{};
    std::size_t http3_slots_{};
};

void close_server_group(const std::weak_ptr<http_server_group_state>& group) noexcept {
    if (const auto owner = group.lock()) {
        owner->close();
    }
}

void fail_server_group(const std::weak_ptr<http_server_group_state>& group, const std::exception_ptr& failure) noexcept {
    if (const auto owner = group.lock()) {
        owner->record_failure(failure);
    }
}

}  // namespace ruvia::detail

namespace ruvia {

http_server::http_server(event_loop loop, http_server_callbacks callbacks, server_config config)
    : state_(std::allocate_shared<detail::http_server_state>(
          std::pmr::polymorphic_allocator<detail::http_server_state>(process_resource()),
          std::move(loop), std::allocate_shared<http_server_callbacks>(std::pmr::polymorphic_allocator<http_server_callbacks>(process_resource()), std::move(callbacks)), config)) {
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
bool http_server::close_connection(std::uint64_t connection_id) & {
    return state_->close_connection(connection_id);
}
http_server& http_server::trusted_proxies(trusted_proxy_config config) & {
    state_->trusted_proxies(std::move(config));
    return *this;
}
task<void> http_server::start() & {
    return state_->start();
}
void http_server::remove_listener(const listener_endpoint& endpoint) & {
    state_->remove(endpoint);
}
void http_server::replace_tls_identities(const listener_endpoint& endpoint, tls_config config) & {
    const auto state = state_;
    state->replace(endpoint, std::move(config));
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

http_server_group::http_server_group(std::span<const event_loop> loops, http_server_callbacks callbacks, server_config config)
    : state_(std::allocate_shared<detail::http_server_group_state>(
          std::pmr::polymorphic_allocator<detail::http_server_group_state>(process_resource()),
          loops, std::move(callbacks), config)) {
    state_->bind_stop();
}
http_server_group::~http_server_group() {
    state_->close();
}
void http_server_group::listen(listen_config config) & {
    state_->add(std::move(config), false);
}
void http_server_group::add_connection_service(listen_config config) & {
    state_->add(std::move(config), true);
}
http_server_group& http_server_group::trusted_proxies(trusted_proxy_config config) & {
    state_->trusted_proxies(std::move(config));
    return *this;
}
void http_server_group::start() & {
    state_->start();
}
void http_server_group::remove_listener(const listener_endpoint& endpoint) & {
    state_->remove(endpoint);
}
void http_server_group::replace_tls_identities(const listener_endpoint& endpoint, tls_config config) & {
    const auto state = state_;
    state->replace(endpoint, std::move(config));
}
#ifndef _WIN32
listener_sockets http_server_group::duplicate_listener_sockets(const listener_endpoint& endpoint) const& {
    return state_->duplicate(endpoint);
}
#endif
void http_server_group::adopt(std::size_t worker_index, const listener_endpoint& service, native_socket socket) & {
    state_->adopt(worker_index, service, socket);
}
bool http_server_group::close_connection(std::uint64_t connection_id) & {
    return state_->close_connection(connection_id);
}
void http_server_group::close() noexcept {
    state_->close();
}
void http_server_group::shutdown() & {
    state_->shutdown();
}
http_server_stats http_server_group::stats() const noexcept {
    return state_->stats();
}
std::size_t http_server_group::worker_count() const noexcept {
    return state_->worker_count();
}
const worker_handle& http_server_group::worker(std::size_t index) const& {
    return state_->worker(index);
}

}  // namespace ruvia
