#pragma once

// Internal startup-time controller registration contracts.

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/web/detail/middleware/middleware_descriptor.h"
#include "ruvia/web/detail/router/route_handlers.h"
#include "ruvia/web/detail/router/route_modes.h"
#include "ruvia/web/detail/util/registration_resource.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/websocket.h"

namespace ruvia::detail {

class router;

template <typename controller_type>
class controller_registration_access;

struct controller_store_state;
struct controller_store_state_deleter {
    void operator()(controller_store_state* state) const noexcept;
};

class controller_store;

// One registered controller type: how a worker constructs its instance, and
// how that instance declares its routes. Routes are declared once, into the
// process route plan; every worker afterwards only constructs instances.
struct controller_registrar_type final {
    void* (*create_)(controller_store&){nullptr};
    void (*register_routes_)(void* controller, router&){nullptr};

    [[nodiscard]] friend bool operator==(
        const controller_registrar_type&, const controller_registrar_type&) noexcept = default;
};

// The controller instances of one worker, one per registrar in registrar
// order, which is the order the route plan's controller slots use.
class controller_store final {
public:
    controller_store();
    explicit controller_store(std::span<const controller_registrar_type> registrars);
    ~controller_store();

    controller_store(const controller_store&) = delete;
    controller_store& operator=(const controller_store&) = delete;
    controller_store(controller_store&&) noexcept;
    controller_store& operator=(controller_store&&) noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] void* instance(std::size_t slot) const noexcept;

private:
    template <typename controller_type>
    friend void* create_controller(controller_store& controllers);

    template <typename t_type>
    t_type& emplace() {
        auto* raw = construct_pmr_object<t_type>(registration_resource());
        try {
            add_lifetime(raw, &controller_store::destroy<t_type>);
        } catch (...) {
            destroy_pmr_object(raw, registration_resource());
            throw;
        }
        return *raw;
    }

    using destroy_type = void (*)(void*) noexcept;

    void add_lifetime(void* target, destroy_type destroy);

    template <typename t_type>
    static void destroy(void* target) noexcept {
        destroy_pmr_object(static_cast<t_type*>(target), registration_resource());
    }

    std::unique_ptr<controller_store_state, controller_store_state_deleter> state_;
};

class controller_route_builder final {
public:
    controller_route_builder(const controller_route_builder&) = delete;
    controller_route_builder& operator=(const controller_route_builder&) = delete;
    ~controller_route_builder();

private:
    template <typename controller_type>
    friend class controller_registration_access;

    // Middlewares come from make_middlewares(), already allocated from
    // registration_resource(), and are stored as-is.
    controller_route_builder(
        router& router_value, std::string_view prefix, std::pmr::vector<controller_middleware_descriptor> middlewares);

    // Each builds the endpoint its route kind runs and registers it through
    // the router's single registration path.
    void register_route(http_known_method method, std::string_view path, route_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares) const;
    void register_route(http_known_method method, std::string_view path, route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares) const;
    void register_extension_method_route(std::string_view method_token, std::string_view path,
        route_handler_type handler, std::span<const controller_middleware_descriptor> middlewares) const;
    void register_extension_method_route(std::string_view method_token, std::string_view path,
        route_stream_handler_type handler, std::span<const controller_middleware_descriptor> middlewares) const;
    void register_tunnel_route(std::string_view protocol, std::string_view target, route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config) const;
    void register_websocket_route(http_known_method method, std::string_view path,
        route_stream_handler_type handler, std::span<const controller_middleware_descriptor> middlewares,
        websocket_route_config websocket_config) const;
    [[nodiscard]] controller_route_builder create_scope(
        std::string_view prefix, std::span<const controller_middleware_descriptor> middlewares) const;

    struct owned_prefix_tag_type final {};
    controller_route_builder(router& router_value, std::pmr::string prefix,
        std::pmr::vector<controller_middleware_descriptor> middlewares, owned_prefix_tag_type);

    class impl_type;
    struct impl_deleter_type {
        void operator()(impl_type* impl) const noexcept;
    };
    std::unique_ptr<impl_type, impl_deleter_type> impl_;
};

[[nodiscard]] bool add_controller_registrar(controller_registrar_type registrar_value);
// Static controller discovery is complete before main. The first application/test_app
// build seals this registry; loading a controller-bearing module afterwards is
// a startup contract error instead of silently changing only later workers.
[[nodiscard]] std::pmr::vector<controller_registrar_type> seal_controller_registrars();

}  // namespace ruvia::detail
