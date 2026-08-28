// Controller registration surface remains macro-only and private to the framework.
#include <concepts>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/Controller.h"

namespace {

template <typename T>
concept HasControllerPublicGroupPrefix = requires { T::ruviaControllerGroupPrefix(); };

template <typename T>
concept HasControllerPublicGroupMiddlewares = requires { T::ruviaControllerGroupMiddlewares(); };

template <typename T>
concept HasControllerPublicRegisterRoutes = requires(T& controller, ruvia::detail::Router& router) { controller.registerRoutes(router); };

template <typename T>
concept HasControllerPublicRegistrationState = requires { T::ruviaControllerRegistered_; };

template <typename T>
concept HasControllerRegistrationAccessPublicHooks = requires {
    T::groupPrefix();
    T::groupMiddlewares();
};
class FastSurfaceController final : public ruvia::Controller<FastSurfaceController> {
public:
    RUVIA_CONTROLLER_GROUP("/surface-fast")

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/res-slot-merge", responseSlotMerge);
    RUVIA_GET("/res-setter-headers", responseSetterHeaders);
    RUVIA_GET("/body-response", bodyResponse);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> responseSlotMerge(ruvia::Context& c) {
        c.header("X-Res-Slot", "kept");
        co_return c.text("response slot merge\n");
    }

    ruvia::Task<ruvia::HttpResponse> responseSetterHeaders(ruvia::Context& c) {
        c.header("X-Setter-Override", "slot");
        c.header("Content-Type", "application/slot");
        auto response = c.text("response setter headers\n");
        response.header("X-Setter-Override", "response");
        response.header("X-Assigned-Only", "response");
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> bodyResponse(ruvia::Context& c) {
        c.status(ruvia::http_status::kCreated);
        c.header("X-Body-Prepared", "true");
        c.header("X-Body-Response", "true");
        co_return c.body("body response\n");
    }
};

class UngroupedControllerProbe final : public ruvia::Controller<UngroupedControllerProbe> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_ROUTES_END
};

class ControllerBaseSurfaceProbe final : public ruvia::Controller<ControllerBaseSurfaceProbe> {
public:
    template <typename T>
    inline static constexpr bool hasLegacyMiddlewareFactory = requires { T::template ruviaMakeMiddlewares<>(); };

    template <typename T>
    inline static constexpr bool hasLegacyRouteRegistration = requires { &T::ruviaAddRoute; };
};

#ifndef _MSC_VER
static_assert(!HasControllerPublicGroupPrefix<FastSurfaceController>);
static_assert(!HasControllerPublicGroupMiddlewares<FastSurfaceController>);
static_assert(!HasControllerPublicRegisterRoutes<FastSurfaceController>);
static_assert(!HasControllerPublicRegistrationState<FastSurfaceController>);
static_assert(!HasControllerPublicRegisterRoutes<UngroupedControllerProbe>);
static_assert(!HasControllerRegistrationAccessPublicHooks<ruvia::detail::ControllerRegistrationAccess<FastSurfaceController>>);
static_assert(!ControllerBaseSurfaceProbe::hasLegacyMiddlewareFactory<ControllerBaseSurfaceProbe>);
static_assert(!ControllerBaseSurfaceProbe::hasLegacyRouteRegistration<ControllerBaseSurfaceProbe>);
#endif

}  // namespace

int main() {
    return 0;
}
