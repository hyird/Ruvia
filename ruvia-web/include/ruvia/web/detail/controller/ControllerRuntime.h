#pragma once

#include "ruvia/web/detail/controller/ControllerDescriptors.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/middleware/MiddlewareRegistration.h"
#include "ruvia/web/Model.h"
#include "ruvia/http/UrlEncoding.h"
#include "ruvia/web/Validation.h"

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
    constexpr Controller() noexcept = default;
    ~Controller() = default;
};

namespace detail {

template <typename ControllerT>
class ControllerRegistrationAccess final {
    friend ControllerT;

    template <typename T>
    friend void registerControllerInstance(Router& router, ControllerStore& controllerLifetimes);

    using MiddlewareList = std::pmr::vector<ControllerMiddlewareDescriptor>;

    [[nodiscard]] static constexpr std::string_view groupPrefix() noexcept {
        if constexpr (requires { ControllerT::ruviaControllerGroupPrefix(); }) {
            return ControllerT::ruviaControllerGroupPrefix();
        } else {
            return {};
        }
    }

    [[nodiscard]] static MiddlewareList groupMiddlewares() {
        if constexpr (requires { ControllerT::ruviaControllerGroupMiddlewares(); }) {
            return ControllerT::ruviaControllerGroupMiddlewares();
        } else {
            return makeMiddlewares<>();
        }
    }

    [[nodiscard]] static ControllerRouteBuilder createRouteGroup(Router& router, std::string_view prefix, MiddlewareList middlewares) {
        return ControllerRouteBuilder(router, prefix, std::move(middlewares));
    }

    [[nodiscard]] static ControllerRouteBuilder createRouteGroup(const ControllerRouteBuilder& scope, std::string_view prefix, MiddlewareList middlewares) {
        return scope.createScope(prefix, std::move(middlewares));
    }

    static void addRoute(const ControllerRouteBuilder& scope, HttpKnownMethod method, std::string_view path, ControllerRouteHandler handler, RequestBodyMode bodyMode, std::span<const ControllerMiddlewareDescriptor> middlewares) {
        scope.registerRoute(method, path, std::move(handler), bodyMode, middlewares);
    }

    static void addExtensionMethodRoute(const ControllerRouteBuilder& scope, std::string_view methodToken, std::string_view path, ControllerRouteHandler handler, RequestBodyMode bodyMode, std::span<const ControllerMiddlewareDescriptor> middlewares) {
        scope.registerExtensionMethodRoute(methodToken, path, std::move(handler), bodyMode, middlewares);
    }

    static void addResponseStreamRoute(const ControllerRouteBuilder& scope, HttpKnownMethod method, std::string_view path, ControllerRouteStreamHandler handler, std::span<const ControllerMiddlewareDescriptor> middlewares) {
        scope.registerResponseStreamRoute(method, path, std::move(handler), middlewares);
    }

    static void addSseRoute(const ControllerRouteBuilder& scope, HttpKnownMethod method, std::string_view path, ControllerRouteStreamHandler handler, std::span<const ControllerMiddlewareDescriptor> middlewares) {
        scope.registerSseRoute(method, path, std::move(handler), middlewares);
    }

    static void addWebSocketRoute(const ControllerRouteBuilder& scope, HttpKnownMethod method, std::string_view path, ControllerRouteStreamHandler handler, std::span<const ControllerMiddlewareDescriptor> middlewares, WebSocketRouteOptions webSocketOptions = {}) {
        scope.registerWebSocketRoute(method, path, std::move(handler), middlewares, webSocketOptions);
    }

    template <Task<HttpResponse> (ControllerT::*Handler)(Context&)>
    [[nodiscard]] static ControllerRouteHandler bind(ControllerT* instance) noexcept {
        return ControllerRouteHandler(instance, &invoke<Handler>);
    }

    template <Task<void> (ControllerT::*Handler)(Context&)>
    [[nodiscard]] static ControllerRouteStreamHandler bindStream(ControllerT* instance) noexcept {
        return ControllerRouteStreamHandler(instance, &invokeStream<Handler>);
    }

    template <typename MiddlewareT>
    [[nodiscard]] static ControllerMiddlewareDescriptor makeMiddleware() {
        return makeMiddlewareDescriptor<MiddlewareT>();
    }

    template <typename... MiddlewareTs>
    [[nodiscard]] static MiddlewareList makeMiddlewares() {
        MiddlewareList middlewares(registrationResource());
        if constexpr (sizeof...(MiddlewareTs) > 0) {
            middlewares.reserve(sizeof...(MiddlewareTs));
            (middlewares.push_back(makeMiddleware<MiddlewareTs>()), ...);
        }
        return middlewares;
    }

    static void registerRoutes(ControllerT& controller, Router& router) {
        controller.registerRoutes(router);
    }

    template <Task<HttpResponse> (ControllerT::*Handler)(Context&)>
    [[nodiscard]] static Task<HttpResponse> invoke(void* target, Context& context) {
        return (static_cast<ControllerT*>(target)->*Handler)(context);
    }

    template <Task<void> (ControllerT::*Handler)(Context&)>
    [[nodiscard]] static Task<void> invokeStream(void* target, Context& context) {
        return (static_cast<ControllerT*>(target)->*Handler)(context);
    }
};

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
    static_assert(FormBody<BodyT>::value, "field validator body type must use RUVIA_REQUEST_MODEL");
    auto parsed = detail::ModelParseAccess::parseFormFieldsPartial<BodyT>(
        fields, c.resource());
    if (!parsed) {
        throwInvalidValidationTarget<Target>();
    }
    return std::move(*parsed);
}

template <ValidationTarget Target, typename BodyT>
[[nodiscard]] Task<BodyT> parseValidatedBody(Context& c) {
    if constexpr (Target == ValidationTarget::kJson) {
        if (!detail::contentTypeMatches(c.req().header("Content-Type").value_or(std::string_view{}), "application/json")) {
            detail::throwInvalidJsonContentType();
        }
        const auto requestBody = co_await c.req().text();
        auto parsed = detail::ModelParseAccess::parseJsonBorrowedPartial<BodyT>(
            requestBody, c.resource());
        if (!parsed) detail::throwInvalidJsonBody();
        co_return std::move(*parsed);
    } else if constexpr (Target == ValidationTarget::kForm) {
        if (!detail::contentTypeMatches(c.req().header("Content-Type").value_or(std::string_view{}), "application/x-www-form-urlencoded")) {
            detail::throwInvalidFormContentType();
        }
        const auto requestBody = co_await c.req().text();
        auto parsed = detail::ModelParseAccess::parseFormBorrowedPartial<BodyT>(
            requestBody, c.resource());
        if (!parsed) detail::throwInvalidFormBody();
        co_return std::move(*parsed);
    } else if constexpr (Target == ValidationTarget::kQuery) {
        co_return parseValidatedFields<Target, BodyT>(c, c.req().queryFields());
    } else if constexpr (Target == ValidationTarget::kParam) {
        co_return parseValidatedFields<Target, BodyT>(c, c.req().paramFields());
    } else if constexpr (Target == ValidationTarget::kHeader) {
        co_return parseValidatedFields<Target, BodyT>(c, c.req().headerFields());
    } else if constexpr (Target == ValidationTarget::kCookie) {
        co_return parseValidatedFields<Target, BodyT>(c, c.req().cookieFields());
    } else {
        static_assert(alwaysFalse<BodyT>, "unsupported validator target");
    }
}

template <ValidationTarget Target, typename BodyT, typename ValidatorT>
Task<void> invokeModelValidator(const ValidatorT& validatorMiddleware, Context& c, Next& next) {
    BodyT body = co_await parseValidatedBody<Target, BodyT>(c);
    Validator validator(c.resource());
    validatorMiddleware.validate(body, validator);
    std::move(validator).throwIfInvalid();
    if constexpr (Target == ValidationTarget::kJson) {
        const auto rawJson = co_await c.req().text();
        auto binding = bindValidatedJsonModel(c, body, rawJson);
        co_await next();
    } else {
        auto binding = bindValidatedModel(c, body);
        co_await next();
    }
}

template <typename ControllerT>
void registerControllerInstance(Router& router, ControllerStore& controllerLifetimes) {
    auto& controller = controllerLifetimes.emplace<ControllerT>();
    ControllerRegistrationAccess<ControllerT>::registerRoutes(controller, router);
}

template <typename ControllerT>
[[nodiscard]] bool registerController() {
    static_assert(std::is_base_of_v<Controller<ControllerT>, ControllerT>, "controller must derive from ruvia::Controller<ControllerT>");
    static_assert(std::is_final_v<ControllerT>, "controller must be final");
    static_assert(std::is_default_constructible_v<ControllerT>, "controller must be default constructible");

    return addControllerRegistrar(&registerControllerInstance<ControllerT>);
}

inline void registerControllers(Router& router, ControllerStore& controllerLifetimes, std::span<const ControllerRegistrar> registrars) {
    runControllerRegistrars(router, controllerLifetimes, registrars);
}

}  // namespace detail

}  // namespace ruvia
