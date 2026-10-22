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
#include "ruvia/web/detail/middleware/middleware_descriptor.h"
#include "ruvia/web/detail/router/route_handlers.h"
#include "ruvia/web/detail/util/registration_resource.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/websocket.h"

namespace ruvia::detail {

class router;

template <typename controller_type>
class controller_registration_access;

class controller_store;

// One registered controller type: how a worker constructs its instance, and
// how that instance declares its routes. Routes are declared once, into the
// process route plan; every worker afterwards only constructs instances.
struct controller_registrar final {
    void* (*create_)(controller_store&){nullptr};
    void (*register_routes_)(void* controller, router&){nullptr};

    [[nodiscard]] friend bool operator==(
        const controller_registrar&, const controller_registrar&) noexcept = default;
};

// The controller instances of one worker, one per registrar in registrar
// order, which is the order the route plan's controller slots use.
class controller_store final {
public:
    controller_store();
    explicit controller_store(std::span<const controller_registrar> registrars);
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

    struct controller_lifetime final {
        void* target_{nullptr};
        destroy_type destroy_{nullptr};

        controller_lifetime(void* target_value, destroy_type destroy_value) noexcept
            : target_(target_value),
              destroy_(destroy_value) {}
        controller_lifetime(const controller_lifetime&) = delete;
        controller_lifetime& operator=(const controller_lifetime&) = delete;
        controller_lifetime(controller_lifetime&& other) noexcept
            : target_(std::exchange(other.target_, nullptr)),
              destroy_(std::exchange(other.destroy_, nullptr)) {}
        controller_lifetime& operator=(controller_lifetime&& other) noexcept {
            if (this != &other) {
                if (target_ != nullptr) {
                    destroy_(target_);
                }
                target_ = std::exchange(other.target_, nullptr);
                destroy_ = std::exchange(other.destroy_, nullptr);
            }
            return *this;
        }
        ~controller_lifetime() {
            if (target_ != nullptr) {
                destroy_(target_);
            }
        }
    };

    void add_lifetime(void* target, destroy_type destroy);

    template <typename t_type>
    static void destroy(void* target) noexcept {
        destroy_pmr_object(static_cast<t_type*>(target), registration_resource());
    }

    std::pmr::vector<controller_lifetime> lifetimes_{registration_resource()};
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
    void register_route(http_known_method method, std::string_view path, route_handler handler,
        std::span<const controller_middleware_descriptor> middlewares) const;
    void register_route(http_known_method method, std::string_view path, route_stream_handler handler,
        std::span<const controller_middleware_descriptor> middlewares) const;
    void register_extension_method_route(std::string_view method_token, std::string_view path,
        route_handler handler, std::span<const controller_middleware_descriptor> middlewares) const;
    void register_extension_method_route(std::string_view method_token, std::string_view path,
        route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares) const;
    void register_tunnel_route(std::string_view protocol, std::string_view target, route_stream_handler handler,
        std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config) const;
    void register_websocket_route(http_known_method method, std::string_view path,
        route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares,
        websocket_route_config websocket_config) const;
    [[nodiscard]] controller_route_builder create_scope(
        std::string_view prefix, std::span<const controller_middleware_descriptor> middlewares) const;

    struct owned_prefix_tag_type final {};
    controller_route_builder(router& router_value, std::pmr::string prefix,
        std::pmr::vector<controller_middleware_descriptor> middlewares, owned_prefix_tag_type);

    router& router_;
    std::pmr::string prefix_;
    std::pmr::vector<controller_middleware_descriptor> middlewares_;
};

[[nodiscard]] bool add_controller_registrar(controller_registrar registrar_value);
// Static controller discovery is complete before main. The first application/test_app
// build seals this registry; loading a controller-bearing module afterwards is
// a startup contract error instead of silently changing only later workers.
[[nodiscard]] std::pmr::vector<controller_registrar> seal_controller_registrars();

}  // namespace ruvia::detail
