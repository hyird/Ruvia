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

[[nodiscard]] inline bool isFormEncodeSafe(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '*' ||
        c == '-' ||
        c == '.' ||
        c == '_';
}

inline void appendFormEncodedComponent(std::pmr::string& output, std::string_view input) {
    constexpr char kHex[] = "0123456789ABCDEF";
    for (const auto ch : input) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == ' ') {
            output.push_back('+');
        } else if (isFormEncodeSafe(c)) {
            output.push_back(static_cast<char>(c));
        } else {
            output.push_back('%');
            output.push_back(kHex[c >> 4]);
            output.push_back(kHex[c & 0x0f]);
        }
    }
}

inline void appendLowerFormEncodedComponent(std::pmr::string& output, std::string_view input) {
    constexpr char kHex[] = "0123456789ABCDEF";
    for (const auto ch : input) {
        auto c = static_cast<unsigned char>(ch);
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<unsigned char>(c - 'A' + 'a');
        }
        if (c == ' ') {
            output.push_back('+');
        } else if (isFormEncodeSafe(c)) {
            output.push_back(static_cast<char>(c));
        } else {
            output.push_back('%');
            output.push_back(kHex[c >> 4]);
            output.push_back(kHex[c & 0x0f]);
        }
    }
}

inline void appendRouteParamsAsForm(Context& c, std::pmr::string& output) {
    const auto& params = c.req().param();
    std::pmr::string scratch(c.resource());
    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto& param = params[i];
        if (i != 0) {
            output.push_back('&');
        }
        appendFormEncodedComponent(output, param.name);
        output.push_back('=');
        if (hasUrlEncoding(param.value, UrlDecodeMode::kPercent)) {
            if (!decodeUrlComponent(param.value, scratch, UrlDecodeMode::kPercent)) {
                throwInvalidParam();
            }
            appendFormEncodedComponent(output, scratch);
        } else {
            appendFormEncodedComponent(output, param.value);
        }
    }
}

inline void appendRequestHeadersAsForm(Context& c, std::pmr::string& output) {
    const auto headers = c.req().header();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto& header = headers[i];
        if (i != 0) {
            output.push_back('&');
        }
        appendLowerFormEncodedComponent(output, header.name);
        output.push_back('=');
        appendFormEncodedComponent(output, header.value);
    }
}

inline void appendRequestCookiesAsForm(Context& c, std::pmr::string& output) {
    const auto cookies = c.req().cookie();
    for (std::size_t i = 0; i < cookies.size(); ++i) {
        const auto& cookie = cookies[i];
        if (i != 0) {
            output.push_back('&');
        }
        appendFormEncodedComponent(output, cookie.name);
        output.push_back('=');
        appendFormEncodedComponent(output, cookie.value);
    }
}

template <ValidationTarget Target, typename BodyT>
[[nodiscard]] Task<BodyT> parseValidatedBody(Context& c) {
    if constexpr (Target == ValidationTarget::kJson) {
        co_return co_await c.req().template json<BodyT>();
    } else if constexpr (Target == ValidationTarget::kForm) {
        co_return co_await c.req().template form<BodyT>();
    } else if constexpr (Target == ValidationTarget::kQuery) {
        static_assert(FormBody<BodyT>::value, "query validator body type must use RUVIA_MODEL");
        auto parsed = FormBody<BodyT>::parse(c.req().queryString(), c.resource());
        if (!parsed) {
            detail::throwInvalidQuery();
        }
        co_return std::move(*parsed);
    } else if constexpr (Target == ValidationTarget::kParam) {
        static_assert(FormBody<BodyT>::value, "param validator body type must use RUVIA_MODEL");
        std::pmr::string params(c.resource());
        appendRouteParamsAsForm(c, params);
        auto parsed = FormBody<BodyT>::parse(detail::storeValidationInput(c, std::move(params)), c.resource());
        if (!parsed) {
            detail::throwInvalidParam();
        }
        co_return std::move(*parsed);
    } else if constexpr (Target == ValidationTarget::kHeader) {
        static_assert(FormBody<BodyT>::value, "header validator body type must use RUVIA_MODEL");
        std::pmr::string headers(c.resource());
        appendRequestHeadersAsForm(c, headers);
        auto parsed = FormBody<BodyT>::parse(detail::storeValidationInput(c, std::move(headers)), c.resource());
        if (!parsed) {
            detail::throwInvalidHeader();
        }
        co_return std::move(*parsed);
    } else if constexpr (Target == ValidationTarget::kCookie) {
        static_assert(FormBody<BodyT>::value, "cookie validator body type must use RUVIA_MODEL");
        std::pmr::string cookies(c.resource());
        appendRequestCookiesAsForm(c, cookies);
        auto parsed = FormBody<BodyT>::parse(detail::storeValidationInput(c, std::move(cookies)), c.resource());
        if (!parsed) {
            detail::throwInvalidCookie();
        }
        co_return std::move(*parsed);
    } else {
        static_assert(alwaysFalse<BodyT>, "unsupported validator target");
    }
}

template <ValidationTarget Target, typename BodyT, typename ValidatorT>
Task<void> invokeModelValidator(
    const ValidatorT& validatorMiddleware,
    Context& c,
    const Next& next) {
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
