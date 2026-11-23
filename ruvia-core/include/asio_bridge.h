#pragma once

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include <asio/associated_allocator.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/awaitable.hpp>
#include <asio/bind_allocator.hpp>
#include <asio/disposition.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

#include "lifetimebound.h"
#include "suspend_race.h"
#include "task.h"

namespace ruvia {

class worker_ref;

// The completion argument of async_start(). Void tasks complete with
// std::exception_ptr, which satisfies Asio's disposition concept directly;
// value-returning tasks complete with the value first and the failure second,
// passed by rvalue reference so over-aligned results are never passed by value
// on MSVC.
template <typename t_type>
    requires detail::asio_task_result<t_type>
using task_completion_result = std::conditional_t<std::is_void_v<t_type>, std::exception_ptr, std::variant<t_type, std::exception_ptr>>;

// Asio completion signatures always provide an error code and may also provide
// a result that remains meaningful on partial failure (for example transferred
// bytes). The completion therefore owns both fields. Void completions carry
// only the error code: the value accessors and the two-argument constructor
// exist only for a non-void result_type.
template <typename result_type>
class asio_completion final {
    using result_storage_type = std::conditional_t<std::is_void_v<result_type>, std::monostate, result_type>;

public:
    explicit asio_completion(std::error_code code) noexcept
        requires std::is_void_v<result_type>
        :
        error_code_(code),
        result_{} {}

    asio_completion(std::error_code code, result_storage_type value) noexcept(std::is_nothrow_move_constructible_v<result_storage_type>)
        requires(!std::is_void_v<result_type>)
        :
        error_code_(code),
        result_(std::move(value)) {}

    asio_completion(const asio_completion&) = default;
    asio_completion(asio_completion&&) = default;
    asio_completion& operator=(const asio_completion&) = delete;
    asio_completion& operator=(asio_completion&&) = delete;

    [[nodiscard]] std::error_code code() const noexcept {
        return error_code_;
    }

    [[nodiscard]] bool ok() const noexcept {
        return !error_code_;
    }

    [[nodiscard]] bool failed() const noexcept {
        return static_cast<bool>(error_code_);
    }

    [[nodiscard]] const result_storage_type& value() const& noexcept RUVIA_LIFETIMEBOUND
        requires(!std::is_void_v<result_type>)
    {
        return result_;
    }
    const result_storage_type& value() const&& = delete;

    [[nodiscard]] result_storage_type take_value() && noexcept(std::is_nothrow_move_constructible_v<result_storage_type>)
        requires(!std::is_void_v<result_type>)
    {
        return std::move(result_);
    }

private:
    std::error_code error_code_;
    [[no_unique_address]] result_storage_type result_;
};

// The single-use awaiter of one completion-based Asio operation, as returned
// by async_asio(). It owns the initiation and the completion state until the
// operation finishes, and is neither copyable, movable, nor addressable; an
// owner that keeps it beyond one await-expression constructs it in place
// (for example std::optional::emplace) from the initiation.
template <typename result_type, typename initiate_type>
class asio_completion_awaiter final {
public:
    explicit asio_completion_awaiter(initiate_type initiate) :
        initiate_(std::move(initiate)) {}
    asio_completion_awaiter(const asio_completion_awaiter&) = delete;
    asio_completion_awaiter& operator=(const asio_completion_awaiter&) = delete;
    asio_completion_awaiter(asio_completion_awaiter&&) = delete;
    asio_completion_awaiter& operator=(asio_completion_awaiter&&) = delete;
    void operator&() const = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    // If initiate_ throws, the exception propagates from the await-expression
    // directly ([expr.await]/5), without an exception_ptr side channel.
    //
    // An asio initiation may complete synchronously (for example a closed
    // socket or a resolver cache hit), invoking the completion handler before
    // await_suspend returns. Resuming the continuation from inside that window
    // would run the coroutine while it is still suspended-by-await_suspend and
    // potentially destroy its frame before this function returns. The
    // suspend_race_state records the completion instead; the return value of
    // await_suspend then reports the actual race order, so a synchronous
    // completion is consumed by await_resume without resuming at all.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
        if constexpr (std::is_void_v<result_type>) {
            initiate_([this, continuation](std::error_code ec, auto&&...) mutable {
                if (state_.complete(asio_completion<void>(ec))) {
                    continuation.resume();
                }
            });
        } else {
            initiate_([this, continuation](std::error_code ec, result_type result_value) mutable {
                if (state_.complete(asio_completion<result_type>(ec, std::move(result_value)))) {
                    continuation.resume();
                }
            });
        }
        return state_.suspend(continuation);
    }

    [[nodiscard]] asio_completion<result_type> await_resume() {
        return state_.take_value();
    }

private:
    initiate_type initiate_;
    suspend_race_state<asio_completion<result_type>> state_;
};

// Adapts a completion-based Asio operation for a Ruvia coroutine. The
// initiation receives one handler taking (std::error_code[, result_type]).
template <typename result_type = void, typename initiate_type>
[[nodiscard]] auto async_asio(initiate_type&& initiate) {
    return asio_completion_awaiter<result_type, std::decay_t<initiate_type>>(std::forward<initiate_type>(initiate));
}

// The worker's io_context executor for native I/O bound to that worker. It
// borrows the runtime's io_context: finish its operations before the runtime
// or attachment retires. Empty or detached worker refs throw std::logic_error;
// stopping workers still expose it so already admitted operations can finish.
[[nodiscard]] asio::io_context::executor_type asio_executor(const worker_ref& worker);

// Close a TCP socket and cancel any outstanding I/O.
inline void close_socket(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

// Parse a complete textual IP address, rejecting embedded NUL bytes, bracketed
// forms, ports, and partial or padded IPv4 octets.
[[nodiscard]] std::optional<asio::ip::address> try_parse_asio_ip_address(std::string_view text) noexcept;

[[nodiscard]] inline asio::ip::address parse_asio_ip_address(std::string_view text) {
    if (const auto parsed = try_parse_asio_ip_address(text)) {
        return *parsed;
    }
    throw std::invalid_argument("invalid IP address literal");
}

}  // namespace ruvia

namespace ruvia::detail {

// Owns the started task and the completion handler, allocated with the
// handler's associated allocator, until delivery on its associated executor.
template <typename t_type, typename handler_type>
class task_completion_state final {
public:
    using handler_allocator_type = asio::associated_allocator_t<handler_type>;
    using state_allocator_type = typename std::allocator_traits<handler_allocator_type>::template rebind_alloc<task_completion_state>;
    using state_allocator_traits_type = std::allocator_traits<state_allocator_type>;

    static task_completion_state* create(task<t_type>&& task_value, handler_type&& handler_value) {
        handler_allocator_type handler_allocator(asio::get_associated_allocator(handler_value));
        state_allocator_type state_allocator(handler_allocator);
        auto* state_value = state_allocator_traits_type::allocate(state_allocator, 1);
        try {
            state_allocator_traits_type::construct(state_allocator, state_value, std::move(task_value), std::forward<handler_type>(handler_value), std::move(handler_allocator));
        } catch (...) {
            state_allocator_traits_type::deallocate(state_allocator, state_value, 1);
            throw;
        }
        return state_value;
    }

    task_completion_state(task<t_type>&& task_value, handler_type&& handler_value, handler_allocator_type allocator_value) :
        task_(std::move(task_value)),
        handler_(std::forward<handler_type>(handler_value)),
        allocator_(std::move(allocator_value)) {}

    void start() {
        // Allocator and handler setup may retire a scoped cold frame.
        if (task_.registration_.scoped()) {
            try {
                task_.prepare_direct_start();
            } catch (...) {
                state_deleter_type{state_allocator_type(allocator_)}(this);
                throw;
            }
        }
        task_.coroutine_.promise().control_.set_completion(this, &task_completion_state::complete);
        task_.start();
    }

    static void complete(void* raw) noexcept {
        auto* state_value = static_cast<task_completion_state*>(raw);
        auto executor = asio::get_associated_executor(state_value->handler_);
        auto completion_allocator = state_value->allocator_;
        state_owner_type owner_value(state_value, state_deleter_type{state_allocator_type(state_value->allocator_)});
        asio::post(executor, asio::bind_allocator(std::move(completion_allocator), [owner_value = std::move(owner_value)]() mutable {
            deliver(std::move(owner_value));
        }));
    }

private:
    struct state_deleter_type final {
        void operator()(task_completion_state* state_value) noexcept {
            state_allocator_traits_type::destroy(allocator_, state_value);
            state_allocator_traits_type::deallocate(allocator_, state_value, 1);
        }

        state_allocator_type allocator_;
    };

    using state_owner_type = std::unique_ptr<task_completion_state, state_deleter_type>;

    static void deliver(state_owner_type owner_value) {
        // The delivery owner outlives the result and every callback argument.
        // An owning completion handler can publish external readiness in its
        // destructor only after moved-from result storage has been reclaimed.
        auto handler = std::move(owner_value->handler_);
        owner_value->task_.prepare_direct_completion();
        auto result_value = [&owner_value]() -> task_completion_result<t_type> {
            try {
                if constexpr (std::is_void_v<t_type>) {
                    owner_value->task_.coroutine_.promise().result();
                    return {};
                } else {
                    return task_completion_result<t_type>(std::in_place_index<0>, owner_value->task_.coroutine_.promise().result());
                }
            } catch (...) {
                if constexpr (std::is_void_v<t_type>) {
                    return std::current_exception();
                } else {
                    return task_completion_result<t_type>(std::in_place_index<1>, std::current_exception());
                }
            }
        }();
        owner_value->task_.finish_direct_completion();
        owner_value.reset();
        std::move(handler)(std::move(result_value));
    }

    task<t_type> task_;
    handler_type handler_;
    handler_allocator_type allocator_;
};

template <typename t_type>
    requires asio_task_result<t_type> && (!std::is_void_v<t_type>)
[[nodiscard]] asio::awaitable<t_type> await_non_void_task(task<t_type> task_value) {
    auto result_value = co_await async_start(std::move(task_value), asio::use_awaitable);
    if (result_value.index() == 1) {
        std::rethrow_exception(std::get<1>(result_value));
    }
    co_return std::move(std::get<0>(result_value));
}

}  // namespace ruvia::detail

namespace ruvia {

// Starts a lazy task as an Asio asynchronous operation. The task starts
// inline on the calling thread; its completion is delivered through the
// handler's associated executor with a task_completion_result<T>, after the
// coroutine frame has been reclaimed. The operation state is allocated with
// the handler's associated allocator. Any completion token works: a callback,
// use_awaitable, use_future, deferred, or a custom token.
template <typename t_type, typename completion_token_type>
    requires detail::asio_task_result<t_type>
auto async_start(task<t_type> task_value, completion_token_type&& token) {
    using completion_signature_type = std::conditional_t<std::is_void_v<t_type>, void(std::exception_ptr), void(task_completion_result<t_type>&&)>;
    return asio::async_initiate<completion_token_type, completion_signature_type>([](auto&& handler, task<t_type> started_task) {
        // A deferred token may initiate after the cold task's scope closes.
        started_task.prepare_direct_start();
        using handler_type = std::decay_t<decltype(handler)>;
        auto* state_value = detail::task_completion_state<t_type, handler_type>::create(std::move(started_task), std::forward<decltype(handler)>(handler));
        state_value->start();
    }, token, std::move(task_value));
}

// Adapts a lazy task for an Asio coroutine owner such as co_spawn. A failed
// task rethrows its exception from the awaitable. Void tasks use Asio's
// std::exception_ptr disposition, so use_awaitable produces the awaitable<void>
// directly without a wrapper coroutine frame.
template <typename t_type>
    requires detail::asio_task_result<t_type>
[[nodiscard]] asio::awaitable<t_type> as_awaitable(task<t_type> task_value) {
    if constexpr (std::is_void_v<t_type>) {
        return async_start(std::move(task_value), asio::use_awaitable);
    } else {
        return detail::await_non_void_task(std::move(task_value));
    }
}

}  // namespace ruvia
