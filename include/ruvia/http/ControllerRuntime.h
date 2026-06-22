#pragma once

#include "ruvia/http/Context.h"
#include "ruvia/http/ContextModel.h"
#include "ruvia/http/MiddlewareRuntime.h"
#include "ruvia/http/Model.h"
#include "ruvia/http/Validation.h"

#include <memory_resource>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ruvia {

template <typename ControllerT>
class Controller {
public:
    using RuviaControllerType = ControllerT;

protected:
    using RuviaMiddlewareList = std::pmr::vector<detail::ControllerMiddlewareDescriptor>;

    constexpr Controller() noexcept = default;
    ~Controller() = default;

    [[nodiscard]] static constexpr std::string_view ruviaControllerGroupPrefix() noexcept {
        return {};
    }

    [[nodiscard]] static RuviaMiddlewareList ruviaControllerGroupMiddlewares() {
        return {};
    }

    [[nodiscard]] static detail::ControllerRouteBuilder ruviaCreateRouteGroup(
        Router& router,
        std::string_view prefix,
        RuviaMiddlewareList middlewares) {
        return detail::ControllerRouteBuilder(router, prefix, std::move(middlewares));
    }

    [[nodiscard]] static detail::ControllerRouteBuilder ruviaCreateRouteGroup(
        const detail::ControllerRouteBuilder& scope,
        std::string_view prefix,
        RuviaMiddlewareList middlewares) {
        return scope.createScope(prefix, std::move(middlewares));
    }

    static void ruviaAddRoute(
        const detail::ControllerRouteBuilder& scope,
        HttpMethod method,
        std::pmr::string path,
        detail::ControllerRouteHandler handler,
        RequestBodyMode bodyMode,
        RuviaMiddlewareList middlewares) {
        scope.registerRoute(method, std::move(path), std::move(handler), bodyMode, std::move(middlewares));
    }

    static void ruviaAddStreamRoute(
        const detail::ControllerRouteBuilder& scope,
        HttpMethod method,
        std::pmr::string path,
        detail::ControllerRouteStreamHandler handler,
        ResponseBodyMode responseMode,
        RuviaMiddlewareList middlewares,
        WebSocketRouteOptions webSocketOptions = {}) {
        scope.registerStreamRoute(
            method,
            std::move(path),
            std::move(handler),
            responseMode,
            std::move(middlewares),
            webSocketOptions);
    }

    template <typename T, Task<HttpResponse> (T::*Handler)(Context&)>
    [[nodiscard]] static detail::ControllerRouteHandler bind(
        T* instance) noexcept {
        return detail::ControllerRouteHandler{instance, &Controller::invoke<T, Handler>};
    }

    template <typename T, Task<void> (T::*Handler)(Context&)>
    [[nodiscard]] static detail::ControllerRouteStreamHandler bindStream(
        T* instance) noexcept {
        return detail::ControllerRouteStreamHandler{instance, &Controller::invokeStream<T, Handler>};
    }

    template <typename MiddlewareT>
    [[nodiscard]] static detail::ControllerMiddlewareDescriptor ruviaMakeMiddleware() {
        return detail::makeMiddlewareDescriptor<MiddlewareT>();
    }

    template <typename... MiddlewareTs>
    [[nodiscard]] static RuviaMiddlewareList ruviaMakeMiddlewares() {
        return {ruviaMakeMiddleware<MiddlewareTs>()...};
    }

private:
    template <typename T, Task<HttpResponse> (T::*Handler)(Context&)>
    [[nodiscard]] static Task<HttpResponse> invoke(void* target, Context& context) {
        return (static_cast<T*>(target)->*Handler)(context);
    }

    template <typename T, Task<void> (T::*Handler)(Context&)>
    [[nodiscard]] static Task<void> invokeStream(void* target, Context& context) {
        return (static_cast<T*>(target)->*Handler)(context);
    }
};

namespace detail {

template <ValidationTarget Target, typename BodyT>
[[nodiscard]] Task<BodyT> parseValidatedBody(Context& c) {
    if constexpr (Target == ValidationTarget::kJson) {
        co_return co_await c.template json<BodyT>();
    } else if constexpr (Target == ValidationTarget::kForm) {
        co_return co_await c.template form<BodyT>();
    } else {
        static_assert(alwaysFalse<BodyT>, "unsupported validator target");
    }
}

template <ValidationTarget Target, typename BodyT, typename ValidatorT>
Task<HttpResponse> invokeModelValidator(
    const ValidatorT& validatorMiddleware,
    Context& c,
    const Next& next) {
    BodyT body = co_await parseValidatedBody<Target, BodyT>(c);
    Validator validator(c.resource());
    validatorMiddleware.validate(body, validator);
    std::move(validator).throwIfInvalid();
    c.setValid(Target, std::move(body));
    co_return co_await next(c);
}

template <typename ControllerT>
void registerControllerInstance(Router& router, ControllerStore& controllerLifetimes) {
    auto& controller = controllerLifetimes.emplace<ControllerT>();
    controller.registerRoutes(router);
}

template <typename ControllerT>
[[nodiscard]] bool registerController() {
    static_assert(
        std::is_base_of_v<Controller<ControllerT>, ControllerT>,
        "controller must derive from ruvia::Controller<ControllerT>");
    static_assert(std::is_final_v<ControllerT>, "controller must be final");
    static_assert(std::is_default_constructible_v<ControllerT>, "controller must be default constructible");

    return addControllerRegistrar(&registerControllerInstance<ControllerT>);
}

inline void registerControllers(Router& router, ControllerStore& controllerLifetimes) {
    runControllerRegistrars(router, controllerLifetimes);
}

}  // namespace detail

}  // namespace ruvia
