#pragma once

#include "ruvia/http/ControllerDescriptors.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/ContextModel.h"
#include "ruvia/http/MiddlewareRuntime.h"
#include "ruvia/http/Model.h"
#include "ruvia/http/UrlEncoding.h"
#include "ruvia/http/Validation.h"

#include <memory_resource>
#include <span>
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
        return RuviaMiddlewareList(detail::registrationResource());
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
        std::string_view path,
        detail::ControllerRouteHandler handler,
        RequestBodyMode bodyMode,
        std::span<const detail::ControllerMiddlewareDescriptor> middlewares) {
        scope.registerRoute(method, path, std::move(handler), bodyMode, middlewares);
    }

    static void ruviaAddDynamicRoute(
        const detail::ControllerRouteBuilder& scope,
        HttpMethod method,
        std::string_view path,
        detail::ControllerRouteHandler handler,
        RequestBodyMode bodyMode,
        std::span<const detail::ControllerMiddlewareDescriptor> middlewares) {
        scope.registerRoute(
            method,
            path,
            std::move(handler),
            bodyMode,
            middlewares,
            ResponseBodyMode::kDynamic);
    }

    static void ruviaAddStreamRoute(
        const detail::ControllerRouteBuilder& scope,
        HttpMethod method,
        std::string_view path,
        detail::ControllerRouteStreamHandler handler,
        ResponseBodyMode responseMode,
        std::span<const detail::ControllerMiddlewareDescriptor> middlewares,
        WebSocketRouteOptions webSocketOptions = {}) {
        scope.registerStreamRoute(
            method,
            path,
            std::move(handler),
            responseMode,
            middlewares,
            webSocketOptions);
    }

    template <typename T, Task<HttpResponse> (T::*Handler)(Context&)>
    [[nodiscard]] static detail::ControllerRouteHandler bind(
        T* instance) noexcept {
        return detail::ControllerRouteHandler(instance, &Controller::invoke<T, Handler>);
    }

    template <typename T, Task<void> (T::*Handler)(Context&)>
    [[nodiscard]] static detail::ControllerRouteStreamHandler bindStream(
        T* instance) noexcept {
        return detail::ControllerRouteStreamHandler(instance, &Controller::invokeStream<T, Handler>);
    }

    template <typename MiddlewareT>
    [[nodiscard]] static detail::ControllerMiddlewareDescriptor ruviaMakeMiddleware() {
        return detail::makeMiddlewareDescriptor<MiddlewareT>();
    }

    template <typename... MiddlewareTs>
    [[nodiscard]] static RuviaMiddlewareList ruviaMakeMiddlewares() {
        RuviaMiddlewareList middlewares(detail::registrationResource());
        if constexpr (sizeof...(MiddlewareTs) > 0) {
            middlewares.reserve(sizeof...(MiddlewareTs));
            (middlewares.push_back(ruviaMakeMiddleware<MiddlewareTs>()), ...);
        }
        return middlewares;
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

template <ValidationTarget Target>
[[noreturn]] inline void throwInvalidValidationTarget() {
    if constexpr (Target == ValidationTarget::kQuery) {
        detail::throwInvalidQuery();
    } else if constexpr (Target == ValidationTarget::kParam) {
        detail::throwInvalidParam();
    } else if constexpr (Target == ValidationTarget::kHeader) {
        detail::throwInvalidHeader();
    } else if constexpr (Target == ValidationTarget::kCookie) {
        detail::throwInvalidCookie();
    } else {
        static_assert(alwaysFalse<std::integral_constant<ValidationTarget, Target>>, "unsupported validator target");
    }
}

template <ValidationTarget Target, typename BodyT>
[[nodiscard]] BodyT parseValidatedFields(Context& c, const RequestNameValueList& fields) {
    static_assert(FormBody<BodyT>::value, "field validator body type must use RUVIA_MODEL");
    auto parsed = FormBody<BodyT>::parseFields(fields, c.resource());
    if (!parsed) {
        throwInvalidValidationTarget<Target>();
    }
    return std::move(*parsed);
}

template <ValidationTarget Target, typename BodyT>
[[nodiscard]] Task<BodyT> parseValidatedBody(Context& c) {
    if constexpr (Target == ValidationTarget::kJson) {
        co_return co_await c.req().template json<BodyT>();
    } else if constexpr (Target == ValidationTarget::kForm) {
        co_return co_await c.req().template form<BodyT>();
    } else if constexpr (Target == ValidationTarget::kQuery) {
        co_return parseValidatedFields<Target, BodyT>(c, c.req().query());
    } else if constexpr (Target == ValidationTarget::kParam) {
        co_return parseValidatedFields<Target, BodyT>(c, c.req().param());
    } else if constexpr (Target == ValidationTarget::kHeader) {
        co_return parseValidatedFields<Target, BodyT>(c, c.req().header());
    } else if constexpr (Target == ValidationTarget::kCookie) {
        co_return parseValidatedFields<Target, BodyT>(c, c.req().cookie());
    } else {
        static_assert(alwaysFalse<BodyT>, "unsupported validator target");
    }
}

template <ValidationTarget Target, typename BodyT, typename ValidatorT>
Task<void> invokeModelValidator(
    const ValidatorT& validatorMiddleware,
    Context& c,
    Next& next) {
    BodyT body = co_await parseValidatedBody<Target, BodyT>(c);
    Validator validator(c.resource());
    validatorMiddleware.validate(body, validator);
    std::move(validator).throwIfInvalid();
    setValidatedBody(c, Target, std::move(body));
    co_await next();
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
