#include "controller.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app.h"
#include "borrowed_text.h"
#include "callback_ref.h"
#include "http_error.h"
#include "http_request.h"
#include "http_tunnel.h"
#include "middleware.h"
#include "rate_limit.h"
#include "rate_limiting.h"
#include "route_path_segment.h"
#include "route_table.h"
#include "router.h"
#include "test_app.h"
#include "websocket.h"
#include "worker_state.h"

namespace ruvia {

// The route-registration surface a controller sees. Each call builds the
// endpoint its route kind runs and hands it, with the scope's prefix and
// middlewares, to the router's single registration path.

using namespace detail;

namespace {

[[nodiscard]] std::size_t write_joined_controller_path(char* destination, std::string_view prefix, std::string_view path) noexcept {
    std::size_t length = 0;
    if (!prefix.empty() && prefix != "/") {
        if (prefix.front() != '/') {
            destination[length++] = '/';
        }
        prefix.copy(destination + length, prefix.size());
        length += prefix.size();
        if (length > 1 && destination[length - 1] == '/') {
            --length;
        }
    }
    if (path.empty() || path == "/") {
        if (length == 0) {
            destination[length++] = '/';
        }
        return length;
    }
    if (path.front() == '/') {
        path.remove_prefix(1);
    }
    destination[length++] = '/';
    path.copy(destination + length, path.size());
    return length + path.size();
}

struct scoped_controller_path final {
    static constexpr std::size_t inline_capacity = 256;
    std::array<char, inline_capacity> inline_{};
    std::string heap_{};
    std::string_view view_{};

    scoped_controller_path(std::string_view prefix, std::string_view path) {
        if (prefix.empty() || prefix == "/") {
            if (path.empty()) {
                view_ = "/";
                return;
            }
            if (path.front() == '/') {
                view_ = path;
                return;
            }
        }
        const std::size_t max_size = prefix.size() + path.size() + 2U;
        if (max_size <= inline_capacity) {
            const auto length = write_joined_controller_path(inline_.data(), prefix, path);
            view_ = std::string_view(inline_.data(), length);
            return;
        }
        heap_.resize(max_size);
        heap_.resize(write_joined_controller_path(heap_.data(), prefix, path));
        view_ = heap_;
    }
};

[[nodiscard]] std::string join_controller_paths(std::string_view prefix, std::string_view path) {
    scoped_controller_path joined(prefix, path);
    if (!joined.heap_.empty()) {
        return std::move(joined.heap_);
    }
    return std::string(joined.view_);
}
[[nodiscard]] std::vector<controller_middleware_descriptor> merge_middleware_descriptors(std::span<const controller_middleware_descriptor> base, std::span<const controller_middleware_descriptor> extra) {
    std::vector<controller_middleware_descriptor> middlewares;
    middlewares.reserve(base.size() + extra.size());
    middlewares.insert(middlewares.end(), base.begin(), base.end());
    middlewares.insert(middlewares.end(), extra.begin(), extra.end());
    return middlewares;
}

[[nodiscard]] request_body_mode declared_request_body_mode(std::span<const controller_middleware_descriptor> controller_middlewares, std::span<const controller_middleware_descriptor> route_middlewares) noexcept {
    for (const auto descriptors : {controller_middlewares, route_middlewares}) {
        for (const auto& descriptor : descriptors) {
            if (descriptor.stream_request_body()) {
                return request_body_mode::stream;
            }
        }
    }
    return request_body_mode::buffered;
}
}  // namespace

detail::controller_route_builder::controller_route_builder(router& router_value, std::string_view prefix, std::vector<controller_middleware_descriptor> middlewares) :
    controller_route_builder(router_value, join_controller_paths({}, prefix), std::move(middlewares), owned_prefix_tag_type{}) {}

detail::controller_route_builder::controller_route_builder(router& router_value, std::string prefix, std::vector<controller_middleware_descriptor> middlewares, owned_prefix_tag_type) :
    router_(router_value),
    prefix_(std::move(prefix)),
    middlewares_(std::move(middlewares)) {}

detail::controller_route_builder::~controller_route_builder() = default;

void detail::controller_route_builder::register_route(http_known_method method, std::string_view method_token, std::string_view path, route_handler handler, std::span<const controller_middleware_descriptor> middlewares) const {
    const auto body_mode = declared_request_body_mode(middlewares_, middlewares);
    const scoped_controller_path joined(prefix_, path);
    router_.register_endpoint(method, method_token, joined.view_, route_endpoint::buffered(callback_access::invoke(handler), body_mode), callback_access::target(handler), middlewares_, middlewares);
}

void detail::controller_route_builder::register_route(http_known_method method, std::string_view method_token, std::string_view path, route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares) const {
    const auto body_mode = declared_request_body_mode(middlewares_, middlewares);
    const scoped_controller_path joined(prefix_, path);
    router_.register_endpoint(method, method_token, joined.view_, route_endpoint::response_stream(callback_access::invoke(handler), body_mode), callback_access::target(handler), middlewares_, middlewares);
}

void detail::controller_route_builder::register_tunnel_route(std::string_view protocol, std::string_view target, route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config) const {
    std::optional<scoped_controller_path> joined;
    if (!protocol.empty()) {
        joined.emplace(prefix_, target);
    }
    const auto path = protocol.empty() ? target : joined->view_;
    router_.register_endpoint(http_known_method::connect, {}, path, route_endpoint::tunnel(callback_access::invoke(handler), protocol, config), callback_access::target(handler), middlewares_, middlewares);
}

void detail::controller_route_builder::register_websocket_route(http_known_method method, std::string_view path, route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares, websocket_route_config websocket_config) const {
    const scoped_controller_path joined(prefix_, path);
    router_.register_endpoint(method, {}, joined.view_, route_endpoint::websocket(callback_access::invoke(handler), websocket_config), callback_access::target(handler), middlewares_, middlewares);
}
detail::controller_route_builder detail::controller_route_builder::create_scope(std::string_view prefix, std::span<const controller_middleware_descriptor> middlewares) const {
    auto merged = merge_middleware_descriptors(middlewares_, middlewares);
    return controller_route_builder(router_, join_controller_paths(prefix_, prefix), std::move(merged), owned_prefix_tag_type{});
}

namespace detail {
namespace {

struct controller_registry_state final {
    std::mutex mutex_;
    std::vector<controller_registrar> registrars_{};
    bool sealed_{false};
};

controller_registry_state& controller_registry() {
    static controller_registry_state state;
    return state;
}

}  // namespace

controller_store::controller_store() = default;

controller_store::controller_store(std::span<const controller_registrar> registrars) {
    lifetimes_.reserve(registrars.size());
    for (const auto& registrar : registrars) {
        // Capacity is reserved, so adopting a created instance cannot throw.
        lifetimes_.emplace_back(registrar.create_(), registrar.destroy_);
    }
}

controller_store::~controller_store() = default;

controller_store::controller_store(controller_store&&) noexcept = default;

controller_store& controller_store::operator=(controller_store&&) noexcept = default;

std::size_t controller_store::size() const noexcept {
    return lifetimes_.size();
}

void* controller_store::instance(std::size_t slot) const noexcept {
    return lifetimes_[slot].target_;
}

bool add_controller_registrar(controller_registrar registrar_value) {
    auto& state_value = controller_registry();
    std::lock_guard lock(state_value.mutex_);
    if (state_value.sealed_) {
        throw std::logic_error("controller registration is sealed; load every controller module before application::run()");
    }
    for (const auto& existing : state_value.registrars_) {
        if (existing == registrar_value) {
            return true;
        }
    }
    state_value.registrars_.push_back(registrar_value);
    return true;
}

std::vector<controller_registrar> seal_controller_registrars() {
    std::vector<controller_registrar> registrars{};
    auto& state_value = controller_registry();
    std::lock_guard lock(state_value.mutex_);
    state_value.sealed_ = true;
    registrars = state_value.registrars_;
    return registrars;
}

}  // namespace detail

}  // namespace ruvia

namespace ruvia::detail {

std::string_view normalize_fallback_prefix(std::string_view prefix) {
    if (prefix.empty() || prefix.front() != '/') {
        throw std::invalid_argument("fallback prefix must start with '/'");
    }
    if ((prefix.find('?') != std::string_view::npos) || !is_valid_http_origin_form_target(prefix)) {
        throw std::invalid_argument("fallback prefix must be an origin-form path without query");
    }
    while (prefix.size() > 1 && prefix.back() == '/') {
        prefix.remove_suffix(1);
    }
    if (prefix == "/") {
        throw std::invalid_argument("fallback prefix must not be '/'; register app-wide without a prefix");
    }
    return prefix;
}

namespace {

template <typename handlers_type, typename handler_type>
void append_prefix_handler(handlers_type& handlers, std::string_view prefix, handler_type&& handler) {
    if (!handler) {
        throw std::invalid_argument("fallback handler must not be null");
    }
    const auto normalized = normalize_fallback_prefix(prefix);
    for (const auto& existing : handlers) {
        if (fallback_prefixes_equivalent(existing.first, normalized)) {
            throw std::invalid_argument("duplicate fallback prefix");
        }
    }
    handlers.emplace_back(std::string(normalized), std::move(handler));
}

}  // namespace

app_configuration::app_configuration() = default;

void app_configuration::add_middleware(owned_middleware_descriptor&& descriptor) {
    const auto& value = descriptor.descriptor();
    if (value.validated_model_type_key() != nullptr) {
        throw std::invalid_argument("validator middleware binds to a route and cannot be app-wide");
    }
    // Route admission reads body-limit declarations from route and controller
    // lists only; app-wide they would silently bound nothing. The app-wide
    // ceiling is server_config::max_buffered_body_bytes_.
    if (value.request_body_limit() != 0) {
        throw std::invalid_argument("body_limit binds to a route or controller and cannot be app-wide");
    }
    if (value.stream_request_body()) {
        throw std::invalid_argument("stream_body binds to a route or controller and cannot be app-wide");
    }
    middlewares_.push_back(std::move(descriptor));
}

void app_configuration::add_worker_state(worker_state_definition&& definition) {
    append_worker_state_definition(worker_states_, std::move(definition));
}

// A second app-wide registration throws like a duplicate scoped prefix rather
// than silently replacing the first by call order.
void app_configuration::on_error(callback_registration<http_error_handler>&& handler) {
    if (!handler) {
        throw std::invalid_argument("error handler must not be null");
    }
    if (error_handler_) {
        throw std::invalid_argument("duplicate app-wide error handler");
    }
    error_handler_ = std::move(handler);
}

callback_registration<http_error_handler> app_configuration::on_error(std::nullptr_t) noexcept {
    return std::exchange(error_handler_, nullptr);
}

void app_configuration::on_error(borrowed_text prefix, callback_registration<http_error_handler>&& handler) {
    append_prefix_handler(prefix_error_handlers_, prefix.view(), std::move(handler));
}

void app_configuration::on_not_found(callback_registration<http_not_found_handler>&& handler) {
    if (!handler) {
        throw std::invalid_argument("not found handler must not be null");
    }
    if (not_found_handler_) {
        throw std::invalid_argument("duplicate app-wide not found handler");
    }
    not_found_handler_ = std::move(handler);
}

callback_registration<http_not_found_handler> app_configuration::on_not_found(std::nullptr_t) noexcept {
    return std::exchange(not_found_handler_, nullptr);
}

void app_configuration::on_not_found(borrowed_text prefix, callback_registration<http_not_found_handler>&& handler) {
    append_prefix_handler(prefix_not_found_handlers_, prefix.view(), std::move(handler));
}

void app_configuration::rate_limit(const rate_limit_config& config) {
    validate_rate_limit_config(config);
    rate_limit_ = config;
}

void app_configuration::rate_limit(std::nullptr_t) noexcept {
    rate_limit_.reset();
}

void app_configuration::apply(router& routes_value) const {
    routes_value.set_error_handler(callback_access::ref(error_handler_));
    routes_value.set_not_found_handler(callback_access::ref(not_found_handler_));
    routes_value.set_prefix_error_handlers(prefix_error_handlers_);
    routes_value.set_prefix_not_found_handlers(prefix_not_found_handlers_);
    if (!middlewares_.empty()) {
        std::vector<controller_middleware_descriptor> descriptors;
        descriptors.reserve(middlewares_.size());
        for (const auto& middleware_value : middlewares_) {
            descriptors.push_back(middleware_value.descriptor());
        }
        routes_value.set_global_middlewares(descriptors);
    }
}

}  // namespace ruvia::detail

namespace ruvia {

template <typename host_type>
template <typename change_type>
host_type& dispatch_configuration<host_type>::edit(change_type&& change) {
    auto& host = static_cast<host_type&>(*this);
    host.edit_configuration(detail::callback_access::bind<void(detail::app_configuration&)>(change));
    return host;
}

template <typename host_type>
host_type& dispatch_configuration<host_type>::add_middleware(detail::owned_middleware_descriptor descriptor) {
    return edit([&descriptor](detail::app_configuration& configuration) {
        configuration.add_middleware(std::move(descriptor));
    });
}

template <typename host_type>
host_type& dispatch_configuration<host_type>::add_worker_state(detail::worker_state_definition definition) {
    return edit([&definition](detail::app_configuration& configuration) {
        configuration.add_worker_state(std::move(definition));
    });
}

// Handlers are pinned before the host's guard is taken: publication under it
// only swaps owning pointers.
template <typename host_type>
host_type& dispatch_configuration<host_type>::on_error(http_error_handler handler) & {
    auto owned = detail::own_callback(std::move(handler));
    return edit([&owned](detail::app_configuration& configuration) {
        configuration.on_error(std::move(owned));
    });
}

template <typename host_type>
host_type& dispatch_configuration<host_type>::on_not_found(http_not_found_handler handler) & {
    auto owned = detail::own_callback(std::move(handler));
    return edit([&owned](detail::app_configuration& configuration) {
        configuration.on_not_found(std::move(owned));
    });
}

// A cleared handler leaves through `retired`, destroyed once edit() returned.
template <typename host_type>
host_type& dispatch_configuration<host_type>::on_error(std::nullptr_t) & {
    detail::callback_registration<http_error_handler> retired;
    return edit([&retired](detail::app_configuration& configuration) {
        retired = configuration.on_error(nullptr);
    });
}

template <typename host_type>
host_type& dispatch_configuration<host_type>::on_not_found(std::nullptr_t) & {
    detail::callback_registration<http_not_found_handler> retired;
    return edit([&retired](detail::app_configuration& configuration) {
        retired = configuration.on_not_found(nullptr);
    });
}

template <typename host_type>
host_type& dispatch_configuration<host_type>::on_error(scoped_error_handler_options options) & {
    auto owned = detail::own_callback(std::move(options.handler_));
    return edit([&options, &owned](detail::app_configuration& configuration) {
        configuration.on_error(options.prefix_, std::move(owned));
    });
}

template <typename host_type>
host_type& dispatch_configuration<host_type>::on_not_found(scoped_not_found_handler_options options) & {
    auto owned = detail::own_callback(std::move(options.handler_));
    return edit([&options, &owned](detail::app_configuration& configuration) {
        configuration.on_not_found(options.prefix_, std::move(owned));
    });
}

template <typename host_type>
host_type& dispatch_configuration<host_type>::rate_limit(rate_limit_config config) & {
    return edit([&config](detail::app_configuration& configuration) {
        configuration.rate_limit(config);
    });
}

template <typename host_type>
host_type& dispatch_configuration<host_type>::rate_limit(std::nullptr_t) & {
    return edit([](detail::app_configuration& configuration) {
        configuration.rate_limit(nullptr);
    });
}

template class dispatch_configuration<application>;
template class dispatch_configuration<test_app>;
template class dispatch_configuration<::ruvia::router>;

}  // namespace ruvia
