#pragma once

#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/task.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/ip_address.h"
#include "ruvia/web/error.h"
#include "ruvia/web/web_worker.h"

namespace ruvia {

class context;

// Declared before body admission, not executed as middleware. Route and
// controller ceilings can only tighten the server limit; the strictest wins.
// Global use()/use_at() reject this route-only policy.
template <std::size_t max_bytes>
class body_limit final {
public:
    static_assert(max_bytes > 0, "body limit must be greater than 0");
    static constexpr std::size_t ruvia_request_body_limit_bytes = max_bytes;
};

struct middleware_scope_options final {
    borrowed_text prefix_{};
};

namespace detail {
struct next_access;
struct middleware_chain;

struct next_state final {
    enum class invocation_type : std::uint8_t {
        ready,
        repeated,
        expired,
    };

    struct control_type final {
        enum class phase_type : std::uint8_t {
            fresh,
            invoked,
            expired,
        };

        [[nodiscard]] invocation_type begin_invocation() noexcept {
            if (phase_ == phase_type::fresh) {
                phase_ = phase_type::invoked;
                return invocation_type::ready;
            }
            return phase_ == phase_type::invoked ? invocation_type::repeated : invocation_type::expired;
        }

        void expire() noexcept {
            phase_ = phase_type::expired;
        }

    private:
        phase_type phase_{phase_type::fresh};
    };

    // The chain being walked, the request it runs for, this hop's single-shot
    // control block, and the frame index the continuation resumes at.
    const middleware_chain* chain_{nullptr};
    context* context_{nullptr};
    control_type* control_{nullptr};
    std::size_t index_{0};
    invocation_type invocation_{invocation_type::ready};
};

using next_invoke_type = task<void> (*)(next_state);
}  // namespace detail

class next final {
public:
    class awaitable final {
        class awaiter final {
        public:
            awaiter(const awaiter&) = delete;
            awaiter& operator=(const awaiter&) = delete;
            awaiter(awaiter&&) = delete;
            awaiter& operator=(awaiter&&) = delete;

            [[nodiscard]] bool await_ready() const noexcept {
                return awaiter_.await_ready();
            }

            [[nodiscard]] std::coroutine_handle<> await_suspend(
                std::coroutine_handle<> continuation) noexcept {
                return awaiter_.await_suspend(continuation);
            }

            void await_resume() {
                awaiter_.await_resume();
            }

            const awaiter* operator&() const = delete;
            awaiter* operator&() = delete;

        private:
            friend class awaitable;

            explicit awaiter(task<void>&& task_value)
                : awaiter_(std::move(task_value).operator co_await()) {}

            decltype(std::declval<task<void>>().operator co_await()) awaiter_;
        };

    public:
        awaitable(const awaitable&) = delete;
        awaitable& operator=(const awaitable&) = delete;
        awaitable(awaitable&&) = delete;
        awaitable& operator=(awaitable&&) = delete;

        [[nodiscard]] awaiter operator co_await() && {
            auto state_value = state_;
            if (phase_ == phase_type::awaited) {
                state_value.invocation_ = detail::next_state::invocation_type::repeated;
            }
            phase_ = phase_type::awaited;
            return awaiter(invoke_(state_value));
        }
        [[nodiscard]] auto operator co_await() & = delete;
        [[nodiscard]] auto operator co_await() const& = delete;
        [[nodiscard]] auto operator co_await() const&& = delete;
        const awaitable* operator&() const = delete;
        awaitable* operator&() = delete;

    private:
        friend class next;

        constexpr awaitable(detail::next_state state_value, detail::next_invoke_type invoke) noexcept
            : state_(state_value),
              invoke_(invoke) {}

        detail::next_state state_;
        detail::next_invoke_type invoke_{nullptr};
        enum class phase_type : std::uint8_t {
            fresh,
            awaited,
        };
        phase_type phase_{phase_type::fresh};
    };

    next(const next&) = delete;
    next& operator=(const next&) = delete;
    next(next&&) = delete;
    next& operator=(next&&) = delete;

    [[nodiscard]] awaitable operator()() &;
    [[nodiscard]] awaitable operator()() const& = delete;
    [[nodiscard]] awaitable operator()() && = delete;
    [[nodiscard]] awaitable operator()() const&& = delete;
    const next* operator&() const = delete;
    next* operator&() = delete;

private:
    friend struct detail::next_access;

    constexpr next(detail::next_state state_value, detail::next_invoke_type invoke) noexcept
        : state_(state_value),
          invoke_(invoke) {}

    detail::next_state state_;
    detail::next_invoke_type invoke_{nullptr};
};

// A middleware is any final class with a public
//     task<void> invoke(context&, next&) [const]
// member. It derives from nothing: the framework composes it through per-type
// entry points and reads its route properties from static members. A
// middleware that produces the response calls context::respond() before it
// returns, so the chain has exactly one response path.
//
// application::use<T>() instantiates the descriptor at the call site. Built-in
// middleware headers already include that machinery. A custom middleware's
// use<T>() call site needs router.h, controller.h, or testing.h.
// This header stays free of context so app.h does not pull it in.
template <typename middleware_type>
concept middleware = std::is_class_v<middleware_type> && std::is_final_v<middleware_type> &&
                     (requires { static_cast<task<void> (middleware_type::*)(context&, next&)>(&middleware_type::invoke); } || requires { static_cast<task<void> (middleware_type::*)(context&, next&) const>(&middleware_type::invoke); });
template <typename policy_type>
concept route_policy =
    std::is_class_v<policy_type> && std::is_final_v<policy_type> && !middleware<policy_type> &&
    (requires {
        { policy_type::ruvia_request_body_limit_bytes } -> std::convertible_to<std::size_t>; } || requires {
            { policy_type::ruvia_stream_request_body } -> std::convertible_to<bool>; });

template <typename modifier_type>
concept route_modifier = middleware<modifier_type> || route_policy<modifier_type>;

}  // namespace ruvia

// Internal startup-time middleware descriptor.

namespace ruvia {

class context;

namespace detail {

class controller_middleware_descriptor;

template <middleware middleware_type, typename args_type>
[[nodiscard]] controller_middleware_descriptor describe_middleware(const void* args) noexcept;

template <route_policy policy_type>
[[nodiscard]] constexpr controller_middleware_descriptor describe_route_policy() noexcept;

class controller_middleware_descriptor final {
public:
    using invoke_type = route_middleware::invoke_type;
    // `args` points at the argument tuple captured by use<T>(args...), or is
    // null when the middleware is default constructed. The tuple belongs to the
    // owned_middleware_descriptor the app configuration keeps, which outlives
    // every plan compiled from it and every instance a worker's route table
    // materializes from this descriptor.
    using create_type = void* (*)(const void* args);
    using destroy_type = void (*)(void*) noexcept;

    [[nodiscard]] invoke_type invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] create_type create() const noexcept {
        return create_;
    }

    [[nodiscard]] const void* args() const noexcept {
        return args_;
    }

    [[nodiscard]] destroy_type destroy() const noexcept {
        return destroy_;
    }

    [[nodiscard]] const void* validated_model_type_key() const noexcept {
        return validated_model_type_key_;
    }

    // Empty means app-wide. Otherwise the middleware runs exactly on requests
    // whose path is under this prefix. The compiled route plan decides that per
    // route and only checks the request path at dispatch for a route serving
    // paths on both sides of the scope. The text is registration-owned and
    // outlives the plan.
    [[nodiscard]] std::string_view prefix() const noexcept {
        return prefix_;
    }

    [[nodiscard]] controller_middleware_descriptor scoped_to(std::string_view prefix) const noexcept {
        auto scoped = *this;
        scoped.prefix_ = prefix;
        return scoped;
    }

    // See middleware_runs_on_unmatched_requests(): true means this middleware also
    // wraps the 404/405/501 terminal, not just matched routes.
    [[nodiscard]] bool runs_on_unmatched_requests() const noexcept {
        return runs_on_unmatched_requests_;
    }

    // 0 when this middleware declares no ceiling.
    [[nodiscard]] std::size_t request_body_limit() const noexcept {
        return request_body_limit_;
    }

    [[nodiscard]] bool uses_route_rate_limit() const noexcept {
        return uses_route_rate_limit_;
    }

    [[nodiscard]] bool stream_request_body() const noexcept {
        return stream_request_body_;
    }

private:
    template <middleware middleware_type, typename args_type>
    friend controller_middleware_descriptor describe_middleware(const void* args) noexcept;
    template <route_policy policy_type>
    friend constexpr controller_middleware_descriptor describe_route_policy() noexcept;

    constexpr controller_middleware_descriptor(invoke_type invoke, create_type create, destroy_type destroy,
        const void* args, const void* validated_model_type_key, bool uses_route_rate_limit,
        bool runs_on_unmatched_requests, std::size_t request_body_limit,
        bool stream_request_body = false) noexcept
        : invoke_(invoke),
          create_(create),
          destroy_(destroy),
          args_(args),
          validated_model_type_key_(validated_model_type_key),
          uses_route_rate_limit_(uses_route_rate_limit),
          request_body_limit_(request_body_limit),
          runs_on_unmatched_requests_(runs_on_unmatched_requests),
          stream_request_body_(stream_request_body) {}
    invoke_type invoke_{nullptr};
    create_type create_{nullptr};
    destroy_type destroy_{nullptr};
    const void* args_{nullptr};
    const void* validated_model_type_key_{nullptr};
    std::string_view prefix_{};
    bool uses_route_rate_limit_{false};
    std::size_t request_body_limit_{0};
    bool runs_on_unmatched_requests_{false};
    bool stream_request_body_{false};
};

// An app-level use<T>(args...) registration: the descriptor plus ownership of
// the argument tuple it points at. The app configuration holds it for its own
// lifetime, which spans every route plan compiled from it; descriptors copied
// into plans and route tables only borrow the tuple. A registration without
// arguments owns nothing.
class owned_middleware_descriptor final {
public:
    using release_args_type = void (*)(const void* args) noexcept;

    owned_middleware_descriptor(controller_middleware_descriptor descriptor, release_args_type release_args) noexcept
        : descriptor_(descriptor),
          release_args_(release_args) {}

    owned_middleware_descriptor(const owned_middleware_descriptor&) = delete;
    owned_middleware_descriptor& operator=(const owned_middleware_descriptor&) = delete;

    owned_middleware_descriptor(owned_middleware_descriptor&& other) noexcept
        : prefix_(std::move(other.prefix_)),
          descriptor_(other.descriptor_.scoped_to(prefix_)),
          release_args_(std::exchange(other.release_args_, nullptr)) {}
    owned_middleware_descriptor& operator=(owned_middleware_descriptor&&) = delete;

    ~owned_middleware_descriptor() {
        if (auto* const release = std::exchange(release_args_, nullptr)) {
            release(descriptor_.args());
        }
    }

    [[nodiscard]] const controller_middleware_descriptor& descriptor() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] owned_middleware_descriptor scoped_to(std::string_view prefix) && {
        prefix_.assign(prefix);
        descriptor_ = descriptor_.scoped_to(prefix_);
        return std::move(*this);
    }

private:
    std::string prefix_{};
    controller_middleware_descriptor descriptor_;
    release_args_type release_args_{nullptr};
};

}  // namespace detail

}  // namespace ruvia

namespace ruvia::detail {

template <middleware middleware_type>
[[nodiscard]] task<void> invoke_middleware(void* target, context& context_value, next& next_value) {
    if constexpr (requires {
                      static_cast<task<void> (middleware_type::*)(context&, next&)>(&middleware_type::invoke);
                  }) {
        constexpr auto method =
            static_cast<task<void> (middleware_type::*)(context&, next&)>(&middleware_type::invoke);
        return (static_cast<middleware_type*>(target)->*method)(context_value, next_value);
    } else {
        constexpr auto method =
            static_cast<task<void> (middleware_type::*)(context&, next&) const>(&middleware_type::invoke);
        return (static_cast<const middleware_type*>(target)->*method)(context_value, next_value);
    }
}

// Constructs the middleware from the arguments use<T>(args...) captured. The
// tuple is owned by the app configuration's owned_middleware_descriptor, so it
// is still readable every time a worker's route table materializes an instance
// -- including across a stop()/run() cycle, which rebuilds instances from the
// same descriptors. Registering without arguments stores no tuple at all: the
// descriptor's args are null and the default constructor runs.
template <typename middleware_type, typename args_type>
[[nodiscard]] void* create_middleware(const void* args) {
    if constexpr (std::tuple_size_v<args_type> == 0) {
        return static_cast<void*>(new middleware_type());
    } else {
        return std::apply(
            [](const auto&... values) {
                return static_cast<void*>(new middleware_type(values...));
            },
            *static_cast<const args_type*>(args));
    }
}

template <typename middleware_type>
void destroy_middleware(void* target) noexcept {
    delete static_cast<middleware_type*>(target);
}

template <typename args_type>
void release_middleware_args(const void* args) noexcept {
    delete static_cast<const args_type*>(args);
}

template <typename middleware_type>
[[nodiscard]] const void* middleware_validated_model_type_key() noexcept {
    if constexpr (requires { typename middleware_type::ruvia_validation_body_type; }) {
        return type_key<typename middleware_type::ruvia_validation_body_type>();
    } else {
        return nullptr;
    }
}

template <typename middleware_type>
[[nodiscard]] constexpr bool middleware_uses_route_rate_limit() noexcept {
    if constexpr (requires { middleware_type::ruvia_uses_route_rate_limit; }) {
        return middleware_type::ruvia_uses_route_rate_limit;
    } else {
        return false;
    }
}

// Whether this middleware is meaningful on a request that matched no route.
// A property of the middleware, not of where it is registered: security headers
// and request ids belong on a 404 response just as much as on a 200, while a
// validator or an authorization check has nothing to act on. Declared as
//     static constexpr bool ruvia_runs_on_unmatched_requests = true;
template <typename middleware_type>
[[nodiscard]] constexpr bool middleware_runs_on_unmatched_requests() noexcept {
    if constexpr (requires { middleware_type::ruvia_runs_on_unmatched_requests; }) {
        return middleware_type::ruvia_runs_on_unmatched_requests;
    } else {
        return false;
    }
}

// A request-body ceiling this middleware declares for the routes it is on, or 0
// for none. Read by the server BEFORE the body is accepted, which is why it
// cannot simply be code inside invoke(): by the time a middleware runs, the
// bytes it would have rejected are already buffered.
template <typename middleware_type>
[[nodiscard]] constexpr std::size_t middleware_request_body_limit() noexcept {
    if constexpr (requires { middleware_type::ruvia_request_body_limit_bytes; }) {
        return middleware_type::ruvia_request_body_limit_bytes;
    } else {
        return 0;
    }
}

template <typename middleware_type>
[[nodiscard]] constexpr bool middleware_stream_request_body() noexcept {
    if constexpr (requires { middleware_type::ruvia_stream_request_body; }) {
        return middleware_type::ruvia_stream_request_body;
    } else {
        return false;
    }
}

// The one description of a middleware type: its entry points and declared
// route properties, bound to the argument tuple (null when there is none).
template <middleware middleware_type, typename args_type>
[[nodiscard]] controller_middleware_descriptor describe_middleware(const void* args) noexcept {
    return controller_middleware_descriptor(&invoke_middleware<middleware_type>,
        &create_middleware<middleware_type, args_type>, &destroy_middleware<middleware_type>, args,
        middleware_validated_model_type_key<middleware_type>(), middleware_uses_route_rate_limit<middleware_type>(),
        middleware_runs_on_unmatched_requests<middleware_type>(), middleware_request_body_limit<middleware_type>(),
        middleware_stream_request_body<middleware_type>());
}

// A default-constructed middleware, as controller and route middleware lists
// declare it. It captures nothing, so it allocates nothing and owns nothing:
// route registration reruns on every route plan compile.
template <middleware middleware_type>
[[nodiscard]] controller_middleware_descriptor make_middleware_descriptor() noexcept {
    static_assert(std::is_default_constructible_v<middleware_type>,
        "middleware registered without arguments must be default constructible");
    return describe_middleware<middleware_type, std::tuple<>>(nullptr);
}

template <route_policy policy_type>
[[nodiscard]] constexpr controller_middleware_descriptor describe_route_policy() noexcept {
    return controller_middleware_descriptor(nullptr, nullptr, nullptr, nullptr, nullptr, false, false,
        middleware_request_body_limit<policy_type>(), middleware_stream_request_body<policy_type>());
}

template <route_modifier modifier_type>
[[nodiscard]] controller_middleware_descriptor make_route_modifier_descriptor() noexcept {
    if constexpr (route_policy<modifier_type>) {
        return describe_route_policy<modifier_type>();
    } else {
        return make_middleware_descriptor<modifier_type>();
    }
}

// Registers one app-level middleware type together with the arguments every
// instance of it is constructed from. Arguments are decayed and copied once,
// at registration; a middleware is built per worker route table, so they must
// stay readable for the app configuration's lifetime rather than the caller's
// scope. The returned owner destroys them. Registering without arguments
// stores no tuple.
template <middleware middleware_type, typename... args_types>
[[nodiscard]] owned_middleware_descriptor make_owned_middleware_descriptor(args_types&&... args) {
    static_assert(std::is_constructible_v<middleware_type, const std::decay_t<args_types>&...>,
        "middleware is not constructible from the arguments passed to use<T>(); a middleware "
        "registered without arguments must be default constructible");

    if constexpr (sizeof...(args_types) == 0) {
        return owned_middleware_descriptor(make_middleware_descriptor<middleware_type>(), nullptr);
    } else {
        using args_tuple_type = std::tuple<std::decay_t<args_types>...>;
        const auto* stored = new args_tuple_type(std::forward<args_types>(args)...);
        return owned_middleware_descriptor(
            describe_middleware<middleware_type, args_tuple_type>(stored), &release_middleware_args<args_tuple_type>);
    }
}

}  // namespace ruvia::detail

namespace ruvia {

class context;
struct csrf_protection_config final {
    std::string cookie_name_{"XSRF-TOKEN"};
    // 必须为脚本主动提交 token 的自定义字段；构造时拒绝 CORS-safelisted、
    // 浏览器保留及已知自动生成/代理字段（名称不区分大小写）。
    // 部署方还须保证代理不注入所选字段，且仅向可信 origin 授予携带凭据的
    // CORS 权限。不能用 Accept、Origin、Host 等环境信息代替主动提交的 token。
    std::string header_name_{"X-XSRF-TOKEN"};
};

// Stateless CSRF protection using the double-submit-cookie pattern (no
// server-side session store needed, so it works across SO_REUSEPORT workers).
// A safe request (GET/HEAD/OPTIONS) without a token cookie is issued a fresh
// one (readable by JavaScript so a SPA can echo it). An unsafe request must
// repeat that cookie's value in the header; a missing or mismatched token is
// rejected with 403. Register on a controller, group, or route that should
// enforce browser XSRF checks. Cookie and header names default to "XSRF-TOKEN"
// and "X-XSRF-TOKEN" and can be rebranded per app.
// This relies on the browser's custom-request-header boundary. It does not
// authenticate a user or protect an origin already trusted by credentialed CORS.
class csrf_protection final {
public:
    csrf_protection();
    explicit csrf_protection(const csrf_protection_config& config);

    csrf_protection(const csrf_protection&) = delete;
    csrf_protection& operator=(const csrf_protection&) = delete;
    csrf_protection(csrf_protection&&) = delete;
    csrf_protection& operator=(csrf_protection&&) = delete;

    task<void> invoke(context& c, next& next);

private:
    [[nodiscard]] static const csrf_protection_config& validate(const csrf_protection_config& source);

    std::string cookie_name_;
    std::string header_name_;
};

}  // namespace ruvia

namespace ruvia {

class context;
struct security_header final {
    std::string name_{};
    std::string value_{};
};
// Legacy browser XSS filters can introduce vulnerabilities in otherwise safe
// pages. Modern policy either emits X-XSS-Protection: 0 to disable the obsolete
// auditor, or omits the obsolete header entirely for clients that ignore it.
enum class xss_protection_header_policy : std::uint8_t {
    emit_disabled,
    omit,
};

enum class default_security_header_policy : std::uint8_t {
    omit,
    emit_default,
};

enum class security_header_conflict_policy : std::uint8_t {
    preserve_existing,
    replace_existing,
};

struct security_headers_config final {
    default_security_header_policy content_type_options_header_ =
        default_security_header_policy::emit_default;
    default_security_header_policy frame_options_header_ = default_security_header_policy::emit_default;
    // Emitted only for requests received over TLS. Plain HTTP responses must
    // never carry Strict-Transport-Security.
    default_security_header_policy strict_transport_security_header_ =
        default_security_header_policy::emit_default;
    xss_protection_header_policy xss_protection_header_ = xss_protection_header_policy::emit_disabled;

    std::string content_security_policy_{"default-src 'self'"};
    std::string referrer_policy_{"strict-origin-when-cross-origin"};
    std::string permissions_policy_{"geolocation=(), microphone=(), camera=()"};

    std::vector<security_header> custom_headers_{};
    security_header_conflict_policy existing_headers_ = security_header_conflict_policy::preserve_existing;
};

// Registered app-wide with the defaults as `app().use<security_headers_middleware>()`,
// or with an owning policy as `app().use<security_headers_middleware>(config)`.
class security_headers_middleware final {
public:
    // A 404 is a response to an attacker-reachable URL like any other, so it
    // needs the same CSP, frame and referrer policy a matched route gets. CORS
    // follows the same response-layer rule for unmatched requests.
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    security_headers_middleware();
    explicit security_headers_middleware(const security_headers_config& config);

    security_headers_middleware(const security_headers_middleware&) = delete;
    security_headers_middleware& operator=(const security_headers_middleware&) = delete;
    security_headers_middleware(security_headers_middleware&&) = delete;
    security_headers_middleware& operator=(security_headers_middleware&&) = delete;

    task<void> invoke(context& context, next& next);

private:
    bool emit_content_type_options_{true};
    bool emit_frame_options_{true};
    bool emit_strict_transport_security_{true};
    bool emit_disabled_xss_protection_{true};
    bool replace_existing_{false};
    std::string content_security_policy_;
    std::string referrer_policy_;
    std::string permissions_policy_;
    std::vector<http_header> custom_headers_;
};

}  // namespace ruvia

namespace ruvia {

class context;

// Buffer capacity required by format_rate_limit_address_key ("v6s/" + 32 hex
// digits + '/' + up to 10 scope digits).
inline constexpr std::size_t rate_limit_key_buffer_bytes = 48;
// Each worker preallocates this many sliding-window key entries when either
// the app-wide default rule or a route-specific rate-limit middleware is
// present. A key probes a bounded run of entries from its hash position; a
// key whose run holds only live entries of other keys takes the rule's
// overflow policy even when other parts of the table are free. An entry stays
// live until two of its key's windows have passed without a request.
inline constexpr std::size_t default_rate_limit_capacity_per_worker = 8192;

enum class rate_limit_overflow_policy : std::uint8_t {
    deny,
    allow,
};

// Per-worker, per-address sliding-window rule. Configuration remains a plain
// value; each owner validates it once before publishing runtime state.
// Each key's windows start at its first request and follow one another. A
// request is admitted while
//     previous * (unelapsed fraction of the current window) + current
// stays below max_requests_, where current and previous count the requests
// admitted in the current and the preceding window (a gap of two or more
// windows leaves previous at zero). The previous window's count fades out
// instead of vanishing at the boundary, so a client cannot spend one budget
// at the end of a window and a second right after it. Refused requests are
// not counted. A refusal's Retry-After is the earliest whole second at which
// the estimate admits again; X-RateLimit-Reset is when it has fallen to zero.
// Both app-wide and route rules key on the resolved client address: unscoped
// IPv6 shares a /64 budget, IPv4-mapped IPv6 shares its dotted IPv4 host budget,
// and scoped IPv6 keys on its full address plus numeric scope. Equivalent
// accepted IPv6 spellings do not create additional budgets.
struct rate_limit_rule final {
    std::size_t max_requests_{0};
    std::chrono::milliseconds window_{0};
    rate_limit_overflow_policy overflow_policy_{rate_limit_overflow_policy::deny};
};

struct rate_limit_config final {
    rate_limit_rule rule_{};
    std::size_t capacity_per_worker_{default_rate_limit_capacity_per_worker};
};

// Normalizes a client IP address into its per-client rate-limit key:
// - IPv4 and IPv4-mapped IPv6 share the canonical dotted IPv4 host key;
// - Unscoped IPv6 is grouped by its /64 network prefix ("v6/<16 hex>");
// - Scoped IPv6 keys on its full 128-bit address and numeric scope ("v6s/<32 hex>/<scope>").
[[nodiscard]] std::string_view format_rate_limit_address_key(
    const ip_address& address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept;
[[nodiscard]] std::string_view format_rate_limit_address_key(
    std::string_view address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept;

struct rate_limit_decision final {
    bool allowed_{true};
    std::size_t limit_{0};
    std::size_t remaining_{0};
    std::uint64_t retry_after_ms_{0};
    std::uint64_t reset_after_ms_{0};

    [[nodiscard]] constexpr bool allowed() const noexcept {
        return allowed_;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return allowed_;
    }
    [[nodiscard]] constexpr std::size_t limit() const noexcept {
        return limit_;
    }
    [[nodiscard]] constexpr std::size_t remaining() const noexcept {
        return remaining_;
    }
    [[nodiscard]] constexpr std::uint64_t retry_after_ms() const noexcept {
        return retry_after_ms_;
    }
    [[nodiscard]] constexpr std::uint64_t reset_after_ms() const noexcept {
        return reset_after_ms_;
    }
    [[nodiscard]] constexpr std::uint64_t retry_after_seconds() const noexcept {
        return retry_after_ms_ / 1000U + (retry_after_ms_ % 1000U == 0U ? 0U : 1U);
    }
    [[nodiscard]] constexpr std::uint64_t reset_after_seconds() const noexcept {
        return reset_after_ms_ / 1000U + (reset_after_ms_ % 1000U == 0U ? 0U : 1U);
    }

    friend constexpr bool operator==(const rate_limit_decision&, const rate_limit_decision&) noexcept = default;
};

void apply_rate_limit_headers(http_response& response, const rate_limit_decision& decision);

// Single-worker SipHash-1-3 sliding-window rate limiter table. Preallocates a
// power-of-two slot array at construction and performs zero allocations or
// locks on the decision path.
class sliding_window_rate_limiter final {
public:
    explicit sliding_window_rate_limiter(const rate_limit_config& config);
    explicit sliding_window_rate_limiter(
        std::size_t capacity_per_worker = default_rate_limit_capacity_per_worker);
    sliding_window_rate_limiter(const sliding_window_rate_limiter&) = delete;
    sliding_window_rate_limiter& operator=(const sliding_window_rate_limiter&) = delete;
    sliding_window_rate_limiter(sliding_window_rate_limiter&&) noexcept;
    sliding_window_rate_limiter& operator=(sliding_window_rate_limiter&&) noexcept;
    ~sliding_window_rate_limiter();

    [[nodiscard]] bool has_default_rule() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

    [[nodiscard]] rate_limit_decision decide(std::string_view key,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;
    [[nodiscard]] rate_limit_decision decide(const ip_address& address,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;

    [[nodiscard]] rate_limit_decision decide(std::string_view key, const rate_limit_rule& rule,
        std::uintptr_t scope = 1,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;
    [[nodiscard]] rate_limit_decision decide(const ip_address& address, const rate_limit_rule& rule,
        std::uintptr_t scope = 1,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;

private:
    struct impl_type;
    std::unique_ptr<impl_type> impl_;
};
namespace detail {
// `declaration` identifies the rule's declaration: stacked rules on one route
// each count in their own window. Throws when the rule refuses the request.
void enforce_route_rate_limit(context& context_value, const void* declaration, const rate_limit_rule& rule);

}  // namespace detail

// A per-route rate limit, configured through the type:
//
//     RUVIA_GET("/ready", ready, ruvia::rate_limit<10, 1000>);
//
// Route and controller middleware lists name types, so a middleware registered
// there is default constructed and cannot take constructor arguments. Carrying
// the configuration as template parameters is how such a middleware is
// configured -- the values are constexpr, the type stays default constructible,
// and the chain is still finalized at startup with nothing allocated per
// request. A comma inside the template argument list is fine: the route macro
// pastes its arguments back together.
//
// The limit is scoped to the route it is registered on and keyed on the client
// address, so two routes carrying the same numbers count independently. Each
// declaration also counts on its own: an app or controller rule stacked over a
// route rule enforces both windows, never one shared counter. It does not
// replace application::rate_limit() -- both apply, so the stricter is what a
// caller meets, the same "narrower scope may only tighten" rule body_limit
// follows. Worker-local, like the app-wide rule, and counted with the same
// sliding-window estimate (see rate_limit_rule). A refused request is answered
// like one the app-wide rule refuses: 429 through the application error
// handler, with Retry-After and X-RateLimit-* fields.
template <std::size_t max_requests, std::int64_t window_ms,
    rate_limit_overflow_policy overflow_policy = rate_limit_overflow_policy::deny>
class rate_limit final {
public:
    static_assert(max_requests > 0, "route rate limit max requests must be greater than 0");
    static_assert(window_ms > 0, "route rate limit window must be greater than 0ms");

    static constexpr bool ruvia_uses_route_rate_limit = true;

    task<void> invoke(context& context_value, next& next_value) {
        detail::enforce_route_rate_limit(context_value, this, rule_);
        co_await next_value();
    }

private:
    static constexpr rate_limit_rule rule_{
        .max_requests_ = max_requests,
        .window_ = std::chrono::milliseconds(window_ms),
        .overflow_policy_ = overflow_policy,
    };
};

}  // namespace ruvia

namespace ruvia::detail {

class context_session_state;
struct session_access;

}  // namespace ruvia::detail

namespace ruvia {

class context;
class next;

// A request-local capability bound by session_middleware. The handle borrows
// context state and therefore cannot escape the request.
// Changes are persisted automatically before the response head (including a
// stream head or a websocket/CONNECT handshake) is sent. Once that commit
// begins, set(), clear(), and regenerate() throw std::logic_error; data()
// remains readable.
class session final {
public:
    [[nodiscard]] std::string_view data() const& noexcept;
    std::string_view data() const&& = delete;
    [[nodiscard]] bool empty() const noexcept {
        return data().empty();
    }
    // Every non-empty write replaces an existing session identity at commit.
    // Repeated writes in one request publish one new ID; reads keep the ID.
    void set(std::string_view data);
    void clear();
    // Rotate without changing the data, for an authentication or privilege
    // transition that does not write the session. set() already requests rotation.
    // Replacing an expired or revoked session fails with HTTP 409.
    void regenerate();

private:
    explicit session(detail::context_session_state& state_value) noexcept
        : state_(&state_value) {}

    detail::context_session_state* state_;
    friend class context;
};

}  // namespace ruvia

#ifdef RUVIA_ENABLE_REDIS

namespace ruvia {

// Server-side session backed by Redis (RUVIA_ENABLE_REDIS). Reads the configured
// cookie only when its ID is exactly 48 lowercase hexadecimal characters, the
// fixed length minted by this middleware; other IDs are treated as absent.
// Loads the blob at key_prefix_ + id (by default sess:<id>) into the context.
// Before the response head is sent, persists changes with the configured TTL
// or deletes the session. A new session mints a random ID in an HttpOnly cookie.
// The blob format is the application's; pair it with JSON if desired.
struct session_config final {
    std::string redis_alias_{"default"};
    std::string cookie_name_{"sid"};
    std::string key_prefix_{"sess:"};
    // Must be positive and representable in std::chrono::milliseconds;
    // otherwise construction throws std::invalid_argument.
    std::chrono::seconds ttl_{std::chrono::hours(24)};
};

class session_middleware final {
public:
    session_middleware();
    explicit session_middleware(const session_config& config);

    session_middleware(const session_middleware&) = delete;
    session_middleware& operator=(const session_middleware&) = delete;
    session_middleware(session_middleware&&) = delete;
    session_middleware& operator=(session_middleware&&) = delete;

    task<void> invoke(context& c, next& next);

private:
    friend struct detail::session_access;
    task<void> commit(context& c) const;

    [[nodiscard]] static const session_config& validate(const session_config& source);

    std::string redis_alias_;
    std::string cookie_name_;
    std::string key_prefix_;
    std::chrono::seconds ttl_;
};

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
