#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/HeaderUtils.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/Model.h"
#include "ruvia/http/Validation.h"
#include "ruvia/memory/MemoryPool.h"
#include "ruvia/router/Router.h"

namespace ruvia {

namespace detail {

template <typename T>
concept TaskHttpResponse = std::same_as<std::remove_cvref_t<T>, Task<HttpResponse>>;

template <typename MiddlewareT>
concept AwaitableHandleMiddleware = requires(
    MiddlewareT middleware,
    Context& context,
    const Next& next) {
    { middleware.handle(context, next) } -> TaskHttpResponse;
};

template <typename MiddlewareT>
[[nodiscard]] Task<HttpResponse> invokeMiddleware(
    void* target,
    Context& context,
    const Next& next) {
    auto* middleware = static_cast<MiddlewareT*>(target);
    if constexpr (AwaitableHandleMiddleware<MiddlewareT>) {
        return middleware->handle(context, next);
    } else {
        static_assert(
            AwaitableHandleMiddleware<MiddlewareT>,
            "middleware must implement async handle(Context&, const ruvia::Next&)");
    }
}

template <typename MiddlewareT>
[[nodiscard]] void* createMiddleware();

template <typename MiddlewareT>
void destroyMiddleware(void* target) noexcept;

class ControllerStore final {
public:
    ControllerStore() = default;
    ControllerStore(const ControllerStore&) = delete;
    ControllerStore& operator=(const ControllerStore&) = delete;
    ControllerStore(ControllerStore&&) noexcept = default;
    ControllerStore& operator=(ControllerStore&&) noexcept = default;

    template <typename T, typename... Args>
    T& emplace(Args&&... args) {
        auto value = std::make_unique<T>(std::forward<Args>(args)...);
        auto* raw = value.get();
        lifetimes_.emplace_back(raw, &ControllerStore::destroy<T>);
        value.release();
        return *raw;
    }

    void reserve(std::size_t count) {
        lifetimes_.reserve(count);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return lifetimes_.size();
    }

private:
    struct Lifetime {
        void* target{nullptr};
        void (*destroy)(void*) noexcept{nullptr};

        Lifetime() noexcept = default;
        Lifetime(void* targetValue, void (*destroyValue)(void*) noexcept) noexcept
            : target(targetValue), destroy(destroyValue) {}
        Lifetime(const Lifetime&) = delete;
        Lifetime& operator=(const Lifetime&) = delete;
        Lifetime(Lifetime&& other) noexcept
            : target(std::exchange(other.target, nullptr)),
              destroy(std::exchange(other.destroy, nullptr)) {}
        Lifetime& operator=(Lifetime&& other) noexcept {
            if (this == &other) {
                return *this;
            }
            reset();
            target = std::exchange(other.target, nullptr);
            destroy = std::exchange(other.destroy, nullptr);
            return *this;
        }
        ~Lifetime() {
            reset();
        }

        void reset() noexcept {
            if (target != nullptr && destroy != nullptr) {
                destroy(target);
            }
            target = nullptr;
            destroy = nullptr;
        }
    };

    template <typename T>
    static void destroy(void* target) noexcept {
        delete static_cast<T*>(target);
    }

    std::pmr::vector<Lifetime> lifetimes_{ProcessMemory::instance().upstreamResource()};
};

struct ControllerRouteHandler final {
    void* target{nullptr};
    Next::Invoke invoke{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke != nullptr;
    }

    [[nodiscard]] Task<HttpResponse> operator()(Context& context) const {
        if (invoke == nullptr) {
            throw std::logic_error("route handler is empty");
        }
        return invoke(target, context);
    }
};

struct ControllerRouteStreamHandler final {
    using Invoke = Task<void> (*)(void*, Context&);

    void* target{nullptr};
    Invoke invoke{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke != nullptr;
    }

    [[nodiscard]] Task<void> operator()(Context& context) const {
        if (invoke == nullptr) {
            throw std::logic_error("route stream handler is empty");
        }
        return invoke(target, context);
    }
};

struct ControllerMiddlewareDescriptor final {
    using Invoke = Task<HttpResponse> (*)(void*, Context&, const Next&);
    using Create = void* (*)();
    using Destroy = void (*)(void*) noexcept;

    Invoke invoke{nullptr};
    Create create{nullptr};
    Destroy destroy{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke != nullptr && create != nullptr && destroy != nullptr;
    }
};

template <typename MiddlewareT>
[[nodiscard]] ControllerMiddlewareDescriptor makeMiddlewareDescriptor();

class ControllerRouteBuilder final {
public:
    ControllerRouteBuilder(
        Router& router,
        std::string_view prefix,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares = {});
    ControllerRouteBuilder(ControllerRouteBuilder&&) noexcept;
    ControllerRouteBuilder& operator=(ControllerRouteBuilder&&) noexcept;
    ControllerRouteBuilder(const ControllerRouteBuilder&) = delete;
    ControllerRouteBuilder& operator=(const ControllerRouteBuilder&) = delete;
    ~ControllerRouteBuilder();

    void registerRoute(
        HttpMethod method,
        std::pmr::string path,
        ControllerRouteHandler handler,
        RequestBodyMode bodyMode,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares = {}) const;
    void registerStreamRoute(
        HttpMethod method,
        std::pmr::string path,
        ControllerRouteStreamHandler handler,
        ResponseBodyMode responseMode,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares = {},
        WebSocketRouteOptions webSocketOptions = {}) const;
    [[nodiscard]] ControllerRouteBuilder createScope(
        std::string_view prefix,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares = {}) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace detail

template <typename MiddlewareT>
class Middleware;

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

template <typename MiddlewareT>
class Middleware {
public:
    using RuviaMiddlewareType = MiddlewareT;

protected:
    constexpr Middleware() noexcept = default;
    ~Middleware() = default;
};

namespace detail {

template <typename MiddlewareT>
[[nodiscard]] ControllerMiddlewareDescriptor makeMiddlewareDescriptor() {
    static_assert(
        std::is_base_of_v<Middleware<MiddlewareT>, MiddlewareT>,
        "middleware must derive from ruvia::Middleware<MiddlewareT>");
    static_assert(std::is_final_v<MiddlewareT>, "middleware must be final");
    static_assert(std::is_default_constructible_v<MiddlewareT>, "middleware must be default constructible");

    return ControllerMiddlewareDescriptor{
        &invokeMiddleware<MiddlewareT>,
        &createMiddleware<MiddlewareT>,
        &destroyMiddleware<MiddlewareT>};
}

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
    validator.throwIfInvalid();
    c.setValid(Target, std::move(body));
    co_return co_await next(c);
}

using ControllerRegistrar = void (*)(Router&, ControllerStore&);

[[nodiscard]] bool addControllerRegistrar(ControllerRegistrar registrar);
void runControllerRegistrars(Router& router, ControllerStore& controllerLifetimes);

template <typename ControllerT>
void registerControllerInstance(Router& router, ControllerStore& controllerLifetimes) {
    auto& controller = controllerLifetimes.emplace<ControllerT>();
    controller.registerRoutes(router);
}

template <typename MiddlewareT>
[[nodiscard]] void* createMiddleware() {
    return new MiddlewareT();
}

template <typename MiddlewareT>
void destroyMiddleware(void* target) noexcept {
    delete static_cast<MiddlewareT*>(target);
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
    [[nodiscard]] ::ruvia::Task<::ruvia::HttpResponse> handle( \
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
