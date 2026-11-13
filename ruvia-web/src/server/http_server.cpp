#include "ruvia/web/http_server.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/bind_executor.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/move_only_function.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/web/detail/controller_descriptors.h"

#include "integration/runtime_control_completion.h"
#include "integration/runtime_control_gate.h"
#include "integration/worker_capabilities.h"
#include "router/compiled_route_plan.h"
#include "router/route_table.h"
#include "server/http_server_options_validation.h"
#include "server/http_server_tls_identity.h"
#include "server/inbound_buffer_budget.h"
#include "server/prepared_listener_state.h"
#include "server/worker_codecs.h"
#include "server/worker_connections.h"

namespace ruvia::detail {
namespace {

http_server_options standalone_options(server_config config, const http_server_callbacks& callbacks, std::size_t count) {
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
#if defined(RUVIA_ENABLE_HTTP3)
    options.http3_frame_ = callback_access::ref(callbacks.http3_frame_);
    options.http3_field_ = callback_access::ref(callbacks.http3_field_);
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

template <typename input_type>
std::shared_ptr<owned_listen_configs> own_listen(input_type input) {
    owned_listen_configs guard(std::move(input));
    // Allocation precedes transfer so allocation failure still closes handles.
    return std::make_shared<owned_listen_configs>(std::move(guard));
}

struct owned_endpoint final {
    std::string address_;
    port_number port_;
    explicit owned_endpoint(const listener_endpoint& endpoint)
        : address_(endpoint.address_.view()),
          port_(endpoint.port_) {}
    listener_endpoint view() const {
        return {.address_ = address_, .port_ = port_};
    }
};

struct socket_owner final {
    native_socket socket_;
    explicit socket_owner(native_socket socket) noexcept
        : socket_(socket) {}
    socket_owner(socket_owner&& other) noexcept
        : socket_(std::exchange(other.socket_, invalid_native_socket)) {}
    socket_owner(const socket_owner&) = delete;
    ~socket_owner() {
        close_native_socket(socket_);
    }
};

struct owned_adopt_options final {
    std::vector<std::byte> prefix_{};
    std::optional<ip_endpoint> remote_;
    explicit owned_adopt_options(adopt_options options)
        : prefix_(options.prefix_.begin(), options.prefix_.end()),
          remote_(options.remote_) {}
    adopt_options view() const noexcept {
        return {.prefix_ = prefix_, .remote_ = remote_};
    }
};

using definitions = std::vector<http_server_listener_definition>;
using sessions = std::vector<prepared_http_server_session>;
struct listener_candidate final {
    definitions definitions_{};
    std::vector<sessions> sessions_;
};

listener_candidate load_candidate(const std::vector<std::shared_ptr<owned_listen_configs>>& inputs,
    bool external, server_config config, const std::shared_ptr<http_server_callbacks>& callbacks,
    std::size_t count, std::chrono::steady_clock::time_point origin) {
    listener_candidate result;
    const auto options = standalone_options(config, *callbacks, count);
    for (const auto& input : inputs) {
        for (auto& config : input->configs_) {
            if (!config.dispatch_) {
                if (static_cast<bool>(callbacks->connection_) == static_cast<bool>(callbacks->request_stream_)) {
                    throw std::invalid_argument("standalone listener requires an unambiguous callback or explicit dispatch");
                }
                config.dispatch_ = callbacks->connection_ ? listener_dispatch::connection : listener_dispatch::request_stream;
            }
        }
        auto loaded = input->load();
        for (const auto& definition : loaded) {
            if (!definition.connection_ && !definition.request_stream_ &&
                !std::holds_alternative<http_server_listener_definition::redirect_http_to_https>(definition.transport_)) {
                throw std::invalid_argument("HTTP server requires request_stream or connection dispatch");
            }
            if (external && (definition.http3_ || definition.inherited_sockets_ ||
                                std::holds_alternative<http_server_listener_definition::redirect_http_to_https>(definition.transport_))) {
                throw std::invalid_argument("an externally accepted service cannot own a listener or redirect");
            }
            ensure_new_listener_endpoints(result.definitions_, std::span<const http_server_listener_definition>(&definition, 1));
            result.definitions_.push_back(validate_run_listener(definition, options, origin));
        }
    }
    return result;
}

void prepare_sessions(listener_candidate& candidate, std::size_t count, bool observe) {
    candidate.sessions_.resize(count);
    for (auto& worker : candidate.sessions_) {
        worker.reserve(candidate.definitions_.size());
        for (const auto& definition : candidate.definitions_) {
            worker.push_back(std::holds_alternative<http_server_listener_definition::tls>(definition.transport_)
                                 ? prepare_http_server_session_config(definition, observe)
                                 : prepared_http_server_session{});
        }
    }
}

bool needs_tls_preparation(const std::vector<std::shared_ptr<owned_listen_configs>>& inputs, const definitions& existing = {}) {
    for (const auto& input : inputs) {
        if (input->needs_tls_preparation()) {
            return true;
        }
    }
    for (const auto& definition : existing) {
        if (std::holds_alternative<http_server_listener_definition::tls>(definition.transport_)) {
            return true;
        }
    }
    return false;
}

template <typename function_type>
task<std::invoke_result_t<function_type&>> prepare_work(worker_ref caller, bool blocking, function_type function) {
    using result_type = std::invoke_result_t<function_type&>;
    if (!caller.is_current() || !caller.accepting()) {
        throw std::logic_error("HTTP server caller is not running");
    }
    if (blocking) {
        auto result = co_await run_blocking(caller, std::move(function));
        if (!caller.accepting()) {
            throw std::logic_error("HTTP server caller is stopping");
        }
        co_return std::move(result).take_value();
    }
    if constexpr (std::is_void_v<result_type>) {
        function();
        co_return;
    } else {
        co_return function();
    }
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
        bool published_{false};
    };
    struct activity final {
        http_server_state& self_;
        explicit activity(http_server_state& self)
            : self_(self) {
            ++self_.controls_;
        }
        ~activity() {
            --self_.controls_;
            self_.changed_.notify();
        }
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
          worker_(loop_.worker()),
          callbacks_(std::move(callbacks)),
          options_(standalone_options(config, *callbacks_, count)),
          inbound_budget_(options_.server_.max_inbound_buffer_bytes_per_worker_),
          controllers_(std::span<const controller_registrar>{}),
          routes_(plan_, controllers_),
          tasks_(worker_),
          capabilities_(loop_.io_context(), worker_, {},
              {.max_decoded_body_bytes_ = options_.server_.max_buffered_body_bytes_,
                  .http_client_worker_ = options_.server_.http_client_worker_,
                  .trusted_proxies_ = &options_.trusted_proxies_,
                  .precompressed_static_files_ = options_.compression_.has_value(),
                  .file_io_timeout_ = options_.server_.write_timeout_,
                  .connection_failure_ = &options_.connection_failure_}),
          connections_(loop_.io_context(), worker_, routes_, capabilities_, options_, token_, phase_, tasks_, {},
              {.target_ = this, .invoke_ = &fail}, index
#if defined(RUVIA_ENABLE_HTTP3)
              ,
              {.directory_ = directory.get(), .worker_index_ = index}
#endif
              ),
          services_(),
          changed_(worker_)
#if defined(_WIN32)
          ,
          worker_index_(index)
#endif
    {
        options_.inbound_budget_ = &inbound_budget_;
        options_.codecs_ = &codecs_;
        closed_waiters_.reserve(2);
#if defined(RUVIA_ENABLE_HTTP3)
        directory_ = std::move(directory);
#endif
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
                const bool observe = !state->grouped_;
                co_await shutdown_owned(std::move(state), observe);
            }
        });
    }
    void stage(std::shared_ptr<owned_listen_configs> input) {
        require_current();
        require_open();
        if (started_ || starting_) {
            throw std::logic_error("listen() is only available before HTTP server startup");
        }
        staged_.push_back(std::move(input));
    }
    void trusted_proxies(trusted_proxy_config config) {
        require_current();
        require_open();
        if (started_ || starting_) {
            throw std::logic_error("cannot change trusted proxies after HTTP server startup");
        }
        options_.set_trusted_proxies(config);
    }
    task<void> add(std::shared_ptr<owned_listen_configs> input, bool external) {
        return add_owned(shared_from_this(), std::move(input), external);
    }
    task<prepared_listener> prepare(std::shared_ptr<owned_listen_configs> input) {
        auto transaction = prepared_listener_access::make(worker_);
        return prepared_listener_access::launch(transaction, add_owned(shared_from_this(), std::move(input), false, transaction));
    }
    task<void> start() {
        return start_owned(shared_from_this());
    }
    task<void> replace(owned_endpoint endpoint, std::shared_ptr<owned_listen_configs> replacement) {
        return replace_owned(shared_from_this(), std::move(endpoint), std::move(replacement));
    }
    void adopt(const listener_endpoint& endpoint, native_socket socket, adopt_options options) {
        std::size_t index;
        try {
            require_current();
            require_running();
            index = find(endpoint);
        } catch (...) {
            close_native_socket(socket);
            throw;
        }
        connections_.adopt_connection(index, socket, options);
    }
    bool close_connection(std::uint64_t id) {
        require_current();
        require_running();
        return connections_.close_connection(id);
    }
    task<void> wait_connections(owned_endpoint endpoint, operation_options options) {
        return wait_connections_owned(shared_from_this(), std::move(endpoint), std::move(options));
    }
    void remove(const listener_endpoint& endpoint, std::shared_ptr<void> receipt = {}, listener_remove_options options = {}) {
        require_current();
        require_running();
        const auto index = find(endpoint);
#if defined(_WIN32)
        if (!services_[index].external_ && (!grouped_ || worker_index_ == 0)) {
            struct removal_command final {
                std::shared_ptr<void> receipt_;
                std::shared_ptr<http_server_state> owner_;
                std::size_t index_;
                void operator()() {
                    owner_->acceptor_->remove_listener(index_, receipt_);
                }
            };
            if (!acceptor_->post_control(removal_command{receipt, shared_from_this(), index})) {
                throw std::logic_error("HTTP server acceptor is stopping");
            }
        }
#endif
        connections_.remove_listener(index, std::move(receipt), options);
        services_[index].active_ = false;
    }
#ifndef _WIN32
    listener_sockets duplicate(const listener_endpoint& endpoint) {
        require_current();
        require_running();
        const auto index = find(endpoint);
        listener_sockets sockets;
        auto& tcp = std::holds_alternative<http_server_listener_definition::tls>(services_[index].definition_.transport_) ? sockets.https_ : sockets.http_;
        tcp.reserve(1);
        sockets.http3_.reserve(1);
        const auto result = connections_.duplicate_listener(index);
        tcp.push_back(result.tcp_);
        if (result.udp_ != invalid_native_socket) {
            sockets.http3_.push_back(result.udp_);
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
            if (!loop_.defer_cleanup([self] { self->begin_close(); })) {
                if (io_started_.load(std::memory_order_acquire)) {
                    std::terminate();
                }
                finish_closed();
            }
        } catch (...) {
            std::terminate();
        }
    }
    task<void> shutdown() {
        return shutdown_serialized(shared_from_this());
    }
    http_server_stats stats() const {
        auto result = connections_.stats();
        result.worker_failures_ = fatal_count_.load(std::memory_order_relaxed);
        return result;
    }
    const worker_ref& worker() const noexcept {
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
            if (services_[index].active_ && services_[index].published_ && services_[index].definition_.endpoint_ == target) {
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
    void validate_new(const definitions& definitions_value) const {
        require_open();
        for (const auto& existing : services_) {
            if (existing.active_) {
                ensure_new_listener_endpoints(std::span<const http_server_listener_definition>(&existing.definition_, 1), definitions_value);
            }
        }
    }
    void append(const definitions& definitions_value, bool external) {
        validate_new(definitions_value);
        services_.reserve(services_.size() + definitions_value.size());
        // Clone before mutating the live list.
        std::vector<service> added;
        added.reserve(definitions_value.size());
        auto next = http3_slots_;
        for (const auto& definition : definitions_value) {
            const auto slot = definition.http3_ ? std::optional<std::size_t>(next++) : std::nullopt;
            added.push_back({definition.clone(), external, true, slot});
        }
        for (auto& entry : added) {
            services_.push_back(std::move(entry));
        }
        http3_slots_ = next;
    }
#if defined(_WIN32)
    template <typename callable_type>
    static task<void> on_acceptor(std::shared_ptr<http_server_state> self, callable_type callable) {
        runtime_control_completion completion(self->worker_);
        bool posted = false;
        std::exception_ptr posting_failure;
        try {
            posted = self->acceptor_->post_control(completion.make_command(
                [self, callable = std::move(callable)](const auto& receipt) mutable {
                    try {
                        callable();
                        receipt.complete();
                    } catch (...) {
                        receipt.complete(std::current_exception());
                    }
                }));
        } catch (...) {
            posting_failure = std::current_exception();
        }
        co_await completion.wait();
        if (const auto failure = completion.failure()) {
            std::rethrow_exception(failure);
        }
        if (posting_failure) {
            std::rethrow_exception(posting_failure);
        }
        if (!posted || completion.completed() != 1) {
            throw std::logic_error("HTTP server acceptor is stopping");
        }
    }
    task<void> ensure_acceptor() {
        if (grouped_ || acceptor_) {
            co_return;
        }
        bool needed = false;
        for (const auto& entry : services_) {
            needed |= entry.active_ && !entry.external_;
        }
        if (!needed) {
            co_return;
        }
        acceptor_ = std::make_unique<windows_acceptor>(std::span<const http_server_listener_definition>{}, 1,
#if defined(RUVIA_ENABLE_HTTP3)
            directory_.get(),
#endif
            worker_failure_sink{.target_ = this, .invoke_ = &fail});
        connections_.bind_acceptor(*acceptor_);
        acceptor_->bind_worker(0, connections_.acceptor_target());
        co_await acceptor_->start(worker_);
        co_await acceptor_->serve(worker_);
    }
#endif
    static task<void> prepare_range(std::shared_ptr<http_server_state> self, std::size_t first, sessions prepared) {
#if defined(RUVIA_ENABLE_HTTP3)
        for (const auto& entry : self->services_) {
            if (entry.http3_slot_ && !self->directory_) {
                self->directory_ = std::make_unique<http3_worker_directory>(1);
                self->directory_->bind_worker(0, self->worker_);
                break;
            }
        }
#endif
#if defined(_WIN32)
        co_await self->ensure_acceptor();
#endif
        for (std::size_t offset = 0; offset != prepared.size(); ++offset) {
            self->require_open();
            const auto index = first + offset;
            auto& configured = self->services_[index];
            if (!configured.active_) {
                continue;
            }
            self->connections_.prepare_listener(index, configured.http3_slot_, configured.definition_, configured.external_, std::move(prepared[offset])
#if defined(RUVIA_ENABLE_HTTP3)
                                                                                                                                 ,
                self->directory_.get()
#endif
            );
#if defined(_WIN32)
            if (!configured.external_ && (!self->grouped_ || self->worker_index_ == 0)) {
                co_await on_acceptor(self, [self, index] {
                    auto& entry = self->services_[index];
                    self->acceptor_->prepare_listener(index, entry.http3_slot_, entry.definition_
#if defined(RUVIA_ENABLE_HTTP3)
                        ,
                        self->directory_.get()
#endif
                    );
                });
            }
#endif
            configured.definition_.inherited_sockets_.reset();
        }
        co_return;
    }
    static task<void> open_range(std::shared_ptr<http_server_state> self, std::size_t first,
        std::shared_ptr<prepared_listener_state> transaction = {}) {
#if defined(_WIN32)
        runtime_control_completion completion(self->worker_);
        auto command = completion.make_command([self, first](const auto& receipt) noexcept {
            for (auto index = first; index != self->services_.size(); ++index) {
                if (self->services_[index].active_ && !self->services_[index].external_) {
                    self->acceptor_->open_listener(index);
                }
            }
            receipt.complete();
        });
        move_only_function<void()> accepting(std::move(command));
#endif
        if (transaction) {
            co_await transaction->checkpoint(self->token_);
        }
        self->require_open();
        for (auto index = first; index != self->services_.size(); ++index) {
            if (self->services_[index].active_) {
                self->connections_.open_listener(index);
                self->services_[index].published_ = true;
            }
        }
#if defined(_WIN32)
        const bool posted = self->acceptor_ ? self->acceptor_->post_control(std::move(accepting)) : true;
        accepting = nullptr;
        co_await completion.wait();
        (void)completion.failure();
        if (!posted) {
            throw std::logic_error("HTTP server acceptor is stopping");
        }
#endif
        self->require_open();
    }
    static task<void> discard_range(std::shared_ptr<http_server_state> self, std::size_t first) {
        runtime_control_completion retired(self->worker_);
        auto receipt = std::make_shared<runtime_control_completion::receipt>(retired.make_receipt());
        for (auto index = first; index != self->services_.size(); ++index) {
            self->connections_.discard_listener(index, receipt);
            self->services_[index].active_ = false;
            self->services_[index].definition_.inherited_sockets_.reset();
#if defined(_WIN32)
            if (self->acceptor_ && !self->services_[index].external_ && (!self->grouped_ || self->worker_index_ == 0)) {
                co_await on_acceptor(self, [self, index, receipt] { self->acceptor_->discard_listener(index, receipt); });
            }
#endif
        }
        receipt.reset();
        co_await retired.wait();
        (void)retired.failure();
    }
    static task<void> prepare_start_owned(std::shared_ptr<http_server_state> self, sessions prepared) {
        self->require_current();
        self->require_open();
        if (self->started_ || self->starting_) {
            throw std::logic_error("HTTP server can only start once");
        }
        self->starting_ = true;
        activity active(*self);
        self->phase_ = http_server_worker_state::running;
        std::exception_ptr failure;
        try {
            self->io_started_.store(true, std::memory_order_release);
            self->connections_.prepare();
            co_await prepare_range(self, 0, std::move(prepared));
            co_await self->capabilities_.start();
            self->require_open();
        } catch (...) {
            failure = std::current_exception();
        }
        self->starting_ = false;
        self->changed_.notify();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    static task<void> start_owned(std::shared_ptr<http_server_state> self) {
        self->require_current();
        auto lease = co_await self->gate_.acquire(self->worker_);
        self->require_open();
        if (self->started_ || self->starting_) {
            throw std::logic_error("HTTP server can only start once");
        }
        std::exception_ptr failure;
        {
            activity active(*self);
            self->starting_ = true;
            try {
                auto inputs = std::move(self->staged_);
                auto existing = std::make_shared<listener_candidate>();
                for (const auto& entry : self->services_) {
                    existing->definitions_.push_back(entry.definition_.clone());
                }
                const bool blocking = needs_tls_preparation(inputs, existing->definitions_);
                const auto first = co_await prepare_work(self->worker_, blocking, [inputs = std::move(inputs), existing, config = self->options_.server_, callbacks = self->callbacks_, origin = self->key_origin_]() mutable {
                    auto loaded = load_candidate(inputs, false, config, callbacks, 1, origin);
                    const auto first = existing->definitions_.size();
                    ensure_new_listener_endpoints(existing->definitions_, loaded.definitions_);
                    for (auto& definition : loaded.definitions_) {
                        existing->definitions_.push_back(std::move(definition));
                    }
                    prepare_sessions(*existing, 1, static_cast<bool>(callbacks->tls_client_hello_));
                    return first;
                });
                self->require_open();
                definitions added;
                for (auto index = first; index != existing->definitions_.size(); ++index) {
                    added.push_back(existing->definitions_[index].clone());
                }
                self->append(added, false);
                self->starting_ = false;
                co_await prepare_start_owned(self, std::move(existing->sessions_.front()));
                co_await open_range(self, 0);
                self->started_ = true;
            } catch (...) {
                failure = std::current_exception();
            }
            self->starting_ = false;
            self->changed_.notify();
        }
        if (failure) {
            self->request_close();
            co_await shutdown_owned(self);
            std::rethrow_exception(failure);
        }
    }
    static task<void> add_owned(std::shared_ptr<http_server_state> self, std::shared_ptr<owned_listen_configs> input, bool external,
        std::shared_ptr<prepared_listener_state> transaction = {}) {
        self->require_current();
        auto lease = co_await self->gate_.acquire(self->worker_);
        self->require_open();
        if (!external) {
            self->require_running();
        }
        activity active(*self);
        const bool blocking = input->needs_tls_preparation();
        auto candidate = co_await prepare_work(self->worker_, blocking, [input = std::move(input), config = self->options_.server_, callbacks = self->callbacks_, origin = self->key_origin_, external, running = self->started_] {
            auto candidate = load_candidate({input}, external, config, callbacks, 1, origin);
            if (running) {
                prepare_sessions(candidate, 1, static_cast<bool>(callbacks->tls_client_hello_));
            }
            return candidate;
        });
        self->require_open();
        const auto first = self->services_.size();
        self->append(candidate.definitions_, external);
        if (!self->started_) {
            co_return;
        }
        std::exception_ptr failure;
        try {
            co_await prepare_range(self, first, std::move(candidate.sessions_.front()));
            co_await open_range(self, first, std::move(transaction));
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            try {
                co_await discard_range(self, first);
            } catch (...) {
                self->request_close();
            }
            std::rethrow_exception(failure);
        }
    }
    static task<void> wait_connections_owned(std::shared_ptr<http_server_state> self, owned_endpoint endpoint, operation_options options) {
        self->require_current();
        self->require_running();
        activity active(*self);
        co_await connection_count::wait(self->connections_.listener_connections(service_endpoint(endpoint.view())),
            self->worker_, std::move(options), self->token_);
    }
    static task<void> replace_owned(std::shared_ptr<http_server_state> self, owned_endpoint endpoint,
        std::shared_ptr<owned_listen_configs> replacement) {
        self->require_current();
        // Declared before the lease so a selector destructor can reenter control.
        std::shared_ptr<const http_server_tls_identities> retired;
        auto lease = co_await self->gate_.acquire(self->worker_);
        self->require_running();
        activity active(*self);
        const auto index = self->find(endpoint.view());
        const auto* policy = std::get_if<http_server_listener_definition::tls>(&self->services_[index].definition_.transport_);
        if (!policy) {
            throw std::invalid_argument("the connection service does not use TLS");
        }
        ensure_same_http_server_tls_policy(*policy, replacement->configs_.front().tls_);
        auto identities = co_await prepare_work(self->worker_, true, [replacement] {
            auto loaded = replacement->load();
            return std::get<http_server_listener_definition::tls>(loaded.front().transport_).identities_;
        });
        self->require_running();
        if (self->find(endpoint.view()) != index) {
            throw std::logic_error("TLS service changed during preparation");
        }
        auto& current = std::get<http_server_listener_definition::tls>(self->services_[index].definition_.transport_);
        retired = current.identities_;
        self->connections_.adopt_tls_identities(index, identities);
        current.identities_ = std::move(identities);
    }
    void finish_closed() noexcept {
        std::vector<runtime_control_completion::receipt> waiters;
        {
            std::lock_guard lock(closed_mutex_);
            closed_.store(true, std::memory_order_release);
            waiters.swap(closed_waiters_);
        }
        for (const auto& waiter : waiters) {
            waiter.complete();
        }
        group_retirement_.reset();
    }
    void subscribe_closed(runtime_control_completion::receipt receipt) {
        {
            std::lock_guard lock(closed_mutex_);
            if (!closed_.load(std::memory_order_acquire)) {
                closed_waiters_.push_back(std::move(receipt));
                return;
            }
        }
        receipt.complete();
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
            ruvia::async_start(cleanup(), asio::bind_executor(loop_.executor(), [self = shared_from_this()](std::exception_ptr failure) noexcept {
                if (failure) {
                    {
                        std::lock_guard lock(self->failure_mutex_);
                        if (!self->failure_) {
                            self->failure_ = failure;
                        }
                    }
                    fail_server_group(self->group_, failure);
                }
                self->finish_closed();
                self->changed_.notify();
            }));
        } catch (...) {
            std::terminate();
        }
    }
    task<void> cleanup() {
        while (starting_ || controls_) {
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
        // One native thread barrier per acceptor. The first member's cleanup
        // remains live on its loop until the HANDLE wait and reclaim finish.
        if (acceptor_ && (!grouped_ || worker_index_ == 0)) {
            acceptor_->request_stop();
            co_await acceptor_->join(worker_, loop_.io_context());
        }
#endif
        staged_.clear();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    static task<void> shutdown_owned(std::shared_ptr<http_server_state> self, bool observe = true) {
        self->require_current();
        self->request_close();
        runtime_control_completion completion(self->worker_);
        self->subscribe_closed(completion.make_receipt());
        co_await completion.wait();
        (void)completion.failure();
        if (!observe) {
            co_return;
        }
        std::exception_ptr failure;
        {
            std::lock_guard lock(self->failure_mutex_);
            failure = self->failure_;
            self->failure_observed_ = true;
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    static task<void> shutdown_serialized(std::shared_ptr<http_server_state> self) {
        self->require_current();
        self->request_close();
        auto lease = co_await self->gate_.acquire(self->worker_);
        co_await shutdown_owned(self);
    }

    event_loop loop_;
    worker_ref worker_;
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
    std::vector<service> services_;
    std::vector<std::shared_ptr<owned_listen_configs>> staged_;
    std::size_t http3_slots_{};
    std::chrono::steady_clock::time_point key_origin_{std::chrono::steady_clock::now()};
    worker_signal changed_;
    runtime_control_gate gate_;
    std::size_t controls_{};
    bool starting_{};
    bool started_{};
    bool close_started_{};
    std::atomic<bool> closing_{};
    std::atomic<bool> closed_{};
    std::atomic<bool> io_started_{};
    std::mutex closed_mutex_;
    std::vector<runtime_control_completion::receipt> closed_waiters_;
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
    event_loop_stop_registration stop_registration_;
};

class http_server_group_state final : public std::enable_shared_from_this<http_server_group_state> {
public:
    http_server_group_state(std::span<const event_loop> loops, http_server_callbacks_factory callbacks, server_config config)
        : callbacks_factory_(std::move(callbacks)) {
        if (!callbacks_factory_) {
            throw std::invalid_argument("HTTP server group requires a callback factory");
        }
        if (loops.empty()) {
            throw std::invalid_argument("HTTP server group requires at least one event loop");
        }
        for (std::size_t index = 0; index != loops.size(); ++index) {
            if (!loops[index].valid() || !loops[index].worker().accepting()) {
                throw std::invalid_argument("HTTP server group requires valid event loops");
            }
            for (std::size_t previous = 0; previous != index; ++previous) {
                if (loops[index].worker().id() == loops[previous].worker().id()) {
                    throw std::invalid_argument("HTTP server group requires distinct event loops");
                }
            }
        }
#if defined(RUVIA_ENABLE_HTTP3)
        directory_ = std::make_shared<http3_worker_directory>(loops.size());
#endif
        workers_.reserve(loops.size());
        for (std::size_t index = 0; index != loops.size(); ++index) {
            auto worker = std::make_shared<http_server_state>(loops[index], std::make_shared<http_server_callbacks>(), config, index, loops.size()
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
        acceptor_ = std::make_shared<windows_acceptor>(std::span<const http_server_listener_definition>{}, loops.size(),
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
    void stage(std::shared_ptr<owned_listen_configs> input) {
        std::lock_guard lock(config_mutex_);
        require_open();
        if (startup_requested_) {
            throw std::logic_error("listen() is only available before HTTP server group startup");
        }
        staged_.push_back(std::move(input));
    }
    void trusted_proxies(trusted_proxy_config config) {
        std::lock_guard lock(config_mutex_);
        require_open();
        if (startup_requested_) {
            throw std::logic_error("cannot change trusted proxies after HTTP server group startup");
        }
        http_server_options validated;
        validated.set_trusted_proxies(config);
        std::vector<http_client_ip_extractor> candidates;
        candidates.reserve(workers_.size());
        for (std::size_t index = 0; index != workers_.size(); ++index) {
            candidates.emplace_back();
            candidates.back() = validated.trusted_proxies_;
        }
        for (std::size_t index = 0; index != workers_.size(); ++index) {
            workers_[index]->options_.trusted_proxies_ = std::move(candidates[index]);
        }
    }
    task<void> start(worker_ref caller) {
        return start_owned(shared_from_this(), std::move(caller));
    }
    task<void> add(worker_ref caller, std::shared_ptr<owned_listen_configs> input, bool external) {
        return add_owned(shared_from_this(), std::move(caller), std::move(input), external);
    }
    task<prepared_listener> prepare(worker_ref caller, std::shared_ptr<owned_listen_configs> input) {
        auto transaction = prepared_listener_access::make(caller);
        return prepared_listener_access::launch(transaction, add_owned(shared_from_this(), std::move(caller), std::move(input), false, transaction));
    }
    task<void> remove(worker_ref caller, owned_endpoint endpoint, listener_remove_options options) {
        return remove_owned(shared_from_this(), std::move(caller), std::move(endpoint), options);
    }
    task<void> replace(worker_ref caller, owned_endpoint endpoint, std::shared_ptr<owned_listen_configs> replacement) {
        return replace_owned(shared_from_this(), std::move(caller), std::move(endpoint), std::move(replacement));
    }
    task<bool> close_connection(worker_ref caller, std::uint64_t id) {
        return close_connection_owned(shared_from_this(), std::move(caller), id);
    }
    task<void> wait_connections(worker_ref caller, owned_endpoint endpoint, operation_options options) {
        return wait_connections_owned(shared_from_this(), std::move(caller), std::move(endpoint), std::move(options));
    }
#ifndef _WIN32
    task<listener_sockets> duplicate(worker_ref caller, owned_endpoint endpoint) {
        return duplicate_owned(shared_from_this(), std::move(caller), std::move(endpoint));
    }
#endif
    task<void> adopt(worker_ref caller, std::size_t index, owned_endpoint endpoint, socket_owner socket, owned_adopt_options options) {
        return adopt_owned(shared_from_this(), std::move(caller), index, std::move(endpoint), std::move(socket), std::move(options));
    }
    task<void> shutdown(worker_ref caller) {
        return shutdown_owned(shared_from_this(), std::move(caller));
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
    http_server_stats stats() const {
        http_server_stats result;
        for (const auto& worker : workers_) {
            const auto value = worker->stats();
            result.active_connections_ += value.active_connections_;
            result.connections_refused_ += value.connections_refused_;
            result.connection_failures_ += value.connection_failures_;
            result.accept_failures_ += value.accept_failures_;
            result.worker_failures_ += value.worker_failures_;
            append_listener_stats(result, value);
        }
        return result;
    }
    std::size_t worker_count() const noexcept {
        return workers_.size();
    }
    const worker_ref& worker(std::size_t index) const {
        return workers_.at(index)->worker_;
    }

private:
    task<void> build_callbacks(worker_ref caller) {
        if (!callbacks_factory_) {
            co_return;
        }
        std::exception_ptr failure;
        try {
            for (const auto& worker : workers_) {
                co_await control(caller, worker, [this](http_server_state& state) {
                    auto callbacks = callbacks_factory_(state.worker_);
                    *state.callbacks_ = std::move(callbacks);
                    const auto configured = standalone_options(state.options_.server_, *state.callbacks_, workers_.size());
                    state.options_.connection_ = configured.connection_;
                    state.options_.request_stream_ = configured.request_stream_;
                    state.options_.access_log_.callback_ = configured.access_log_.callback_;
                    state.options_.connection_failure_.callback_ = configured.connection_failure_.callback_;
                    state.options_.tls_client_hello_ = configured.tls_client_hello_;
                    state.options_.http2_input_ = configured.http2_input_;
#if defined(RUVIA_ENABLE_HTTP2)
                    state.options_.http2_frame_ = configured.http2_frame_;
                    state.options_.http2_headers_ = configured.http2_headers_;
#endif
                    state.options_.connection_admission_ = configured.connection_admission_;
                    state.options_.connection_open_ = configured.connection_open_;
                    state.options_.connection_close_ = configured.connection_close_;
                    if (&state != workers_.front().get()) {
                        const auto& first = *workers_.front()->callbacks_;
                        const auto& current = *state.callbacks_;
                        if (static_cast<bool>(first.connection_) != static_cast<bool>(current.connection_) ||
                            static_cast<bool>(first.request_stream_) != static_cast<bool>(current.request_stream_) ||
                            static_cast<bool>(first.access_) != static_cast<bool>(current.access_) ||
                            static_cast<bool>(first.connection_failure_) != static_cast<bool>(current.connection_failure_) ||
                            static_cast<bool>(first.tls_client_hello_) != static_cast<bool>(current.tls_client_hello_) ||
                            static_cast<bool>(first.http2_input_) != static_cast<bool>(current.http2_input_) ||
#if defined(RUVIA_ENABLE_HTTP2)
                            static_cast<bool>(first.http2_frame_) != static_cast<bool>(current.http2_frame_) ||
                            static_cast<bool>(first.http2_headers_) != static_cast<bool>(current.http2_headers_) ||
#endif
                            static_cast<bool>(first.connection_admission_) != static_cast<bool>(current.connection_admission_) ||
                            static_cast<bool>(first.connection_open_) != static_cast<bool>(current.connection_open_) ||
                            static_cast<bool>(first.connection_close_) != static_cast<bool>(current.connection_close_)) {
                            throw std::invalid_argument("HTTP server group callbacks must have identical presence");
                        }
                    }
                });
            }
        } catch (...) {
            failure = std::current_exception();
        }
        callbacks_factory_ = {};
        if (failure) {
            close();
            co_await join_closed(caller);
            std::rethrow_exception(failure);
        }
    }
    // Every closure owns its receipt, including the spawned coroutine callback.
    // A failed post is joined as well, before any borrowed command state dies.
    template <typename function_type>
    static task<void> control(worker_ref caller, std::shared_ptr<http_server_state> worker, function_type function) {
        runtime_control_completion completion(caller);
        bool posted = false;
        std::exception_ptr posting_failure;
        try {
            posted = worker->loop_.defer_cleanup(completion.make_command(
                [worker, function = std::move(function)](const auto& receipt) mutable {
                    try {
                        function(*worker);
                        receipt.complete();
                    } catch (...) {
                        receipt.complete(std::current_exception());
                    }
                }));
        } catch (...) {
            posting_failure = std::current_exception();
        }
        co_await completion.wait();
        if (const auto failure = completion.failure()) {
            std::rethrow_exception(failure);
        }
        if (posting_failure) {
            std::rethrow_exception(posting_failure);
        }
        if (!posted || completion.completed() != 1) {
            throw std::logic_error("HTTP server group worker has retired");
        }
    }
    template <typename function_type>
    static task<void> async_control(worker_ref caller, std::shared_ptr<http_server_state> worker, function_type function) {
        runtime_control_completion completion(caller);
        bool posted = false;
        std::exception_ptr posting_failure;
        try {
            posted = worker->loop_.defer_cleanup(completion.make_command(
                [worker, function = std::move(function)](const auto& receipt) mutable {
                    try {
                        // Keep the callable address-stable until the task and
                        // its completion callback have both been reclaimed.
                        auto operation = std::make_shared<function_type>(std::move(function));
                        auto finish = receipt;
                        ++worker->controls_;
                        try {
                            ruvia::async_start((*operation)(worker), asio::bind_executor(worker->loop_.executor(), std::move(finish).bind([worker, operation](const auto& done, std::exception_ptr failure) {
                                --worker->controls_;
                                worker->changed_.notify();
                                done.complete(failure);
                            })));
                        } catch (...) {
                            --worker->controls_;
                            worker->changed_.notify();
                            throw;
                        }
                    } catch (...) {
                        receipt.complete(std::current_exception());
                    }
                }));
        } catch (...) {
            posting_failure = std::current_exception();
        }
        co_await completion.wait();
        if (const auto failure = completion.failure()) {
            std::rethrow_exception(failure);
        }
        if (posting_failure) {
            std::rethrow_exception(posting_failure);
        }
        if (!posted || completion.completed() != 1) {
            throw std::logic_error("HTTP server group worker has retired");
        }
    }
    static task<void> open_prepared(std::shared_ptr<http_server_group_state> self, worker_ref caller,
        std::size_t first, std::shared_ptr<prepared_listener_state> transaction = {}) {
        runtime_control_completion completion(caller);
        std::vector<move_only_function<void()>> commands;
        commands.reserve(self->workers_.size());
        for (const auto& worker : self->workers_) {
            commands.emplace_back(completion.make_command([worker, first](const auto& receipt) noexcept {
                if (!worker->closing_.load(std::memory_order_acquire)) {
                    for (auto index = first; index != worker->services_.size(); ++index) {
                        if (worker->services_[index].active_) {
                            worker->connections_.open_listener(index);
                            worker->services_[index].published_ = true;
                        }
                    }
                    worker->started_ = true;
                }
                receipt.complete();
            }));
        }
#if defined(_WIN32)
        runtime_control_completion accepting_completion(caller);
        move_only_function<void()> accepting(accepting_completion.make_command([self, first](const auto& receipt) noexcept {
            const auto& services = self->workers_.front()->services_;
            for (auto index = first; index != services.size(); ++index) {
                if (services[index].active_ && !services[index].external_) {
                    self->acceptor_->open_listener(index);
                }
            }
            receipt.complete();
        }));
#endif
        if (transaction) {
            co_await transaction->checkpoint(self->workers_.front()->token_);
        }
        self->require_open();
        bool posted = true;
        std::exception_ptr failure;
        try {
            for (std::size_t index = 0; index != commands.size() && posted; ++index) {
                posted = self->workers_[index]->loop_.defer_cleanup(std::move(commands[index]));
            }
        } catch (...) {
            failure = std::current_exception();
        }
        commands.clear();
        co_await completion.wait();
        (void)completion.failure();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (!posted) {
            throw std::logic_error("HTTP server group worker has retired");
        }
        self->require_open();
#if defined(_WIN32)
        posted = self->acceptor_->post_control(std::move(accepting));
        accepting = nullptr;
        co_await accepting_completion.wait();
        (void)accepting_completion.failure();
        if (!posted) {
            throw std::logic_error("HTTP server acceptor is stopping");
        }
#endif
        self->require_open();
    }
    static task<void> start_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller) {
        auto lease = co_await self->gate_.acquire(caller);
        self->require_open();
        std::vector<std::shared_ptr<owned_listen_configs>> inputs;
        {
            std::lock_guard lock(self->config_mutex_);
            if (self->startup_requested_) {
                throw std::logic_error("HTTP server group can only start once");
            }
            self->startup_requested_ = true;
            inputs.swap(self->staged_);
        }
        std::exception_ptr failure;
        try {
            co_await self->build_callbacks(caller);
            auto existing = std::make_shared<listener_candidate>();
            co_await control(caller, self->workers_.front(), [existing](http_server_state& state) {
                for (const auto& entry : state.services_) {
                    existing->definitions_.push_back(entry.definition_.clone());
                }
            });
            const auto first = existing->definitions_.size();
            const auto& source = self->workers_.front();
            const bool blocking = needs_tls_preparation(inputs, existing->definitions_);
            co_await prepare_work(caller, blocking, [inputs = std::move(inputs), existing, config = source->options_.server_, callbacks = source->callbacks_, count = self->workers_.size(), origin = source->key_origin_]() mutable {
                auto loaded = load_candidate(inputs, false, config, callbacks, count, origin);
                ensure_new_listener_endpoints(existing->definitions_, loaded.definitions_);
                for (auto& definition : loaded.definitions_) {
                    existing->definitions_.push_back(std::move(definition));
                }
                prepare_sessions(*existing, count, static_cast<bool>(callbacks->tls_client_hello_));
            });
            self->require_open();
            auto added = std::make_shared<definitions>();
            for (auto index = first; index != existing->definitions_.size(); ++index) {
                added->push_back(existing->definitions_[index].clone());
            }
            for (const auto& worker : self->workers_) {
                co_await control(caller, worker, [added](http_server_state& state) { state.append(*added, false); });
            }
            self->listener_slots_ = existing->definitions_.size();
#if defined(_WIN32)
            co_await self->acceptor_->start(caller);
            co_await self->acceptor_->serve(caller);
#endif
            // Every owner prepares, but none admits before the final prepare.
            for (std::size_t index = 0; index != self->workers_.size(); ++index) {
                co_await async_control(caller, self->workers_[index], [prepared = std::move(existing->sessions_[index])](auto worker) mutable {
                    return http_server_state::prepare_start_owned(std::move(worker), std::move(prepared));
                });
            }
            if (!caller.accepting()) {
                throw std::logic_error("HTTP server caller is stopping");
            }
            self->require_open();
            co_await open_prepared(self, caller, 0);
            self->require_open();
            self->started_ = true;
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            self->close();
            co_await self->join_closed(caller);
            if (const auto fatal = self->observe_failure()) {
                std::rethrow_exception(fatal);
            }
            std::rethrow_exception(failure);
        }
    }
    static task<void> add_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller,
        std::shared_ptr<owned_listen_configs> input, bool external, std::shared_ptr<prepared_listener_state> transaction = {}) {
        auto lease = co_await self->gate_.acquire(caller);
        self->require_open();
        if (!external) {
            self->require_running();
        }
        co_await self->build_callbacks(caller);
        const auto& source = self->workers_.front();
        const bool blocking = input->needs_tls_preparation();
        auto prepared = co_await prepare_work(caller, blocking, [input = std::move(input), external, config = source->options_.server_, callbacks = source->callbacks_, count = self->workers_.size(), origin = source->key_origin_, running = self->started_] {
            auto candidate = load_candidate({input}, external, config, callbacks, count, origin);
            if (running) {
                prepare_sessions(candidate, count, static_cast<bool>(callbacks->tls_client_hello_));
            }
            return candidate;
        });
        auto candidate = std::make_shared<listener_candidate>(std::move(prepared));
        self->require_open();
        for (const auto& worker : self->workers_) {
            co_await control(caller, worker, [candidate](http_server_state& state) {
                state.validate_new(candidate->definitions_);
                state.services_.reserve(state.services_.size() + candidate->definitions_.size());
            });
        }
        auto staged = std::make_shared<std::vector<std::vector<http_server_state::service>>>();
        staged->reserve(self->workers_.size());
        auto next_http3 = source->http3_slots_;
        for (std::size_t worker = 0; worker != self->workers_.size(); ++worker) {
            auto& entries = staged->emplace_back();
            entries.reserve(candidate->definitions_.size());
            next_http3 = source->http3_slots_;
            for (const auto& definition : candidate->definitions_) {
                const auto slot = definition.http3_ ? std::optional<std::size_t>(next_http3++) : std::nullopt;
                entries.push_back({definition.clone(), external, true, slot});
            }
        }
        const auto first = self->listener_slots_;
        std::exception_ptr failure;
        bool appended = false;
        if (!caller.accepting()) {
            throw std::logic_error("HTTP server caller is stopping");
        }
        try {
            for (std::size_t index = 0; index != self->workers_.size(); ++index) {
                co_await control(caller, self->workers_[index], [staged, index, next_http3](http_server_state& state) {
                    state.require_open();
                    for (auto& entry : (*staged)[index]) {
                        state.services_.push_back(std::move(entry));
                    }
                    state.http3_slots_ = next_http3;
                });
            }
            self->listener_slots_ += candidate->definitions_.size();
            appended = true;
            if (!self->started_) {
                co_return;
            }
            for (std::size_t index = 0; index != self->workers_.size(); ++index) {
                co_await async_control(caller, self->workers_[index], [first, prepared = std::move(candidate->sessions_[index])](auto owner) mutable {
                    return http_server_state::prepare_range(std::move(owner), first, std::move(prepared));
                });
            }
            if (!caller.accepting()) {
                throw std::logic_error("HTTP server caller is stopping");
            }
            self->require_open();
            co_await open_prepared(self, caller, first, std::move(transaction));
            self->require_open();
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            if (!appended) {
                self->close();
                co_await self->join_closed(caller);
                std::rethrow_exception(failure);
            }
            for (const auto& worker : self->workers_) {
                try {
                    co_await async_control(caller, worker, [first](auto owner) { return http_server_state::discard_range(std::move(owner), first); });
                } catch (...) {
                    self->close();
                }
            }
#if defined(RUVIA_ENABLE_HTTP3) && !defined(_WIN32)
            for (std::size_t index = 0; index != self->workers_.front()->http3_slots_; ++index) {
                self->directory_->close_unclaimed_listener_sockets(index);
            }
#endif
            std::rethrow_exception(failure);
        }
    }
    static task<void> remove_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller, owned_endpoint endpoint, listener_remove_options options) {
        auto lease = co_await self->gate_.acquire(caller);
        self->require_running();
        // Removal's native accepts hold this receipt until their completion.
        runtime_control_completion completion(caller);
        auto lifetime = std::make_shared<runtime_control_completion::receipt>(completion.make_receipt());
        std::exception_ptr failure;
        try {
            for (const auto& worker : self->workers_) {
                co_await control(caller, worker, [&endpoint, lifetime, options](http_server_state& state) { state.remove(endpoint.view(), lifetime, options); });
            }
        } catch (...) {
            failure = std::current_exception();
        }
        lifetime.reset();
        co_await completion.wait();
        (void)completion.failure();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    static task<bool> close_connection_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller, std::uint64_t id) {
        auto lease = co_await self->gate_.acquire(caller);
        self->require_running();
        for (const auto& worker : self->workers_) {
            bool closed = false;
            co_await control(caller, worker, [&closed, id](http_server_state& state) { closed = state.close_connection(id); });
            if (closed) {
                co_return true;
            }
        }
        co_return false;
    }
    static task<void> replace_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller,
        owned_endpoint endpoint, std::shared_ptr<owned_listen_configs> replacement) {
        struct target final {
            std::size_t slot_;
            std::shared_ptr<const http_server_tls_identities> retired_;
        };
        std::vector<target> targets;
        auto lease = co_await self->gate_.acquire(caller);
        self->require_running();
        targets.reserve(self->workers_.size());
        for (const auto& worker : self->workers_) {
            co_await control(caller, worker, [&targets, &endpoint, replacement](http_server_state& state) {
                state.require_running();
                const auto slot = state.find(endpoint.view());
                const auto* policy = std::get_if<http_server_listener_definition::tls>(&state.services_[slot].definition_.transport_);
                if (!policy) {
                    throw std::invalid_argument("the connection service does not use TLS");
                }
                ensure_same_http_server_tls_policy(*policy, replacement->configs_.front().tls_);
                targets.push_back({slot, policy->identities_});
            });
        }
        auto identities = co_await prepare_work(caller, true,
            [replacement] {
                auto loaded = replacement->load();
                return std::get<http_server_listener_definition::tls>(loaded.front().transport_).identities_;
            });
        self->require_running();
        for (std::size_t index = 0; index != self->workers_.size(); ++index) {
            co_await control(caller, self->workers_[index], [slot = targets[index].slot_, identities](http_server_state& state) {
                state.require_running();
                state.connections_.adopt_tls_identities(slot, identities);
                std::get<http_server_listener_definition::tls>(state.services_[slot].definition_.transport_).identities_ = identities;
            });
        }
    }
#ifndef _WIN32
    static task<listener_sockets> duplicate_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller, owned_endpoint endpoint) {
        auto lease = co_await self->gate_.acquire(caller);
        self->require_running();
        listener_sockets result;
        result.http_.reserve(self->workers_.size());
        result.https_.reserve(self->workers_.size());
        result.http3_.reserve(self->workers_.size());
        try {
            for (const auto& worker : self->workers_) {
                co_await control(caller, worker, [&result, &endpoint](http_server_state& state) {
                    auto sockets = state.duplicate(endpoint.view());
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
        co_return result;
    }
#endif
    static task<void> adopt_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller,
        std::size_t index, owned_endpoint endpoint, socket_owner socket, owned_adopt_options options) {
        auto lease = co_await self->gate_.acquire(caller);
        self->require_running();
        if (index >= self->workers_.size()) {
            throw std::out_of_range("HTTP server group worker index");
        }
        co_await control(caller, self->workers_[index], [&socket, &endpoint, &options](http_server_state& state) {
            state.adopt(endpoint.view(), std::exchange(socket.socket_, invalid_native_socket), options.view());
        });
    }
    static task<void> wait_connections_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller,
        owned_endpoint endpoint, operation_options options) {
        operation_steps steps(options);
        std::vector<std::shared_ptr<connection_count>> counts;
        std::vector<stop_token> stops;
        {
            auto lease = co_await self->gate_.acquire(caller);
            self->require_running();
            const auto target = service_endpoint(endpoint.view());
            counts.reserve(self->workers_.size());
            stops.reserve(self->workers_.size());
            for (const auto& worker : self->workers_) {
                counts.push_back(worker->connections_.listener_connections(target));
                stops.push_back(worker->token_);
            }
        }
        stop_group stopping(stops);
        for (const auto& count : counts) {
            co_await connection_count::wait(count, caller, steps.next("wait_connections_closed"), stopping.token());
        }
    }
    static task<void> shutdown_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller) {
        self->close();
        auto lease = co_await self->gate_.acquire(caller);
        co_await self->join_closed(caller);
        if (const auto failure = self->observe_failure()) {
            std::rethrow_exception(failure);
        }
    }
    task<void> join_closed(worker_ref caller) {
        runtime_control_completion completion(caller);
        std::exception_ptr registration_failure;
        try {
            for (const auto& worker : workers_) {
                worker->subscribe_closed(completion.make_receipt());
            }
        } catch (...) {
            registration_failure = std::current_exception();
        }
        co_await completion.wait();
        (void)completion.failure();
        if (registration_failure) {
            std::rethrow_exception(registration_failure);
        }
        for (const auto& worker : workers_) {
            for (auto& entry : worker->services_) {
                entry.definition_.inherited_sockets_.reset();
            }
        }
#if defined(RUVIA_ENABLE_HTTP3) && !defined(_WIN32)
        for (std::size_t index = 0; index != workers_.front()->http3_slots_; ++index) {
            directory_->close_unclaimed_listener_sockets(index);
        }
#endif
        std::vector<std::shared_ptr<owned_listen_configs>> staged;
        {
            std::lock_guard lock(config_mutex_);
            staged.swap(staged_);
        }
    }
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
            std::lock_guard lock(worker->failure_mutex_);
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
            if (!worker->loop_.valid() || !worker->loop_.worker().accepting()) {
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
    std::vector<std::shared_ptr<http_server_state>> workers_;
    http_server_callbacks_factory callbacks_factory_;
#if defined(RUVIA_ENABLE_HTTP3)
    std::shared_ptr<http3_worker_directory> directory_;
#endif
#if defined(_WIN32)
    std::shared_ptr<windows_acceptor> acceptor_;
#endif
    runtime_control_gate gate_;
    std::mutex config_mutex_;
    std::vector<std::shared_ptr<owned_listen_configs>> staged_;
    bool startup_requested_{};
    std::atomic<bool> closing_{};
    std::mutex failure_mutex_;
    std::exception_ptr failure_;
    bool failure_observed_{};
    bool started_{};
    std::size_t listener_slots_{};
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

prepared_listener::prepared_listener(std::shared_ptr<detail::prepared_listener_state> state) noexcept
    : state_(std::move(state)) {}
prepared_listener::prepared_listener(prepared_listener&&) noexcept = default;
prepared_listener& prepared_listener::operator=(prepared_listener&& other) noexcept {
    if (this != &other) {
        auto retired = std::exchange(state_, std::move(other.state_));
        if (retired) {
            retired->decide(false);
        }
    }
    return *this;
}
prepared_listener::~prepared_listener() {
    if (auto retired = std::exchange(state_, {})) {
        retired->decide(false);
    }
}
task<void> prepared_listener::activate() && {
    return detail::prepared_listener_access::finish(std::move(*this), true);
}
task<void> prepared_listener::cancel() && {
    return detail::prepared_listener_access::finish(std::move(*this), false);
}
bool prepared_listener::valid() const noexcept {
    return state_ != nullptr;
}

http_server::http_server(event_loop loop, http_server_callbacks callbacks, server_config config)
    : state_(std::make_shared<detail::http_server_state>(std::move(loop), std::make_shared<http_server_callbacks>(std::move(callbacks)), config)) {
    state_->bind_stop();
}
http_server::~http_server() {
    state_->request_close();
}
http_server& http_server::listen(listen_config config) & {
    const auto state = state_;
    state->stage(detail::own_listen(std::move(config)));
    return *this;
}
task<prepared_listener> http_server::prepare_listener(listen_config config) & {
    const auto state = state_;
    return state->prepare(detail::own_listen(std::move(config)));
}
task<prepared_listener> http_server::prepare_listeners(std::span<const listen_config> configs) & {
    const auto state = state_;
    return state->prepare(detail::own_listen(configs));
}
task<void> http_server::add_listener(listen_config config) & {
    return detail::prepared_listener_access::add(prepare_listener(std::move(config)));
}
task<void> http_server::add_connection_service(listen_config config) & {
    const auto state = state_;
    return state->add(detail::own_listen(std::move(config)), true);
}
void http_server::adopt(const listener_endpoint& service, native_socket socket, adopt_options options) & {
    state_->adopt(service, socket, options);
}
bool http_server::close_connection(std::uint64_t id) & {
    return state_->close_connection(id);
}
http_server& http_server::trusted_proxies(trusted_proxy_config config) & {
    state_->trusted_proxies(std::move(config));
    return *this;
}
task<void> http_server::start() & {
    const auto state = state_;
    return state->start();
}
void http_server::remove_listener(const listener_endpoint& endpoint, listener_remove_options options) & {
    state_->remove(endpoint, {}, options);
}
task<void> http_server::wait_connections_closed(const listener_endpoint& endpoint, operation_options options) & {
    return state_->wait_connections(detail::owned_endpoint(endpoint), std::move(options));
}
task<void> http_server::replace_tls_identities(const listener_endpoint& endpoint, tls_config config) & {
    const auto state = state_;
    auto replacement = detail::own_listen(listen_config{.address_ = endpoint.address_, .https_ = endpoint.port_, .tls_ = std::move(config), .dispatch_ = listener_dispatch::connection});
    return state->replace(detail::owned_endpoint(endpoint), std::move(replacement));
}
#ifndef _WIN32
listener_sockets http_server::duplicate_listener_sockets(const listener_endpoint& endpoint) const& {
    return state_->duplicate(endpoint);
}
#endif
void http_server::close() & noexcept {
    state_->request_close();
}
task<void> http_server::shutdown() & {
    const auto state = state_;
    return state->shutdown();
}
http_server_stats http_server::stats() const {
    return state_->stats();
}
const worker_ref& http_server::worker() const& noexcept {
    return state_->worker();
}

http_server_group::http_server_group(std::span<const event_loop> loops, http_server_callbacks_factory callbacks, server_config config)
    : state_(std::make_shared<detail::http_server_group_state>(loops, std::move(callbacks), config)) {
    state_->bind_stop();
}
http_server_group::~http_server_group() {
    state_->close();
}
http_server_group& http_server_group::listen(listen_config config) & {
    const auto state = state_;
    state->stage(detail::own_listen(std::move(config)));
    return *this;
}
task<prepared_listener> http_server_group::prepare_listener(worker_ref caller, listen_config config) & {
    const auto state = state_;
    return state->prepare(std::move(caller), detail::own_listen(std::move(config)));
}
task<prepared_listener> http_server_group::prepare_listeners(worker_ref caller, std::span<const listen_config> configs) & {
    const auto state = state_;
    return state->prepare(std::move(caller), detail::own_listen(configs));
}
task<void> http_server_group::add_listener(worker_ref caller, listen_config config) & {
    return detail::prepared_listener_access::add(prepare_listener(std::move(caller), std::move(config)));
}
task<void> http_server_group::add_connection_service(worker_ref caller, listen_config config) & {
    const auto state = state_;
    return state->add(std::move(caller), detail::own_listen(std::move(config)), true);
}
http_server_group& http_server_group::trusted_proxies(trusted_proxy_config config) & {
    state_->trusted_proxies(std::move(config));
    return *this;
}
task<void> http_server_group::start(worker_ref caller) & {
    const auto state = state_;
    return state->start(std::move(caller));
}
task<void> http_server_group::remove_listener(worker_ref caller, const listener_endpoint& endpoint, listener_remove_options options) & {
    const auto state = state_;
    return state->remove(std::move(caller), detail::owned_endpoint(endpoint), options);
}
task<void> http_server_group::wait_connections_closed(worker_ref caller, const listener_endpoint& endpoint, operation_options options) & {
    return state_->wait_connections(std::move(caller), detail::owned_endpoint(endpoint), std::move(options));
}
task<void> http_server_group::replace_tls_identities(worker_ref caller, const listener_endpoint& endpoint, tls_config config) & {
    const auto state = state_;
    auto replacement = detail::own_listen(listen_config{.address_ = endpoint.address_, .https_ = endpoint.port_, .tls_ = std::move(config), .dispatch_ = listener_dispatch::connection});
    return state->replace(std::move(caller), detail::owned_endpoint(endpoint), std::move(replacement));
}
#ifndef _WIN32
task<listener_sockets> http_server_group::duplicate_listener_sockets(worker_ref caller, const listener_endpoint& endpoint) const& {
    const auto state = state_;
    return state->duplicate(std::move(caller), detail::owned_endpoint(endpoint));
}
#endif
task<void> http_server_group::adopt(worker_ref caller, std::size_t index, const listener_endpoint& endpoint, native_socket socket, adopt_options options) & {
    const auto state = state_;
    detail::socket_owner owner(socket);
    return state->adopt(std::move(caller), index, detail::owned_endpoint(endpoint), std::move(owner), detail::owned_adopt_options(options));
}
task<bool> http_server_group::close_connection(worker_ref caller, std::uint64_t id) & {
    const auto state = state_;
    return state->close_connection(std::move(caller), id);
}
void http_server_group::close() & noexcept {
    state_->close();
}
task<void> http_server_group::shutdown(worker_ref caller) & {
    const auto state = state_;
    return state->shutdown(std::move(caller));
}
http_server_stats http_server_group::stats() const {
    return state_->stats();
}
std::size_t http_server_group::worker_count() const noexcept {
    return state_->worker_count();
}
const worker_ref& http_server_group::worker(std::size_t index) const& {
    return state_->worker(index);
}

}  // namespace ruvia
