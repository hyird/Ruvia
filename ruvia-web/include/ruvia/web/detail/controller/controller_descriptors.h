#pragma once

// Internal startup-time controller registration contracts.

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/detail/middleware/middleware_descriptor.h"
#include "ruvia/web/detail/router/route_modes.h"
#include "ruvia/web/detail/util/callable_ref.h"
#include "ruvia/web/detail/util/registration_resource.h"
#include "ruvia/web/http_tunnel_route_config.h"
#include "ruvia/web/websocket.h"

namespace ruvia::detail {

class router;

template <typename controller_t_type>
class controller_registration_access;

struct controller_store_state;
struct controller_store_state_deleter {
    void operator()(controller_store_state* state) const noexcept;
};

class controller_store;
using controller_registrar_type = void (*)(router&, controller_store&);

class controller_store final {
public:
    controller_store();
    ~controller_store();

    controller_store(const controller_store&) = delete;
    controller_store& operator=(const controller_store&) = delete;
    controller_store(controller_store&&) noexcept;
    controller_store& operator=(controller_store&&) noexcept;

private:
    template <typename controller_t_type>
    friend void register_controller_instance(router& router_value, controller_store& controller_lifetimes);
    friend void run_controller_registrars(router& router_value, controller_store& controller_lifetimes,
        std::span<const controller_registrar_type> registrars);

    template <typename t_type, typename... args_type>
    t_type& emplace(args_type&&... args) {
        auto* resource = registration_resource();
        auto* raw = construct_pmr_object<t_type>(resource, std::forward<args_type>(args)...);
        try {
            add_lifetime(raw, &controller_store::destroy<t_type>, resource);
        } catch (...) {
            destroy_pmr_object(raw, resource);
            throw;
        }
        return *raw;
    }

    void reserve(std::size_t count);

    [[nodiscard]] std::size_t size() const noexcept;

    using destroy_type = void (*)(void*, std::pmr::memory_resource*) noexcept;

    void add_lifetime(void* target, destroy_type destroy, std::pmr::memory_resource* resource);

    template <typename t_type>
    static void destroy(void* target, std::pmr::memory_resource* resource) noexcept {
        destroy_pmr_object(static_cast<t_type*>(target), registration_resource_or_default(resource));
    }

    std::unique_ptr<controller_store_state, controller_store_state_deleter> state_;
};

using controller_route_handler_type = callable_ref<http_response, context&>;
using controller_route_stream_handler_type = callable_ref<void, context&>;

class controller_route_builder final {
public:
    controller_route_builder(controller_route_builder&&) noexcept;
    controller_route_builder& operator=(controller_route_builder&&) noexcept;
    controller_route_builder(const controller_route_builder&) = delete;
    controller_route_builder& operator=(const controller_route_builder&) = delete;
    ~controller_route_builder();

private:
    template <typename controller_t_type>
    friend class controller_registration_access;

    controller_route_builder(router& router_value, std::string_view prefix,
        std::pmr::vector<controller_middleware_descriptor> middlewares =
            std::pmr::vector<controller_middleware_descriptor>(registration_resource()));

    void register_route(http_known_method method, std::string_view path,
        controller_route_handler_type handler, request_body_mode body_mode,
        std::span<const controller_middleware_descriptor> middlewares = {}) const;
    void register_extension_method_route(std::string_view method_token, std::string_view path,
        controller_route_handler_type handler, request_body_mode body_mode,
        std::span<const controller_middleware_descriptor> middlewares = {}) const;
    void register_response_stream_route(http_known_method method, std::string_view path,
        controller_route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares = {}) const;
    void register_sse_route(http_known_method method, std::string_view path,
        controller_route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares = {}) const;
    void register_tunnel_route(std::string_view protocol, std::string_view target,
        controller_route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares = {}, http_tunnel_route_config config = {}) const;
    void register_websocket_route(http_known_method method, std::string_view path,
        controller_route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares = {},
        websocket_route_config websocket_config = {}) const;
    [[nodiscard]] controller_route_builder create_scope(std::string_view prefix,
        const std::pmr::vector<controller_middleware_descriptor>& middlewares =
            std::pmr::vector<controller_middleware_descriptor>(registration_resource())) const;

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
void run_controller_registrars(router& router_value, controller_store& controller_lifetimes,
    std::span<const controller_registrar_type> registrars);

}  // namespace ruvia::detail
