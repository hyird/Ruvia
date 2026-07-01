#pragma once

#include "ruvia/http/ControllerTypes.h"

#define RUVIA_CONTROLLER_GROUP(prefix, ...) \
    [[nodiscard]] static constexpr ::std::string_view ruviaControllerGroupPrefix() noexcept { \
        return prefix; \
    } \
    [[nodiscard]] static RuviaMiddlewareList ruviaControllerGroupMiddlewares() { \
        return RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>(); \
    }

#define RUVIA_ROUTES_BEGIN \
    void registerRoutes(::ruvia::Router& router) { \
        auto ruviaControllerGroup = RuviaControllerType::ruviaCreateRouteGroup( \
            router, \
            RuviaControllerType::ruviaControllerGroupPrefix(), \
            RuviaControllerType::ruviaControllerGroupMiddlewares()); \
        auto& ruviaRouteScope = ruviaControllerGroup;

#define RUVIA_ROUTES_END \
    } \
    inline static const bool ruviaControllerRegistered_ = \
        ::ruvia::detail::registerController<RuviaControllerType>();

#define RUVIA_GET(path, handler, ...) \
    RuviaControllerType::ruviaAddRoute( \
        ruviaRouteScope, \
        ::ruvia::Get, \
        path, \
        bind<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::RequestBodyMode::kBuffered, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>())

// Dynamic-response routes: the handler returns an HttpResponse but may instead
// stream via c.stream()/c.streamSSE(); whichever it does at runtime is honored.
#define RUVIA_GET_DYNAMIC(path, handler, ...) \
    RuviaControllerType::ruviaAddDynamicRoute( \
        ruviaRouteScope, \
        ::ruvia::Get, \
        path, \
        bind<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::RequestBodyMode::kBuffered, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>())

#define RUVIA_POST_DYNAMIC(path, handler, ...) \
    RuviaControllerType::ruviaAddDynamicRoute( \
        ruviaRouteScope, \
        ::ruvia::Post, \
        path, \
        bind<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::RequestBodyMode::kBuffered, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>())

#define RUVIA_GET_STREAM(path, handler, ...) \
    RuviaControllerType::ruviaAddStreamRoute( \
        ruviaRouteScope, \
        ::ruvia::Get, \
        path, \
        bindStream<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::ResponseBodyMode::kStream, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>())

#define RUVIA_GET_SSE(path, handler, ...) \
    RuviaControllerType::ruviaAddStreamRoute( \
        ruviaRouteScope, \
        ::ruvia::Get, \
        path, \
        bindStream<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::ResponseBodyMode::kSse, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>())

#define RUVIA_GET_WS(path, handler, ...) \
    RuviaControllerType::ruviaAddStreamRoute( \
        ruviaRouteScope, \
        ::ruvia::Get, \
        path, \
        bindStream<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::ResponseBodyMode::kWebSocket, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>())

#define RUVIA_GET_WS_OPTIONS(path, handler, options, ...) \
    RuviaControllerType::ruviaAddStreamRoute( \
        ruviaRouteScope, \
        ::ruvia::Get, \
        path, \
        bindStream<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::ResponseBodyMode::kWebSocket, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>(), \
        options)

#define RUVIA_POST(path, handler, ...) \
    RuviaControllerType::ruviaAddRoute( \
        ruviaRouteScope, \
        ::ruvia::Post, \
        path, \
        bind<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::RequestBodyMode::kBuffered, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>())

#define RUVIA_PUT(path, handler, ...) \
    RuviaControllerType::ruviaAddRoute( \
        ruviaRouteScope, \
        ::ruvia::Put, \
        path, \
        bind<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::RequestBodyMode::kBuffered, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>())

#define RUVIA_DELETE(path, handler, ...) \
    RuviaControllerType::ruviaAddRoute( \
        ruviaRouteScope, \
        ::ruvia::Delete, \
        path, \
        bind<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::RequestBodyMode::kBuffered, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>())

#define RUVIA_PATCH(path, handler, ...) \
    RuviaControllerType::ruviaAddRoute( \
        ruviaRouteScope, \
        ::ruvia::Patch, \
        path, \
        bind<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::RequestBodyMode::kBuffered, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>())

#define RUVIA_HEAD(path, handler, ...) \
    RuviaControllerType::ruviaAddRoute( \
        ruviaRouteScope, \
        ::ruvia::Head, \
        path, \
        bind<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::RequestBodyMode::kBuffered, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>())

#define RUVIA_OPTIONS(path, handler, ...) \
    RuviaControllerType::ruviaAddRoute( \
        ruviaRouteScope, \
        ::ruvia::Options, \
        path, \
        bind<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::RequestBodyMode::kBuffered, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>())

#define RUVIA_POST_STREAM(path, handler, ...) \
    RuviaControllerType::ruviaAddRoute( \
        ruviaRouteScope, \
        ::ruvia::Post, \
        path, \
        bind<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::RequestBodyMode::kStream, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>())

#define RUVIA_PUT_STREAM(path, handler, ...) \
    RuviaControllerType::ruviaAddRoute( \
        ruviaRouteScope, \
        ::ruvia::Put, \
        path, \
        bind<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::RequestBodyMode::kStream, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>())

#define RUVIA_PATCH_STREAM(path, handler, ...) \
    RuviaControllerType::ruviaAddRoute( \
        ruviaRouteScope, \
        ::ruvia::Patch, \
        path, \
        bind<RuviaControllerType, &RuviaControllerType::handler>(this), \
        ::ruvia::RequestBodyMode::kStream, \
        RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>())

#define RUVIA_GROUP_BEGIN(prefix, ...) \
    { \
        auto ruviaRouteGroup = RuviaControllerType::ruviaCreateRouteGroup( \
            ruviaRouteScope, \
            prefix, \
            RuviaControllerType::template ruviaMakeMiddlewares<__VA_ARGS__>()); \
        auto& ruviaRouteScope = ruviaRouteGroup;

#define RUVIA_GROUP_END }

#define RUVIA_VALIDATE_BODY(target, body_type, ...) \
public: \
    using RuviaValidationBody = body_type; \
    void validate(const body_type& body, ::ruvia::Validator& validator) const { \
        validateNested(body, {}, validator); \
    } \
    void validateNested( \
        const body_type& body, \
        ::std::string_view prefix, \
        ::ruvia::Validator& validator) const { \
        RUVIA_MODEL_FOR_EACH(RUVIA_VALIDATE_RULE_FIELD, body_type, __VA_ARGS__) \
    } \
    [[nodiscard]] ::ruvia::Task<void> handle( \
        ::ruvia::Context& c, \
        const ::ruvia::Next& next) { \
        return ::ruvia::detail::invokeModelValidator< \
            target, \
            body_type>(*this, c, next); \
    }

#define RUVIA_VALIDATE_JSON(body_type, ...) \
    RUVIA_VALIDATE_BODY(::ruvia::ValidationTarget::kJson, body_type, __VA_ARGS__)

#define RUVIA_VALIDATE_FORM(body_type, ...) \
    RUVIA_VALIDATE_BODY(::ruvia::ValidationTarget::kForm, body_type, __VA_ARGS__)

#define RUVIA_VALIDATE_QUERY(body_type, ...) \
    RUVIA_VALIDATE_BODY(::ruvia::ValidationTarget::kQuery, body_type, __VA_ARGS__)

#define RUVIA_VALIDATE_PARAM(body_type, ...) \
    RUVIA_VALIDATE_BODY(::ruvia::ValidationTarget::kParam, body_type, __VA_ARGS__)
