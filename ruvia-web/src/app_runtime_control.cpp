#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/ip/tcp.hpp>

#include "app.h"
#include "app_state.h"
#include "blocking_pool.h"
#include "connection_lifecycle.h"
#include "controller.h"
#include "failure_report.h"
#include "http_server.h"
#include "io_operation.h"
#include "move_only_function.h"
#include "rate_limit.h"
#include "route_table.h"
#include "runtime_control.h"
#include "server_listener.h"
#include "stop_token.h"
#include "task.h"
#include "tls_config.h"
#include "tls_server.h"
#include "web_worker_runtime.h"
#include "worker_ref.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include "http3_endpoint.h"
#endif
#if defined(_WIN32)
#include "windows_acceptor.h"
#endif

namespace ruvia {
namespace {

// The caller holds the application lock. Awaited controls may originate on
// any worker, including one of this run's own workers.
[[nodiscard]] detail::app_runtime_graph& running_runtime(detail::app_state& state_value, const char* not_running_message) {
    if (!state_value.lifecycle_.running() || state_value.runtime_ == nullptr) {
        throw std::logic_error(not_running_message);
    }
    return *state_value.runtime_;
}

// A lifecycle snapshot taken under the application lock. Controls posted
// after it run unlocked: their owner threads recheck admission, and every
// commit rechecks the lifecycle under the lock.
[[nodiscard]] bool app_running(detail::app_state& state_value) {
    std::lock_guard lock(state_value.mutex_);
    return state_value.lifecycle_.running();
}

class active_app_control final {
public:
    active_app_control(std::shared_ptr<detail::app_state> state, const char* not_running) :
        state_(std::move(state)) {
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
    explicit owned_tls_config(tls_config config) :
        config_(std::move(config)) {}
    owned_tls_config(owned_tls_config&&) = default;
    ~owned_tls_config() {
        detail::tls_secret_cleanser cleanser(config_);
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
    auto result = co_await run_blocking(caller, completion.make_command([function = std::move(function)](const auto& receipt) mutable -> result_type {
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
[[nodiscard]] std::size_t runtime_listener_slot(const detail::app_runtime_graph& runtime, const asio::ip::tcp::endpoint& target) {
    for (std::size_t slot = 0; slot < runtime.listener_slots_.size(); ++slot) {
        if (runtime.listener_slots_[slot] == target) {
            return slot;
        }
    }
    throw std::logic_error("the running app serves no slot for a stored listener");
}

// The index of the HTTPS listener `target` names in the running app, whose
// identities `config` may replace. The caller holds the application lock.
[[nodiscard]] std::size_t find_replaceable_tls_listener(detail::app_state& state_value, const asio::ip::tcp::endpoint& target, const tls_config& config) {
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
void make_worker_controls(control_list& controls, detail::app_runtime_graph& runtime, detail::runtime_control_completion& publication, const run_type& run) {
    controls.reserve(runtime.workers_.size());
    for (std::size_t index = 0; index < runtime.workers_.size(); ++index) {
        controls.emplace_back(publication.make_command([run, worker = runtime.workers_[index].runtime_.get(), index](const auto& receipt) mutable {
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

[[nodiscard]] runtime_route_candidate prepare_runtime_routes(const detail::app_state& state_value, std::size_t worker_count) {
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
            rate_limiters.push_back(std::make_unique<detail::rate_limiter>(std::nullopt, detail::route_rate_limit_presence::present, state_value.options_.rate_limit_capacity_per_worker_));
        }
    }
    return result;
}

task<void> enable_runtime_routes(worker_ref caller, detail::app_runtime_graph& runtime, runtime_route_candidate& candidate) {
    detail::runtime_control_completion publication(caller);
    control_list controls;
    make_worker_controls(controls, runtime, publication, [&runtime, &candidate](detail::web_worker_runtime& worker, std::size_t index) noexcept {
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
[[nodiscard]] move_only_function<void()> make_acceptor_control(detail::windows_acceptor& acceptor, detail::runtime_control_completion& publication, run_type run) {
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
[[nodiscard]] bool post_acceptor_control(detail::windows_acceptor& acceptor, detail::runtime_control_completion& publication, run_type run) {
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

namespace {
task<void> replace_app_tls_identities(std::shared_ptr<detail::app_state> state, worker_ref caller, asio::ip::tcp::endpoint target, owned_tls_config input) {
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
    std::size_t slot = 0;
    {
        std::lock_guard lock(state->mutex_);
        (void)running_runtime(*state, "TLS identities can be replaced only while the app is running");
        index = stored_listener_index(*state, target);
        slot = runtime_listener_slot(runtime, target);
    }
    // The control gate keeps the slot stable and `active` pins the runtime,
    // so the controls are built, posted and dropped outside the lock.
    make_worker_controls(controls, runtime, completion, [slot, identities](detail::web_worker_runtime& worker, std::size_t) {
        worker.adopt_tls_identities(slot, identities);
    });
    const bool posted = post_worker_controls(runtime, controls);
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

task<void> application::replace_tls_identities(worker_ref caller, const listener_endpoint& endpoint, tls_config config) & {
    owned_tls_config input(std::move(config));
    const asio::ip::tcp::endpoint target(detail::normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    return replace_app_tls_identities(state_, std::move(caller), target, std::move(input));
}

namespace {
task<bool> close_app_connection(std::shared_ptr<detail::app_state> state, worker_ref caller, std::uint64_t connection_id) {
    const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
    active_app_control active(state, "connections can be closed only while the app is running");
    auto& runtime = active.runtime();
    std::atomic<bool> closed{false};
    detail::runtime_control_completion completion(caller);
    control_list controls;
    make_worker_controls(controls, runtime, completion, [connection_id, &closed](detail::web_worker_runtime& worker, std::size_t) noexcept {
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
task<void> wait_app_connections(std::shared_ptr<detail::app_state> state, worker_ref caller, asio::ip::tcp::endpoint target, operation_options options) {
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
    const asio::ip::tcp::endpoint target(detail::normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    return wait_app_connections(state_, std::move(caller), target, std::move(options));
}

namespace {
struct listener_addition_candidate final {
    std::vector<detail::http_server_listener_definition> added_;
    std::vector<detail::http_server_listener_definition> prepared_;
    std::vector<std::vector<detail::prepared_http_server_session>> sessions_;
    std::optional<runtime_route_candidate> routes_;
};

[[nodiscard]] std::exception_ptr control_round_failure(const detail::runtime_control_completion& completion, bool posted, std::size_t targets, const char* message) noexcept {
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

task<void> add_app_listener(std::shared_ptr<detail::app_state> state, worker_ref caller, detail::owned_listen_configs input, std::shared_ptr<detail::prepared_listener_state> transaction) {
    const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
    active_app_control active(state, "listeners can be added only while the app is running");
    auto& runtime = active.runtime();
    const auto worker_count = runtime.workers_.size();
    const auto key_origin = runtime.tls_key_origin_;
    const bool needs_routes = !runtime.routes_enabled_;
    const bool inexpensive = std::ranges::none_of(input.configs_, [needs_routes](const auto& config) {
        return config.https_.has_value() || (needs_routes && config.dispatch_ == listener_dispatch::routes);
    });
    auto prepare = [state, worker_count, key_origin, needs_routes, input = std::move(input)]() mutable {
        listener_addition_candidate result;
        result.added_ = input.load();
        result.prepared_.reserve(result.added_.size());
        for (const auto& definition : result.added_) {
            result.prepared_.push_back(detail::validate_run_listener(definition, state->options_, key_origin));
        }
        result.sessions_.reserve(worker_count);
        for (std::size_t worker = 0; worker < worker_count; ++worker) {
            auto& sessions = result.sessions_.emplace_back();
            sessions.reserve(result.prepared_.size());
            for (const auto& definition : result.prepared_) {
                if (std::holds_alternative<detail::http_server_listener_definition::tls>(definition.transport_)) {
                    sessions.push_back(detail::prepare_http_server_session_config(definition, static_cast<bool>(state->options_.tls_client_hello_)));
                } else {
                    sessions.emplace_back();
                }
            }
        }
        if (needs_routes && std::ranges::any_of(result.added_, [](const auto& listener) {
            return !listener.connection_.has_value() && !listener.request_stream_ && !std::holds_alternative<detail::http_server_listener_definition::redirect_http_to_https>(listener.transport_);
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
    // The control gate serializes every writer of http3_directory_, so the
    // directory is built and bound before the lock and only published under it.
    std::unique_ptr<detail::http3_worker_directory> created_directory;
    if (runtime.http3_directory_ == nullptr && std::ranges::any_of(prepared, [](const auto& definition) {
        return definition.http3_.has_value();
    })) {
        created_directory = std::make_unique<detail::http3_worker_directory>(runtime.workers_.size());
        for (std::size_t index = 0; index < runtime.workers_.size(); ++index) {
            created_directory->bind_worker(index, runtime.workers_[index].runtime_->runtime().worker());
        }
    }
#endif
    {
        std::lock_guard lock(state->mutex_);
        (void)running_runtime(*state, "listeners can be added only while the app is running");
        detail::ensure_new_listener_endpoints(state->listeners_, added);
        state->listeners_.reserve(state->listeners_.size() + added.size());
        http3_slots.reserve(added.size());
#if defined(RUVIA_ENABLE_HTTP3)
        if (created_directory != nullptr) {
            runtime.http3_directory_ = std::move(created_directory);
        }
        directory = runtime.http3_directory_.get();
#endif
        first_slot = runtime.listener_slots_.size();
        runtime.listener_slots_.resize(first_slot + added.size());
        for (const auto& definition : prepared) {
            http3_slots.push_back(definition.http3_.has_value() ? std::optional<std::size_t>(runtime.http3_slot_count_++) : std::nullopt);
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
    make_worker_controls(prepare_controls, runtime, prepared_completion, [&](detail::web_worker_runtime& worker, std::size_t worker_index) {
        for (std::size_t index = 0; index < prepared.size(); ++index) {
#if defined(RUVIA_ENABLE_HTTP3)
            worker.prepare_listener(first_slot + index, http3_slots[index], prepared[index], std::move(candidate.sessions_[worker_index][index]), directory);
#else
            worker.prepare_listener(first_slot + index, http3_slots[index], prepared[index], std::move(candidate.sessions_[worker_index][index]));
#endif
        }
    });
    make_worker_controls(discard_controls, runtime, discarded_completion, [discard, receipt = std::make_shared<detail::runtime_control_completion::receipt>(discarded_completion.make_receipt())](detail::web_worker_runtime& worker, std::size_t) {
        discard(worker, receipt);
    });
    make_worker_controls(open_controls, runtime, opened_completion, [open](detail::web_worker_runtime& worker, std::size_t) {
        open(worker);
    });
#if defined(_WIN32)
    auto prepare_acceptor = make_acceptor_control(*runtime.acceptor_, prepared_completion, [&](detail::windows_acceptor& acceptor) {
        for (std::size_t index = 0; index < prepared.size(); ++index) {
#if defined(RUVIA_ENABLE_HTTP3)
            acceptor.prepare_listener(first_slot + index, http3_slots[index], prepared[index], directory);
#else
            acceptor.prepare_listener(first_slot + index, http3_slots[index], prepared[index]);
#endif
        }
    });
    auto discard_acceptor = make_acceptor_control(*runtime.acceptor_, discarded_completion, [discard, receipt = std::make_shared<detail::runtime_control_completion::receipt>(discarded_completion.make_receipt())](detail::windows_acceptor& acceptor) {
        discard(acceptor, receipt);
    });
    detail::runtime_control_completion accepting_completion(caller);
    auto open_acceptor = make_acceptor_control(*runtime.acceptor_, accepting_completion, [open](detail::windows_acceptor& acceptor) {
        open(acceptor);
    });
#endif
    bool posted = post_worker_controls(runtime, prepare_controls);
#if defined(_WIN32)
    if (posted) {
        posted = runtime.acceptor_->post_control(std::move(prepare_acceptor));
    }
    prepare_acceptor = nullptr;
#endif
    co_await prepared_completion.wait();
    auto failure = control_round_failure(prepared_completion, posted, control_targets(runtime), "the app began stopping before every worker prepared the listeners");
    if (!failure) {
        try {
            co_await transaction->checkpoint(runtime.workers_.front().runtime_->cancellation());
        } catch (...) {
            failure = std::current_exception();
        }
    }
    if (!failure) {
        // Owner threads recheck admission when an open runs, and the commit
        // below rechecks the lifecycle, so a stop racing these unlocked posts
        // leaves the listeners closed and rolls them back.
        posted = app_running(*state) && post_worker_controls(runtime, open_controls);
        open_controls.clear();
        co_await opened_completion.wait();
        failure = control_round_failure(opened_completion, posted, runtime.workers_.size(), "the app began stopping before every worker accepted the listeners");
#if defined(_WIN32)
        if (!failure) {
            posted = app_running(*state) && runtime.acceptor_->post_control(std::move(open_acceptor));
            open_acceptor = nullptr;
            co_await accepting_completion.wait();
            failure = control_round_failure(accepting_completion, posted, 1, "the app began stopping before the acceptor accepted the listeners");
        }
#endif
    }
    // Committed listeners drop their inherited-socket claim after the lock: the
    // last owner closes whatever no worker claimed.
    std::vector<std::shared_ptr<detail::inherited_listener_sockets>> released_sockets;
    if (!failure) {
        released_sockets.reserve(added.size());
        std::lock_guard lock(state->mutex_);
        if (!state->lifecycle_.running()) {
            try {
                failure = std::make_exception_ptr(std::logic_error("the app began stopping before every worker accepted the listeners"));
            } catch (...) {
                failure = std::current_exception();
            }
        } else {
            for (std::size_t index = 0; index < added.size(); ++index) {
                runtime.listener_slots_[first_slot + index] = added[index].endpoint_;
                released_sockets.push_back(std::move(added[index].inherited_sockets_));
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
    return detail::prepared_listener_access::launch(transaction, add_app_listener(state_, std::move(caller), std::move(input), transaction));
}
task<prepared_listener> application::prepare_listeners(worker_ref caller, std::span<const listen_config> configs) & {
    detail::owned_listen_configs input(configs);
    auto transaction = detail::prepared_listener_access::make(caller);
    return detail::prepared_listener_access::launch(transaction, add_app_listener(state_, std::move(caller), std::move(input), transaction));
}
task<void> application::add_listener(worker_ref caller, listen_config config) & {
    return detail::prepared_listener_access::add(prepare_listener(std::move(caller), std::move(config)));
}

namespace {
task<void> remove_app_listener(std::shared_ptr<detail::app_state> state, worker_ref caller, asio::ip::tcp::endpoint target, listener_remove_options options) {
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
    auto accepting_lifetime = std::make_shared<detail::runtime_control_completion::receipt>(accepting_completion.make_receipt());
    const bool accepting_posted = post_acceptor_control(*runtime.acceptor_, accepting_completion, [slot, receipt = std::move(accepting_lifetime)](detail::windows_acceptor& acceptor) {
        acceptor.remove_listener(slot, receipt);
    });
    co_await accepting_completion.wait();
    if (auto failure = control_round_failure(accepting_completion, accepting_posted, 1, "the app began stopping before the acceptor removed the listener")) {
        std::rethrow_exception(failure);
    }
#endif
    detail::runtime_control_completion completion(caller);
    control_list controls;
    auto lifetime = std::make_shared<detail::runtime_control_completion::receipt>(completion.make_receipt());
    make_worker_controls(controls, runtime, completion, [slot, receipt = std::move(lifetime), options](detail::web_worker_runtime& worker, std::size_t) {
        worker.remove_listener(slot, receipt, options);
    });
    // A stopping worker refuses the post, and the commit below rechecks the
    // lifecycle, so the post needs no lock.
    const bool posted = app_running(*state) && post_worker_controls(runtime, controls);
    controls.clear();
    co_await completion.wait();
    if (auto failure = control_round_failure(completion, posted, runtime.workers_.size(), "the app began stopping before every worker removed the listener")) {
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
    const asio::ip::tcp::endpoint target(detail::normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    return remove_app_listener(state_, std::move(caller), target, options);
}

#if !defined(_WIN32)
namespace {
task<listener_sockets> duplicate_app_listener_sockets(std::shared_ptr<detail::app_state> state, worker_ref caller, asio::ip::tcp::endpoint target) {
    const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
    active_app_control active(state, "listener sockets can be duplicated only while the app is running");
    auto& runtime = active.runtime();
    std::vector<detail::listener_socket_duplicates> duplicates;
    detail::runtime_control_completion completion(caller);
    control_list controls;
    bool https = false;
    bool http3 = false;
    std::size_t slot = 0;
    {
        std::lock_guard lock(state->mutex_);
        const auto& stored = state->listeners_[stored_listener_index(*state, target)];
        https = std::holds_alternative<detail::http_server_listener_definition::tls>(stored.transport_);
        http3 = stored.http3_.has_value();
        slot = runtime_listener_slot(runtime, target);
    }
    duplicates.resize(runtime.workers_.size());
    make_worker_controls(controls, runtime, completion, [slot, &duplicates](detail::web_worker_runtime& worker, std::size_t index) {
        duplicates[index] = worker.duplicate_listener(slot);
    });
    const bool posted = post_worker_controls(runtime, controls);
    co_await completion.wait();
    listener_sockets result;
    auto& tcp = https ? result.https_ : result.http_;
    try {
        if (auto failure = control_round_failure(completion, posted, duplicates.size(), "the app began stopping before every worker duplicated its sockets")) {
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

task<listener_sockets> application::duplicate_listener_sockets(worker_ref caller, const listener_endpoint& endpoint) const& {
    const asio::ip::tcp::endpoint target(detail::normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    return duplicate_app_listener_sockets(state_, std::move(caller), target);
}
#endif

}  // namespace ruvia
