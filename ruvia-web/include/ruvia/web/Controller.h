#pragma once

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/web/Middleware.h"
#include "ruvia/http/detail/util/BorrowedView.h"
#include "ruvia/web/detail/controller/ControllerRuntime.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <string_view>
#include <utility>

namespace ruvia::detail {

// Methods covered by RUVIA_ALL. HEAD is intentionally omitted: buffered GET
// routes receive an implicit HEAD shadow during route-table build, while
// streaming/SSE/WebSocket routes require an explicit HEAD route.
inline constexpr std::array kRuviaAllRouteMethods = {HttpKnownMethod::kGet, HttpKnownMethod::kPost,
    HttpKnownMethod::kPut, HttpKnownMethod::kPatch, HttpKnownMethod::kDelete,
    HttpKnownMethod::kOptions};

// Startup-only holder for the RUVIA_ON method list; the macro pastes the
// parenthesized list as a constructor call.
class RuviaMethodList final {
public:
    template <std::same_as<HttpKnownMethod>... Methods>
    constexpr explicit RuviaMethodList(Methods... methods) noexcept
        : methods_{methods...},
          count_(sizeof...(Methods)) {
        static_assert(sizeof...(Methods) > 0, "RUVIA_ON requires at least one method");
        static_assert(sizeof...(Methods) <= 9, "RUVIA_ON supports at most 9 methods");
    }

    [[nodiscard]] constexpr const HttpKnownMethod* begin() const& noexcept {
        return methods_.data();
    }
    [[nodiscard]] constexpr const HttpKnownMethod* begin() const&& = delete;

    [[nodiscard]] constexpr const HttpKnownMethod* end() const& noexcept {
        return methods_.data() + count_;
    }
    [[nodiscard]] constexpr const HttpKnownMethod* end() const&& = delete;

private:
    std::array<HttpKnownMethod, 9> methods_{};
    std::size_t count_{0};
};

// Startup-only holder for the RUVIA_ON path list; the macro pastes the
// parenthesized list as a constructor call.
class RuviaPathList final {
public:
    template <typename... Paths>
        requires((std::convertible_to<Paths &&, std::string_view> && ...) &&
                    (!HttpTemporaryOwningCharString<Paths> && ...))
    constexpr explicit RuviaPathList(Paths&&... paths) noexcept
        : paths_{httpBorrowedView(paths)...},
          count_(sizeof...(Paths)) {
        static_assert(sizeof...(Paths) > 0, "RUVIA_ON requires at least one path");
        static_assert(sizeof...(Paths) <= 8, "RUVIA_ON supports at most 8 paths");
    }

    template <typename... Paths>
        requires((std::convertible_to<Paths &&, std::string_view> && ...) &&
                    (HttpTemporaryOwningCharString<Paths> || ...))
    explicit RuviaPathList(Paths&&...) = delete;

    [[nodiscard]] constexpr const std::string_view* begin() const& noexcept {
        return paths_.data();
    }
    [[nodiscard]] constexpr const std::string_view* begin() const&& = delete;

    [[nodiscard]] constexpr const std::string_view* end() const& noexcept {
        return paths_.data() + count_;
    }
    [[nodiscard]] constexpr const std::string_view* end() const&& = delete;

private:
    std::array<std::string_view, 8> paths_{};
    std::size_t count_{0};
};

}  // namespace ruvia::detail

#define RUVIA_CONTROLLER_GROUP(prefix, ...)                                                   \
private:                                                                                      \
    [[nodiscard]] static constexpr ::std::string_view ruviaControllerGroupPrefix() noexcept { \
        static_assert(!::ruvia::detail::HttpTemporaryOwningCharString<decltype((prefix))>,    \
            "controller group prefixes must outlive route registration");                     \
        return ::ruvia::detail::httpBorrowedView(prefix);                                     \
    }                                                                                         \
    [[nodiscard]] static auto ruviaControllerGroupMiddlewares() {                             \
        return ::ruvia::detail::ControllerRegistrationAccess<                                 \
            RuviaControllerType>::template makeMiddlewares<__VA_ARGS__>();                    \
    }

#define RUVIA_ROUTES_BEGIN                                                                    \
private:                                                                                      \
    using RuviaControllerAccess =                                                             \
        ::ruvia::detail::ControllerRegistrationAccess<RuviaControllerType>;                   \
    friend class ::ruvia::detail::ControllerRegistrationAccess<RuviaControllerType>;          \
    void registerRoutes(::ruvia::detail::Router& router) {                                    \
        auto ruviaControllerGroup = RuviaControllerAccess::createRouteGroup(router,           \
            RuviaControllerAccess::groupPrefix(), RuviaControllerAccess::groupMiddlewares()); \
        [[maybe_unused]] auto& ruviaRouteScope = ruviaControllerGroup;

#if defined(_MSC_VER)
// selectany is invalid when a controller has internal linkage (for example in
// an anonymous namespace). MSVC already retains dynamic inline-variable
// initialization, so no additional declaration attribute is needed here.
#define RUVIA_DETAIL_CONTROLLER_RETAIN
#elif defined(__GNUC__) || defined(__clang__)
#define RUVIA_DETAIL_CONTROLLER_RETAIN __attribute__((used))
#else
#define RUVIA_DETAIL_CONTROLLER_RETAIN
#endif

#define RUVIA_ROUTES_END                                                                 \
    }                                                                                    \
    RUVIA_DETAIL_CONTROLLER_RETAIN inline static const bool ruviaControllerRegistered_ = \
        ::ruvia::detail::registerController<RuviaControllerType>();

#define RUVIA_GET(path, handler, ...)                                                      \
    RuviaControllerAccess::addRoute(ruviaRouteScope, ::ruvia::HttpKnownMethod::kGet, path, \
        RuviaControllerAccess::template bind<&RuviaControllerType::handler>(this),         \
        ::ruvia::detail::RequestBodyMode::kBuffered,                                       \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

#define RUVIA_GET_STREAM(path, handler, ...)                                                       \
    RuviaControllerAccess::addResponseStreamRoute(ruviaRouteScope, ::ruvia::HttpKnownMethod::kGet, \
        path, RuviaControllerAccess::template bindStream<&RuviaControllerType::handler>(this),     \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

#define RUVIA_GET_SSE(path, handler, ...)                                                     \
    RuviaControllerAccess::addSseRoute(ruviaRouteScope, ::ruvia::HttpKnownMethod::kGet, path, \
        RuviaControllerAccess::template bindStream<&RuviaControllerType::handler>(this),      \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

#define RUVIA_GET_WS(path, handler, ...)                                                       \
    RuviaControllerAccess::addWebSocketRoute(ruviaRouteScope, ::ruvia::HttpKnownMethod::kGet,  \
        path, RuviaControllerAccess::template bindStream<&RuviaControllerType::handler>(this), \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

#define RUVIA_GET_WS_OPTIONS(path, handler, options, ...)                                      \
    RuviaControllerAccess::addWebSocketRoute(ruviaRouteScope, ::ruvia::HttpKnownMethod::kGet,  \
        path, RuviaControllerAccess::template bindStream<&RuviaControllerType::handler>(this), \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>(), options)

#define RUVIA_POST(path, handler, ...)                                                      \
    RuviaControllerAccess::addRoute(ruviaRouteScope, ::ruvia::HttpKnownMethod::kPost, path, \
        RuviaControllerAccess::template bind<&RuviaControllerType::handler>(this),          \
        ::ruvia::detail::RequestBodyMode::kBuffered,                                        \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

#define RUVIA_PUT(path, handler, ...)                                                      \
    RuviaControllerAccess::addRoute(ruviaRouteScope, ::ruvia::HttpKnownMethod::kPut, path, \
        RuviaControllerAccess::template bind<&RuviaControllerType::handler>(this),         \
        ::ruvia::detail::RequestBodyMode::kBuffered,                                       \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

#define RUVIA_DELETE(path, handler, ...)                                                      \
    RuviaControllerAccess::addRoute(ruviaRouteScope, ::ruvia::HttpKnownMethod::kDelete, path, \
        RuviaControllerAccess::template bind<&RuviaControllerType::handler>(this),            \
        ::ruvia::detail::RequestBodyMode::kBuffered,                                          \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

#define RUVIA_PATCH(path, handler, ...)                                                      \
    RuviaControllerAccess::addRoute(ruviaRouteScope, ::ruvia::HttpKnownMethod::kPatch, path, \
        RuviaControllerAccess::template bind<&RuviaControllerType::handler>(this),           \
        ::ruvia::detail::RequestBodyMode::kBuffered,                                         \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

#define RUVIA_HEAD(path, handler, ...)                                                      \
    RuviaControllerAccess::addRoute(ruviaRouteScope, ::ruvia::HttpKnownMethod::kHead, path, \
        RuviaControllerAccess::template bind<&RuviaControllerType::handler>(this),          \
        ::ruvia::detail::RequestBodyMode::kBuffered,                                        \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

#define RUVIA_OPTIONS(path, handler, ...)                                                      \
    RuviaControllerAccess::addRoute(ruviaRouteScope, ::ruvia::HttpKnownMethod::kOptions, path, \
        RuviaControllerAccess::template bind<&RuviaControllerType::handler>(this),             \
        ::ruvia::detail::RequestBodyMode::kBuffered,                                           \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

// An extension method, routed by its exact wire token: PROPFIND, PURGE, LOCK.
// HTTP's method space is open (HttpKnownMethod is only this framework's fixed
// classification, never the wire value), but routes were keyed on that enum, so
// anything outside it answered 501 and could not be served at all.
//
// The token is case-sensitive per RFC 9110 9.1, must be a valid method token,
// and must NOT be one of the classified methods -- those keep the enum as their
// identity and their own macros. The path must be static: extension routes are
// matched by a cold linear scan with no dynamic-segment index behind it.
#define RUVIA_METHOD(method_token, path, handler, ...)                                  \
    RuviaControllerAccess::addExtensionMethodRoute(ruviaRouteScope, method_token, path, \
        RuviaControllerAccess::template bind<&RuviaControllerType::handler>(this),      \
        ::ruvia::detail::RequestBodyMode::kBuffered,                                    \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

// Hono app.all: registers the handler for GET/POST/PUT/PATCH/DELETE/OPTIONS.
#define RUVIA_ALL(path, handler, ...)                                              \
    for (const auto ruviaRouteMethod : ::ruvia::detail::kRuviaAllRouteMethods)     \
    RuviaControllerAccess::addRoute(ruviaRouteScope, ruviaRouteMethod, path,       \
        RuviaControllerAccess::template bind<&RuviaControllerType::handler>(this), \
        ::ruvia::detail::RequestBodyMode::kBuffered,                               \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

// Hono app.on: registers the handler for an explicit method x path list, e.g.
// RUVIA_ON((ruvia::HttpKnownMethod::kPut, ruvia::HttpKnownMethod::kDelete),
//     ("/items/:id", "/legacy/:id"), handler).
#define RUVIA_ON(methods, paths, handler, ...)                                         \
    for (const auto ruviaRouteMethod : ::ruvia::detail::RuviaMethodList methods)       \
        for (const auto ruviaRoutePath : ::ruvia::detail::RuviaPathList paths)         \
    RuviaControllerAccess::addRoute(ruviaRouteScope, ruviaRouteMethod, ruviaRoutePath, \
        RuviaControllerAccess::template bind<&RuviaControllerType::handler>(this),     \
        ::ruvia::detail::RequestBodyMode::kBuffered,                                   \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

#define RUVIA_POST_STREAM(path, handler, ...)                                               \
    RuviaControllerAccess::addRoute(ruviaRouteScope, ::ruvia::HttpKnownMethod::kPost, path, \
        RuviaControllerAccess::template bind<&RuviaControllerType::handler>(this),          \
        ::ruvia::detail::RequestBodyMode::kStream,                                          \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

#define RUVIA_PUT_STREAM(path, handler, ...)                                               \
    RuviaControllerAccess::addRoute(ruviaRouteScope, ::ruvia::HttpKnownMethod::kPut, path, \
        RuviaControllerAccess::template bind<&RuviaControllerType::handler>(this),         \
        ::ruvia::detail::RequestBodyMode::kStream,                                         \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

#define RUVIA_PATCH_STREAM(path, handler, ...)                                               \
    RuviaControllerAccess::addRoute(ruviaRouteScope, ::ruvia::HttpKnownMethod::kPatch, path, \
        RuviaControllerAccess::template bind<&RuviaControllerType::handler>(this),           \
        ::ruvia::detail::RequestBodyMode::kStream,                                           \
        RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>())

#define RUVIA_GROUP_BEGIN(prefix, ...)                                                     \
    {                                                                                      \
        static_assert(!::ruvia::detail::HttpTemporaryOwningCharString<decltype((prefix))>, \
            "route group prefixes must outlive route registration");                       \
        auto ruviaRouteGroup = RuviaControllerAccess::createRouteGroup(ruviaRouteScope,    \
            ::ruvia::detail::httpBorrowedView(prefix),                                     \
            RuviaControllerAccess::template makeMiddlewares<__VA_ARGS__>());               \
        auto& ruviaRouteScope = ruviaRouteGroup;

#define RUVIA_GROUP_END }

#define RUVIA_VALIDATE_BODY(target, body_type, ...)                                              \
public:                                                                                          \
    static_assert(::ruvia::detail::isRequestModel<body_type>,                                    \
        "RUVIA_VALIDATE_* requires a RUVIA_REQUEST_MODEL");                                      \
    using RuviaValidationBody = body_type;                                                       \
    void validate(const body_type& body, ::ruvia::Validator& validator) const {                  \
        ::ruvia::detail::ModelValidationAccess::validateStructure(body, {}, validator);          \
        validateNested(body, {}, validator);                                                     \
    }                                                                                            \
    void validateNested(                                                                         \
        const body_type& body, ::std::string_view prefix, ::ruvia::Validator& validator) const { \
        RUVIA_VALIDATION_FOR_EACH(RUVIA_VALIDATE_RULE_FIELD, body_type, __VA_ARGS__)             \
    }                                                                                            \
    [[nodiscard]] ::ruvia::Task<void> handle(::ruvia::Context& c, ::ruvia::Next& next) {         \
        return ::ruvia::detail::invokeModelValidator<target, body_type>(*this, c, next);         \
    }

#define RUVIA_VALIDATE_JSON(body_type, ...) \
    RUVIA_VALIDATE_BODY(::ruvia::ValidationTarget::kJson, body_type, __VA_ARGS__)

#define RUVIA_VALIDATE_FORM(body_type, ...) \
    RUVIA_VALIDATE_BODY(::ruvia::ValidationTarget::kForm, body_type, __VA_ARGS__)

#define RUVIA_VALIDATE_QUERY(body_type, ...) \
    RUVIA_VALIDATE_BODY(::ruvia::ValidationTarget::kQuery, body_type, __VA_ARGS__)

#define RUVIA_VALIDATE_PARAM(body_type, ...) \
    RUVIA_VALIDATE_BODY(::ruvia::ValidationTarget::kParam, body_type, __VA_ARGS__)

#define RUVIA_VALIDATE_HEADER(body_type, ...) \
    RUVIA_VALIDATE_BODY(::ruvia::ValidationTarget::kHeader, body_type, __VA_ARGS__)

#define RUVIA_VALIDATE_COOKIE(body_type, ...) \
    RUVIA_VALIDATE_BODY(::ruvia::ValidationTarget::kCookie, body_type, __VA_ARGS__)
