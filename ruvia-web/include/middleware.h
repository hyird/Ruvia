#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "blocking_pool.h"
#include "borrowed_text.h"
#include "http_header.h"
#include "http_response.h"
#include "http_status.h"
#include "stop_token.h"
#include "task.h"
#include "task_scope.h"
#include "worker_ref.h"

namespace ruvia {

enum class integer_parse_error {
    invalid_format,
    out_of_range,
};

namespace detail {
template <typename t_type>
concept parsed_integer = std::integral<t_type> && !std::same_as<std::remove_cv_t<t_type>, bool> && !std::same_as<std::remove_cv_t<t_type>, char> && !std::same_as<std::remove_cv_t<t_type>, wchar_t> && !std::same_as<std::remove_cv_t<t_type>, char8_t> && !std::same_as<std::remove_cv_t<t_type>, char16_t> && !std::same_as<std::remove_cv_t<t_type>, char32_t> && requires(const char* first, const char* last, t_type& value) { std::from_chars(first, last, value, 10); };
}  // namespace detail

// Strict decimal input: no whitespace, leading '+', or trailing characters.
// A leading '-' is accepted only for signed integers. Format errors take
// precedence over overflow when the input also contains trailing characters.
template <detail::parsed_integer t_type>
[[nodiscard]] constexpr std::variant<t_type, integer_parse_error> parse_integer(std::string_view text) noexcept {
    if (text.empty()) {
        return integer_parse_error::invalid_format;
    }
    t_type value{};
    const auto* end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, value, 10);
    if (error == std::errc::invalid_argument || ptr != end) {
        return integer_parse_error::invalid_format;
    }
    if (error == std::errc::result_out_of_range) {
        return integer_parse_error::out_of_range;
    }
    return value;
}

template <std::size_t n>
struct fixed_string {
    char value_[n]{};

    constexpr fixed_string(const char (&text)[n]) noexcept {
        for (std::size_t i = 0; i < n; ++i) {
            value_[i] = text[i];
        }
    }

    [[nodiscard]] constexpr std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::string_view(value_, n - 1);
    }
    [[nodiscard]] constexpr std::string_view view() const&& = delete;
    [[nodiscard]] constexpr const char* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_;
    }
    [[nodiscard]] constexpr const char* data() const&& = delete;
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return n - 1;
    }
    [[nodiscard]] constexpr bool empty() const noexcept {
        return n == 1;
    }
    constexpr operator std::string_view() const& noexcept RUVIA_LIFETIMEBOUND {
        return view();
    }
    constexpr operator std::string_view() const&& = delete;
};

template <std::size_t n>
fixed_string(const char (&)[n]) -> fixed_string<n>;

template <std::size_t left_n, std::size_t right_n>
[[nodiscard]] constexpr bool operator==(const fixed_string<left_n>& left, const fixed_string<right_n>& right) noexcept {
    if constexpr (left_n != right_n) {
        return false;
    } else {
        return std::ranges::equal(left.value_, right.value_);
    }
}

namespace detail {

// Compile-time lookup shared by every schema keyed by fixed_string names
// (model fields, entity columns, database relations).

// Position of `name` in `names`, or names.size() when it is absent.
[[nodiscard]] consteval std::size_t fixed_name_index(std::span<const std::string_view> names, std::string_view name) noexcept {
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == name) {
            return index;
        }
    }
    return names.size();
}

// Whether no two entries of `names` are equal under `equal`.
template <typename equal_type = std::equal_to<>>
[[nodiscard]] consteval bool fixed_names_distinct(std::span<const std::string_view> names, equal_type equal = {}) noexcept {
    for (std::size_t left = 0; left < names.size(); ++left) {
        for (std::size_t right = left + 1; right < names.size(); ++right) {
            if (equal(names[left], names[right])) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace detail

}  // namespace ruvia

namespace ruvia::detail {

// Trivial request/runtime view. Never allocates or extends the callable's
// lifetime. Signature policy changes invocation, not storage or binding logic.
template <typename result_type, bool is_noexcept, typename... args_types>
class callback_ref_view final {
public:
    constexpr callback_ref_view() noexcept = default;
    constexpr callback_ref_view(std::nullptr_t) noexcept {}
    // Select concrete pointer types so MSVC resolves the exception specification.
    using invoke_type = std::conditional_t<is_noexcept, result_type (*)(void*, args_types...) noexcept, result_type (*)(void*, args_types...)>;
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] result_type operator()(args_types... args) const noexcept(is_noexcept) {
        return invoke_target(std::forward<args_types>(args)...);
    }

    [[nodiscard]] friend constexpr bool operator==(callback_ref_view, callback_ref_view) noexcept = default;

private:
    friend struct callback_access;

    constexpr callback_ref_view(void* target, invoke_type invoke) noexcept :
        target_(target),
        invoke_(invoke) {}

    template <typename callable_type>
        requires(std::is_invocable_r_v<result_type, callable_type&, args_types...> && (!is_noexcept || std::is_nothrow_invocable_r_v<result_type, callable_type&, args_types...>))
    [[nodiscard]] static constexpr callback_ref_view bind(callable_type& callable) noexcept {
        return callback_ref_view(const_cast<void*>(static_cast<const void*>(std::addressof(callable))), [](void* target, args_types... args) noexcept(is_noexcept) -> result_type {
            return (*static_cast<callable_type*>(target))(std::forward<args_types>(args)...);
        });
    }

    template <typename... invocation_args>
    [[nodiscard]] result_type invoke_target(invocation_args&&... args) const noexcept(is_noexcept) {
        return invoke_(target_, std::forward<invocation_args>(args)...);
    }

    void* target_{nullptr};
    invoke_type invoke_{nullptr};
};

// Signature decomposition selects the borrowed view's noexcept policy.
template <typename signature_type>
struct callback_signature;

template <typename result_type, typename... args_types>
struct callback_signature<result_type(args_types...)> final {
    template <template <typename, bool, typename...> typename family_type>
    using apply = family_type<result_type, false, args_types...>;
};

template <typename result_type, typename... args_types>
struct callback_signature<result_type(args_types...) noexcept> final {
    template <template <typename, bool, typename...> typename family_type>
    using apply = family_type<result_type, true, args_types...>;
};

template <typename signature_type>
using callback_ref = typename callback_signature<signature_type>::template apply<callback_ref_view>;

struct callback_access final {
    template <typename signature_type>
    [[nodiscard]] static constexpr callback_ref<signature_type> make(void* target, typename callback_ref<signature_type>::invoke_type invoke) noexcept {
        return callback_ref<signature_type>(target, invoke);
    }

    template <typename signature_type>
    [[nodiscard]] static callback_ref<signature_type> ref(const move_only_function<signature_type>& owner_value) noexcept {
        return owner_value ? callback_ref<signature_type>::bind(owner_value) : callback_ref<signature_type>{};
    }

    template <typename callback_type, typename deleter_type>
    [[nodiscard]] static auto ref(const std::unique_ptr<callback_type, deleter_type>& owner_value) noexcept {
        using ref_type = decltype(ref(*owner_value));
        return owner_value ? ref(*owner_value) : ref_type{};
    }

    template <typename signature_type, typename callable_type>
    [[nodiscard]] static constexpr callback_ref<signature_type> bind(callable_type& callable) noexcept {
        return callback_ref<signature_type>::bind(callable);
    }
    template <typename signature_type, typename callable_type>
        requires(!std::is_lvalue_reference_v<callable_type>)
    static void bind(callable_type&&) = delete;

    template <typename ref_type>
    [[nodiscard]] static constexpr void* target(ref_type value) noexcept {
        return value.target_;
    }

    template <typename ref_type>
    [[nodiscard]] static constexpr typename ref_type::invoke_type invoke(ref_type value) noexcept {
        return value.invoke_;
    }
};

static_assert(sizeof(callback_ref<void()>) == 2 * sizeof(void*));
static_assert(std::is_trivially_copyable_v<callback_ref<void()>>);
}  // namespace ruvia::detail

// The three non-owning callable shapes a route runs: a buffered handler, a
// streaming (response-stream / websocket / tunnel) handler, and one middleware
// frame. Each binds a worker-local instance to a process-invariant invoke thunk.

namespace ruvia {

class context;
class http_response;
class next;

namespace detail {

enum class request_body_mode : std::uint8_t { buffered,
    stream };

using route_handler = callback_ref<task<http_response>(context&)>;
using route_stream_handler = callback_ref<task<void>(context&)>;
using route_middleware = callback_ref<task<void>(context&, next&)>;

}  // namespace detail

class validator;

inline constexpr std::size_t max_validation_text_bytes = 1024;

namespace detail {
struct validation_issue_access;

[[nodiscard]] inline std::string_view bounded_validation_text(std::string_view value) noexcept {
    auto count = std::min(value.size(), max_validation_text_bytes);
    // Keep a UTF-8 prefix complete when the last code point crosses the bound.
    while (count < value.size() && count > 0 && (static_cast<unsigned char>(value[count]) & 0xc0U) == 0x80U) {
        --count;
    }
    return value.substr(0, count);
}
}  // namespace detail

struct validation_issue_options final {
    borrowed_text field_{};
    borrowed_text code_{};
    borrowed_text message_{};
};

class validation_issue final {
public:
    validation_issue(const validation_issue&) = default;
    validation_issue(validation_issue&&) noexcept = default;
    validation_issue& operator=(const validation_issue& other);
    validation_issue& operator=(validation_issue&& other) noexcept;

    [[nodiscard]] std::string_view field() const& noexcept RUVIA_LIFETIMEBOUND {
        return field_;
    }
    [[nodiscard]] std::string_view field() const&& = delete;

    [[nodiscard]] std::string_view code() const& noexcept RUVIA_LIFETIMEBOUND {
        return code_;
    }
    [[nodiscard]] std::string_view code() const&& = delete;

    [[nodiscard]] std::string_view message() const& noexcept RUVIA_LIFETIMEBOUND {
        return message_;
    }
    [[nodiscard]] std::string_view message() const&& = delete;

private:
    friend class validator;
    friend struct detail::validation_issue_access;

    void swap(validation_issue& other) noexcept;

    explicit validation_issue(validation_issue_options options) :
        field_(detail::bounded_validation_text(options.field_.view())),
        code_(detail::bounded_validation_text(options.code_.view())),
        message_(detail::bounded_validation_text(options.message_.view())) {}

    std::string field_;
    std::string code_;
    std::string message_;
};
using borrowed_validation_issues = borrowed_span<validation_issue>;

struct http_error_info_options final {
    http_status_code status_{http_status::internal_server_error};
    borrowed_text code_{};
    borrowed_text message_{};
    borrowed_text status_text_{};
    borrowed_validation_issues validation_issues_{};
};

// Non-owning Web application error metadata used by context and custom error
// handlers. Every borrowed value must outlive this view; basic_string rvalues
// and temporary owning validation-issue ranges are rejected. The JSON error
// envelope is a framework product concern, not an HTTP protocol primitive, so
// this type belongs to ruvia-web.
class http_error_info final {
public:
    constexpr explicit http_error_info(http_error_info_options options = {}) noexcept :
        status_(options.status_),
        status_text_(options.status_text_.view()),
        code_(options.code_.view()),
        message_(options.message_.view()),
        validation_issues_(options.validation_issues_.view()) {}

    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] constexpr std::string_view status_text() const noexcept {
        return status_text_;
    }

    [[nodiscard]] constexpr std::string_view code() const noexcept {
        return code_;
    }

    [[nodiscard]] constexpr std::string_view message() const noexcept {
        return message_;
    }

    [[nodiscard]] constexpr std::span<const validation_issue> validation_issues() const noexcept {
        return validation_issues_;
    }

private:
    http_status_code status_{http_status::internal_server_error};
    std::string_view status_text_{};
    std::string_view code_{};
    std::string_view message_{};
    std::span<const validation_issue> validation_issues_{};
};

// The owning Web application error: status, code, message, status text and
// validation issues are deep-copied from the options.
class http_error final : public std::exception {
public:
    explicit http_error(http_error_info_options options);
    http_error(const http_error&) = default;
    http_error& operator=(const http_error& other);
    http_error(http_error&&) noexcept = default;
    http_error& operator=(http_error&&) noexcept = default;
    [[nodiscard]] const char* what() const noexcept override;
    [[nodiscard]] http_error_info info() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] http_error_info info() const&& = delete;

    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] std::string_view status_text() const& noexcept RUVIA_LIFETIMEBOUND {
        return status_text_;
    }
    [[nodiscard]] std::string_view status_text() const&& = delete;

    [[nodiscard]] std::string_view code() const& noexcept RUVIA_LIFETIMEBOUND {
        return code_;
    }
    [[nodiscard]] std::string_view code() const&& = delete;

    [[nodiscard]] std::string_view message() const& noexcept RUVIA_LIFETIMEBOUND {
        return message_;
    }
    [[nodiscard]] std::string_view message() const&& = delete;

    [[nodiscard]] std::span<const validation_issue> validation_issues() const& noexcept RUVIA_LIFETIMEBOUND {
        return validation_issues_;
    }
    [[nodiscard]] std::span<const validation_issue> validation_issues() const&& = delete;

private:
    friend class validator;
    http_error(http_status_code status, std::string_view code, std::string_view message, std::string_view status_text, std::vector<validation_issue>&& issues);
    http_status_code status_{http_status::internal_server_error};
    std::string status_text_;
    std::string code_;
    std::string message_;
    std::vector<validation_issue> validation_issues_;
};

[[nodiscard]] std::string_view default_error_code(http_status_code status) noexcept;

// UTF-8 text describing any exception, for logging a failure such as the one
// application::run rethrows. On Windows, what() of a std::system_error in the
// system or Asio category carries the OS message in the ANSI code page; that
// message is replaced with its UTF-8 form. Any other std::exception yields
// what(), a non-standard exception a fixed description, and null an empty
// string.
[[nodiscard]] std::string exception_text(const std::exception_ptr& exception);

namespace detail {
using http_error_handler_ref = callback_ref<task<http_response>(context&, http_error_info)>;
using http_not_found_handler_ref = callback_ref<task<http_response>(context&)>;
}  // namespace detail

// Answers a request that failed. Accepts a plain function -- on_error(&handler)
// -- and equally a lambda or any other callable, including one that captures
// the logger, config, or metrics sink the handler needs. A self-contained
// callable is owned by the handler value; references captured by that callable
// must still outlive the registered handler.
// 空函数指针构造空处理器；未配置时不得调用。所有权与退役语义同 move_only_function。
using http_error_handler = move_only_function<task<http_response>(context&, http_error_info)>;

// Answers a request that matched no route.
// 空函数指针与目标析构重入的契约同 http_error_handler。
using http_not_found_handler = move_only_function<task<http_response>(context&)>;

struct scoped_error_handler_options final {
    borrowed_text prefix_{};
    http_error_handler handler_{nullptr};
};

struct scoped_not_found_handler_options final {
    borrowed_text prefix_{};
    http_not_found_handler handler_{nullptr};
};

}  // namespace ruvia

// Worker-local user state: application::use_worker_state<T>() registers one recipe, each
// worker builds its own instance from it, and context::worker_state<T>() /
// web_worker_context::worker_state<T>() hand the instance back on that worker.
// This generalizes the db_registry pattern (one connection-pool-like object per
// single-threaded worker) to application-owned types: the instance is only
// ever touched from its worker's thread, so it needs no synchronization.

namespace ruvia::detail {

template <typename t_type>
struct type_key_tag final {
    inline static constexpr std::byte value{};
};

// One stable address per type, used as the binding's identity. Cheaper and more
// robust than typeid: no RTTI requirement and no cross-boundary name equality.
template <typename t_type>
[[nodiscard]] const void* type_key() noexcept {
    return &type_key_tag<std::remove_cvref_t<t_type>>::value;
}

// Two kinds share one intrusive list but never answer each other's lookups.
// c.req().validated<T>() means "a validator produced and checked this"; if a
// hand-bound request-state value of the same type could satisfy it, that
// guarantee would silently become a lie. Keeping the kinds disjoint preserves
// both contracts on one mechanism.
enum class request_binding_kind : std::uint8_t {
    validated_model,
    request_state,
};

class request_bindings;

struct request_binding_node final {
    const void* type_key_;
    const void* value_;
    request_binding_node* previous_;
    request_binding_kind kind_;
};

// The RAII guard a binder holds for as long as the value must stay visible.
// Neither copyable nor movable: the node it owns is linked into an intrusive
// stack by address, so it must not be relocated, and its scope IS the binding's
// lifetime. Held in the binding coroutine's frame across co_await next().
template <typename t_type>
class request_binding_guard final {
public:
    request_binding_guard(const request_binding_guard&) = delete;
    request_binding_guard& operator=(const request_binding_guard&) = delete;
    request_binding_guard(request_binding_guard&&) = delete;
    request_binding_guard& operator=(request_binding_guard&&) = delete;
    ~request_binding_guard() noexcept;

private:
    friend class request_bindings;

    request_binding_guard(request_bindings& bindings, const t_type& value, request_binding_kind kind) noexcept;

    request_bindings* bindings_;
    request_binding_node node_;
};

// context owns only the intrusive head. Every binder owns its value and its
// binding node, so nested next() calls form a naturally scoped, allocation-free
// stack that unwinds in strict LIFO order on success or failure. A binding is a
// capability visible only to its binder's downstream dynamic next() scope;
// upstream middleware cannot retain or observe it later.
class request_bindings final {
public:
    request_bindings() noexcept = default;
    ~request_bindings() noexcept {
        if (head_ != nullptr) {
            std::terminate();
        }
    }
    request_bindings(const request_bindings&) = delete;
    request_bindings& operator=(const request_bindings&) = delete;

    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>* try_find(request_binding_kind kind) const noexcept {
        const auto* node_value = find_node<t_type>(kind);
        return node_value == nullptr ? nullptr : static_cast<const std::remove_cvref_t<t_type>*>(node_value->value_);
    }

    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>& get_validated() const {
        return find_required<t_type>(request_binding_kind::validated_model, "validated request model is not available");
    }
    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>* try_get_state() const noexcept {
        return try_find<t_type>(request_binding_kind::request_state);
    }

    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>& get_state() const {
        return find_required<t_type>(request_binding_kind::request_state, "request state is not bound for this type");
    }

    template <typename t_type>
    [[nodiscard]] request_binding_guard<t_type> bind_validated(const t_type& value) {
        return request_binding_guard<t_type>(*this, value, request_binding_kind::validated_model);
    }
    template <typename t_type>
        requires(!std::is_lvalue_reference_v<t_type>)
    request_binding_guard<std::remove_cvref_t<t_type>> bind_validated(t_type&&) = delete;

    template <typename t_type>
    [[nodiscard]] request_binding_guard<t_type> bind_state(const t_type& value) {
        return request_binding_guard<t_type>(*this, value, request_binding_kind::request_state);
    }
    template <typename t_type>
        requires(!std::is_lvalue_reference_v<t_type>)
    request_binding_guard<std::remove_cvref_t<t_type>> bind_state(t_type&&) = delete;

private:
    template <typename t_type>
    friend class request_binding_guard;
    void push(request_binding_node& node_value) noexcept {
        node_value.previous_ = head_;
        head_ = &node_value;
    }

    void pop(request_binding_node& node_value) noexcept {
        if (head_ != &node_value) {
            std::terminate();
        }
        head_ = node_value.previous_;
    }

    template <typename t_type>
    [[nodiscard]] const request_binding_node* find_node(request_binding_kind kind) const noexcept {
        const auto* key = type_key<std::remove_cvref_t<t_type>>();
        for (auto* node_value = head_; node_value != nullptr; node_value = node_value->previous_) {
            if (node_value->type_key_ == key && node_value->kind_ == kind) {
                return node_value;
            }
        }
        return nullptr;
    }

    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>& find_required(request_binding_kind kind, const char* missing_message) const {
        const auto* found = try_find<t_type>(kind);
        if (found == nullptr) {
            throw std::logic_error(missing_message);
        }
        return *found;
    }

    request_binding_node* head_{nullptr};
};

template <typename t_type>
request_binding_guard<t_type>::request_binding_guard(request_bindings& bindings, const t_type& value, request_binding_kind kind) noexcept :
    bindings_(&bindings),
    node_{type_key<t_type>(), &value, nullptr, kind} {
    bindings_->push(node_);
}

template <typename t_type>
request_binding_guard<t_type>::~request_binding_guard() noexcept {
    bindings_->pop(node_);
}
// Startup-registered recipe for one per-worker state type: the erased user
// factory plus the instance create/destroy pair. The factory runs once inside
// each worker's active identity window before that worker begins dispatching
// callbacks or requests.
class worker_state_definition final {
public:
    worker_state_definition(const worker_state_definition&) = delete;
    worker_state_definition& operator=(const worker_state_definition&) = delete;

    worker_state_definition(worker_state_definition&& other) noexcept :
        type_key_(std::exchange(other.type_key_, nullptr)),
        factory_(std::exchange(other.factory_, nullptr)),
        destroy_factory_(std::exchange(other.destroy_factory_, nullptr)),
        create_instance_(std::exchange(other.create_instance_, nullptr)),
        destroy_instance_(std::exchange(other.destroy_instance_, nullptr)) {}
    worker_state_definition& operator=(worker_state_definition&&) = delete;

    ~worker_state_definition() {
        auto* const factory = std::exchange(factory_, nullptr);
        auto* const destroy = std::exchange(destroy_factory_, nullptr);
        if (factory != nullptr && destroy != nullptr) {
            destroy(factory);
        }
    }

    template <typename t_type, typename factory_type>
    [[nodiscard]] static worker_state_definition make(factory_type&& factory) {
        using stored_type = std::decay_t<factory_type>;
        static_assert(std::is_invocable_v<stored_type&>, "worker state factory must be invocable with no arguments");
        static_assert(std::is_constructible_v<t_type, std::invoke_result_t<stored_type&>>, "worker state factory must return a value that constructs T");
        worker_state_definition definition;
        definition.type_key_ = detail::type_key<t_type>();
        definition.factory_ = new stored_type(std::forward<factory_type>(factory));
        // Named apart from the enclosing `factory` parameter: these lambdas are
        // captureless and receive the erased pointer, not that object.
        definition.destroy_factory_ = [](void* stored_factory) noexcept {
            delete static_cast<stored_type*>(stored_factory);
        };
        definition.create_instance_ = [](void* stored_factory) -> void* {
            return new t_type((*static_cast<stored_type*>(stored_factory))());
        };
        definition.destroy_instance_ = [](void* instance) noexcept {
            delete static_cast<t_type*>(instance);
        };
        return definition;
    }

    // A caller-owned instance lent to one worker's registry: initialize()
    // adopts it and shutdown() leaves it to its owner.
    template <typename t_type>
    [[nodiscard]] static worker_state_definition borrow(t_type& instance) noexcept {
        static_assert(!std::is_const_v<t_type>, "a borrowed worker state must be mutable");
        worker_state_definition definition;
        definition.type_key_ = detail::type_key<t_type>();
        definition.factory_ = std::addressof(instance);
        definition.destroy_factory_ = [](void*) noexcept {};
        definition.create_instance_ = [](void* borrowed) -> void* {
            return borrowed;
        };
        definition.destroy_instance_ = [](void*) noexcept {};
        return definition;
    }

    [[nodiscard]] const void* type_key() const noexcept {
        return type_key_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return type_key_ != nullptr && factory_ != nullptr && destroy_factory_ != nullptr && create_instance_ != nullptr && destroy_instance_ != nullptr;
    }

private:
    friend class worker_state_registry;

    using destroy_factory_type = void (*)(void*) noexcept;
    using create_instance_type = void* (*)(void*);
    using destroy_instance_type = void (*)(void*) noexcept;

    worker_state_definition() noexcept = default;

    const void* type_key_{nullptr};
    void* factory_{nullptr};
    destroy_factory_type destroy_factory_{nullptr};
    create_instance_type create_instance_{nullptr};
    destroy_instance_type destroy_instance_{nullptr};
};

// The single startup validation point for worker state recipes: the registry
// only ever receives definitions that passed through here.
inline void append_worker_state_definition(std::vector<worker_state_definition>& definitions, worker_state_definition&& definition) {
    if (!definition.valid()) {
        throw std::invalid_argument("worker state definition is invalid");
    }
    for (const auto& existing : definitions) {
        if (existing.type_key() == definition.type_key()) {
            throw std::invalid_argument("worker state type is already registered");
        }
    }
    definitions.push_back(std::move(definition));
}

// The per-worker instances. worker_capabilities::start()/stop() initialize and
// destroy the registry on its worker; a throwing factory fails startup before
// the worker dispatches callbacks or requests.
// Registration order remains the lifetime order (destruction is reversed),
// while a separate immutable type index gives request-time lookup one
// allocation-free binary search.
class worker_state_registry final {
public:
    // `definitions` were validated by append_worker_state_definition.
    explicit worker_state_registry(std::span<const worker_state_definition> definitions) noexcept :
        definitions_(definitions) {}

    worker_state_registry(const worker_state_registry&) = delete;
    worker_state_registry& operator=(const worker_state_registry&) = delete;
    worker_state_registry(worker_state_registry&&) = delete;
    worker_state_registry& operator=(worker_state_registry&&) = delete;

    ~worker_state_registry() {
        if (initialized_) {
            std::terminate();
        }
    }

    void initialize() {
        if (initialized_) {
            throw std::logic_error("worker state registry is already initialized");
        }
        entries_.reserve(definitions_.size());
        type_index_.reserve(definitions_.size());
        try {
            for (const auto& definition : definitions_) {
                auto* const instance_value = definition.create_instance_(definition.factory_);
                entries_.push_back(instance_entry_type{instance_value, definition.destroy_instance_});
                type_index_.push_back(type_index_entry_type{definition.type_key_, instance_value});
            }
            std::ranges::sort(type_index_, std::less<const void*>{}, &type_index_entry_type::type_key_);
        } catch (...) {
            destroy_entries();
            throw;
        }
        initialized_ = true;
    }

    void shutdown() noexcept {
        if (!initialized_) {
            return;
        }
        destroy_entries();
        initialized_ = false;
    }

    [[nodiscard]] void* instance(const void* type_key) const noexcept {
        const auto match = std::ranges::lower_bound(type_index_, type_key, std::less<const void*>{}, &type_index_entry_type::type_key_);
        if (match == type_index_.end() || match->type_key_ != type_key) {
            return nullptr;
        }
        return match->instance_;
    }

private:
    struct instance_entry_type final {
        void* instance_{nullptr};
        worker_state_definition::destroy_instance_type destroy_{nullptr};
    };

    struct type_index_entry_type final {
        const void* type_key_{nullptr};
        void* instance_{nullptr};
    };

    void destroy_entries() noexcept {
        type_index_.clear();
        for (std::size_t i = entries_.size(); i > 0; --i) {
            auto& entry_value = entries_[i - 1];
            auto* const instance = std::exchange(entry_value.instance_, nullptr);
            auto* const destroy = std::exchange(entry_value.destroy_, nullptr);
            if (instance != nullptr && destroy != nullptr) {
                destroy(instance);
            }
        }
        entries_.clear();
    }

    std::span<const worker_state_definition> definitions_;
    std::vector<instance_entry_type> entries_;
    std::vector<type_index_entry_type> type_index_;
    bool initialized_{false};
};

// A request and a posted job borrow the same worker-owned capabilities. No
// dispatcher or cancellation ownership is copied until an explicit offload.
class worker_context_capabilities final {
public:
    worker_context_capabilities(const worker_ref& worker_value, const ruvia::stop_token& stop_token_value, const worker_state_registry* worker_states) noexcept :
        worker_(worker_value),
        stop_token_(stop_token_value),
        worker_states_(worker_states) {}
    worker_context_capabilities(worker_ref&&, const ruvia::stop_token&, const worker_state_registry*) = delete;
    worker_context_capabilities(const worker_ref&, ruvia::stop_token&&, const worker_state_registry*) = delete;
    worker_context_capabilities(worker_ref&&, ruvia::stop_token&&, const worker_state_registry*) = delete;

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] const ruvia::stop_token& stop_token() const noexcept {
        return stop_token_;
    }

    [[nodiscard]] const worker_state_registry* worker_states() const noexcept {
        return worker_states_;
    }

    // A router serving the request substitutes its own registry for the
    // duration of its dispatch.
    void bind_worker_states(const worker_state_registry* worker_states) noexcept {
        worker_states_ = worker_states;
    }

    template <typename state_type>
    [[nodiscard]] state_type& worker_state() const {
        return *static_cast<state_type*>(worker_state_instance(type_key<state_type>()));
    }

    // Runs on the blocking pool of this worker's event loop; without one the
    // result reports blocking_status::unavailable. A supplied stop token is
    // combined with this worker's cancellation token for the wait.
    template <typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(callable_type callable, operation_options options) const {
        if (!options.stop_token_.has_state()) {
            options.stop_token_ = stop_token_;
            return ruvia::run_blocking(worker_, std::move(options), std::move(callable));
        }
        return run_blocking_combined(worker_, stop_token_, std::move(options), std::move(callable));
    }

private:
    template <typename callable_type>
    [[nodiscard]] static task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking_combined(worker_ref worker_value, ruvia::stop_token worker_token, operation_options options, callable_type callable) {
        const stop_group stop_owner(worker_token, options.stop_token_);
        options.stop_token_ = stop_owner.token();
        co_return co_await ruvia::run_blocking(std::move(worker_value), std::move(options), std::move(callable));
    }

    [[nodiscard]] void* worker_state_instance(const void* type_key) const;

    const worker_ref& worker_;
    const ruvia::stop_token& stop_token_;
    const worker_state_registry* worker_states_;
};

}  // namespace ruvia::detail

namespace ruvia {

class http_client_ref;
#ifdef RUVIA_ENABLE_DATABASE
class db_session;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_session;
#endif

namespace detail {
class web_worker_dispatch;
class worker_client_registry_view;
}  // namespace detail
class web_worker_context final {
public:
    web_worker_context(const web_worker_context&) = delete;
    web_worker_context& operator=(const web_worker_context&) = delete;
    web_worker_context(web_worker_context&&) = delete;
    web_worker_context& operator=(web_worker_context&&) = delete;

    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;
    [[nodiscard]] ::ruvia::stop_token stop_token() const noexcept;

    template <typename state_type>
    [[nodiscard]] state_type& worker_state() const {
        return capabilities_.worker_state<state_type>();
    }

    // The callable owns its inputs and must not borrow worker state. Completion
    // resumes here; worker shutdown and options.timeout_ end the wait, not the callable.
    // Rejection and callable exceptions are retained in blocking_result; inspect
    // status() or use std::move(result).take_value() to throw. The worker stop token
    // is bound automatically and combined with options.stop_token_. With application::blocking_pool(nullptr) the
    // result reports blocking_status::unavailable.
    template <typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(callable_type callable, operation_options options = {}) const {
        return capabilities_.run_blocking(std::move(callable), std::move(options));
    }

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_session db() const;
    [[nodiscard]] db_session db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_session redis() const;
    [[nodiscard]] redis_session redis(std::string_view alias) const;
#endif
    [[nodiscard]] http_client_ref client() const;
    [[nodiscard]] http_client_ref client(std::string_view alias) const;

private:
    friend class detail::web_worker_dispatch;

    web_worker_context(const worker_ref& worker_value, const detail::worker_client_registry_view& client_registries, const detail::worker_state_registry* worker_states, const ::ruvia::stop_token& stop_token_value) noexcept;
    web_worker_context(worker_ref&&, const detail::worker_client_registry_view&, const detail::worker_state_registry*, const ::ruvia::stop_token&) = delete;
    web_worker_context(const worker_ref&, const detail::worker_client_registry_view&, const detail::worker_state_registry*, ::ruvia::stop_token&&) = delete;
    web_worker_context(worker_ref&&, const detail::worker_client_registry_view&, const detail::worker_state_registry*, ::ruvia::stop_token&&) = delete;

    // The server worker owns these values and joins every posted job before
    // retiring them. Contexts borrow them so starting a job does not copy
    // endpoint or cancellation-state ownership on the worker thread.
    detail::worker_context_capabilities capabilities_;
    const detail::worker_client_registry_view& client_registries_;
    // Each posted callback gets an independent operation lifetime. Declared
    // last so cold frames are destroyed before the callback context disappears.
    mutable ::ruvia::operation_scope operation_scope_;
};

using web_worker_post_result = post_outcome<task<void>(web_worker_context&)>;

struct web_worker_stats final {
    std::uint64_t accepted_{0};
    std::uint64_t queue_full_{0};
    std::uint64_t worker_stopping_{0};
    std::uint64_t completed_{0};
    std::uint64_t failed_{0};
    std::size_t outstanding_{0};
};

class web_worker_ref final {
public:
    web_worker_ref() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;
    [[nodiscard]] web_worker_stats stats() const noexcept;

    template <typename fn_type>
        requires std::invocable<std::decay_t<fn_type>&, web_worker_context&> && std::same_as<std::invoke_result_t<std::decay_t<fn_type>&, web_worker_context&>, task<void>>
    [[nodiscard]] web_worker_post_result post(fn_type&& fn) const {
        return post_task(move_only_function<task<void>(web_worker_context&)>(std::forward<fn_type>(fn)));
    }

private:
    friend class detail::web_worker_dispatch;

    web_worker_ref(std::shared_ptr<detail::web_worker_dispatch> dispatch) noexcept;

    [[nodiscard]] web_worker_post_result post_task(move_only_function<task<void>(web_worker_context&)> task) const;

    // The handle owns a stable terminal endpoint. Server shutdown closes it;
    // retaining a handle cannot retain the server or its io_context.
    std::shared_ptr<detail::web_worker_dispatch> dispatch_;
};

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

            [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
                return awaiter_.await_suspend(continuation);
            }

            void await_resume() {
                awaiter_.await_resume();
            }

            const awaiter* operator&() const = delete;
            awaiter* operator&() = delete;

        private:
            friend class awaitable;

            explicit awaiter(task<void>&& task_value) :
                awaiter_(std::move(task_value).operator co_await()) {}

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

        constexpr awaitable(detail::next_state state_value, detail::next_invoke_type invoke) noexcept :
            state_(state_value),
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

    constexpr next(detail::next_state state_value, detail::next_invoke_type invoke) noexcept :
        state_(state_value),
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
// use<T>() call site needs controller.h or app.h (test_app).
// This header stays free of context so app.h does not pull it in.
template <typename middleware_type>
concept middleware = std::is_class_v<middleware_type> && std::is_final_v<middleware_type> && (requires { static_cast<task<void> (middleware_type::*)(context&, next&)>(&middleware_type::invoke); } || requires { static_cast<task<void> (middleware_type::*)(context&, next&) const>(&middleware_type::invoke); });
template <typename policy_type>
concept route_policy = std::is_class_v<policy_type> && std::is_final_v<policy_type> && !middleware<policy_type> && (requires {
    { policy_type::ruvia_request_body_limit_bytes } -> std::convertible_to<std::size_t>;
} || requires {
    { policy_type::ruvia_stream_request_body } -> std::convertible_to<bool>;
});

template <typename modifier_type>
concept route_modifier = middleware<modifier_type> || route_policy<modifier_type>;

// Internal startup-time middleware descriptor.
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

    constexpr controller_middleware_descriptor(invoke_type invoke, create_type create, destroy_type destroy, const void* args, const void* validated_model_type_key, bool uses_route_rate_limit, bool runs_on_unmatched_requests, std::size_t request_body_limit, bool stream_request_body = false) noexcept :
        invoke_(invoke),
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

    owned_middleware_descriptor(controller_middleware_descriptor descriptor, release_args_type release_args) noexcept :
        descriptor_(descriptor),
        release_args_(release_args) {}

    owned_middleware_descriptor(const owned_middleware_descriptor&) = delete;
    owned_middleware_descriptor& operator=(const owned_middleware_descriptor&) = delete;

    owned_middleware_descriptor(owned_middleware_descriptor&& other) noexcept :
        prefix_(std::move(other.prefix_)),
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
    if constexpr (requires { static_cast<task<void> (middleware_type::*)(context&, next&)>(&middleware_type::invoke); }) {
        constexpr auto method = static_cast<task<void> (middleware_type::*)(context&, next&)>(&middleware_type::invoke);
        return (static_cast<middleware_type*>(target)->*method)(context_value, next_value);
    } else {
        constexpr auto method = static_cast<task<void> (middleware_type::*)(context&, next&) const>(&middleware_type::invoke);
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
        return std::apply([](const auto&... values) {
            return static_cast<void*>(new middleware_type(values...));
        }, *static_cast<const args_type*>(args));
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
    return controller_middleware_descriptor(&invoke_middleware<middleware_type>, &create_middleware<middleware_type, args_type>, &destroy_middleware<middleware_type>, args, middleware_validated_model_type_key<middleware_type>(), middleware_uses_route_rate_limit<middleware_type>(), middleware_runs_on_unmatched_requests<middleware_type>(), middleware_request_body_limit<middleware_type>(), middleware_stream_request_body<middleware_type>());
}

// A default-constructed middleware, as controller and route middleware lists
// declare it. It captures nothing, so it allocates nothing and owns nothing:
// route registration reruns on every route plan compile.
template <middleware middleware_type>
[[nodiscard]] controller_middleware_descriptor make_middleware_descriptor() noexcept {
    static_assert(std::is_default_constructible_v<middleware_type>, "middleware registered without arguments must be default constructible");
    return describe_middleware<middleware_type, std::tuple<>>(nullptr);
}

template <route_policy policy_type>
[[nodiscard]] constexpr controller_middleware_descriptor describe_route_policy() noexcept {
    return controller_middleware_descriptor(nullptr, nullptr, nullptr, nullptr, nullptr, false, false, middleware_request_body_limit<policy_type>(), middleware_stream_request_body<policy_type>());
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
        return owned_middleware_descriptor(describe_middleware<middleware_type, args_tuple_type>(stored), &release_middleware_args<args_tuple_type>);
    }
}

}  // namespace ruvia::detail

namespace ruvia {

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
    default_security_header_policy content_type_options_header_ = default_security_header_policy::emit_default;
    default_security_header_policy frame_options_header_ = default_security_header_policy::emit_default;
    // Emitted only for requests received over TLS. Plain HTTP responses must
    // never carry Strict-Transport-Security.
    default_security_header_policy strict_transport_security_header_ = default_security_header_policy::emit_default;
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
[[nodiscard]] std::string_view format_rate_limit_address_key(const ip_address& address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept;
[[nodiscard]] std::string_view format_rate_limit_address_key(std::string_view address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept;

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
    explicit sliding_window_rate_limiter(std::size_t capacity_per_worker = default_rate_limit_capacity_per_worker);
    sliding_window_rate_limiter(const sliding_window_rate_limiter&) = delete;
    sliding_window_rate_limiter& operator=(const sliding_window_rate_limiter&) = delete;
    sliding_window_rate_limiter(sliding_window_rate_limiter&&) noexcept;
    sliding_window_rate_limiter& operator=(sliding_window_rate_limiter&&) noexcept;
    ~sliding_window_rate_limiter();

    [[nodiscard]] bool has_default_rule() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

    [[nodiscard]] rate_limit_decision decide(std::string_view key, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;
    [[nodiscard]] rate_limit_decision decide(const ip_address& address, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;

    [[nodiscard]] rate_limit_decision decide(std::string_view key, const rate_limit_rule& rule, std::uintptr_t scope = 1, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;
    [[nodiscard]] rate_limit_decision decide(const ip_address& address, const rate_limit_rule& rule, std::uintptr_t scope = 1, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;

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
template <std::size_t max_requests, std::int64_t window_ms, rate_limit_overflow_policy overflow_policy = rate_limit_overflow_policy::deny>
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

// A request-local capability bound by session_middleware. The handle borrows
// context state and therefore cannot escape the request.
// Changes are persisted automatically before the response head (including a
// stream head or a websocket/CONNECT handshake) is sent. Once that commit
// begins, set(), clear(), and regenerate() throw std::logic_error; payload()
// remains readable.
class session final {
public:
    [[nodiscard]] std::string_view payload() const& noexcept;
    std::string_view payload() const&& = delete;
    [[nodiscard]] bool empty() const noexcept {
        return payload().empty();
    }
    // Every non-empty write replaces an existing session identity at commit.
    // Repeated writes in one request publish one new ID; reads keep the ID.
    void set(std::string_view payload);
    void clear();
    // Rotate without changing the payload, for an authentication or privilege
    // transition that does not write the session. set() already requests rotation.
    // Replacing an expired or revoked session fails with HTTP 409.
    void regenerate();

private:
    explicit session(detail::context_session_state& state_value) noexcept :
        state_(&state_value) {}

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
