#pragma once

#include <coroutine>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "byte_view.h"
#include "suspend_race.h"

// Build-only Asio glue shared by ruvia-core, ruvia-web, and the tests through
// ruvia_core_internal; never installed.
namespace ruvia {

class worker_ref;

// Asio completion signatures always provide an error code and may also provide
// a result that remains meaningful on partial failure (for example transferred
// bytes). The completion therefore owns both fields. Void completions carry
// only the error code: the result accessors and the two-argument factory exist
// only for a non-void result_type.
template <typename result_type>
class asio_completion final {
    using result_storage_type = std::conditional_t<std::is_void_v<result_type>, std::monostate, result_type>;

public:
    asio_completion(const asio_completion&) = default;
    asio_completion(asio_completion&&) = default;
    asio_completion& operator=(const asio_completion&) = delete;
    asio_completion& operator=(asio_completion&&) = delete;

    [[nodiscard]] static asio_completion completed(std::error_code error_code) noexcept
        requires std::is_void_v<result_type>
    {
        return asio_completion(error_code, result_storage_type{});
    }

    [[nodiscard]] static asio_completion completed(std::error_code error_code, result_storage_type result_value)
        requires(!std::is_void_v<result_type>)
    {
        return asio_completion(error_code, std::move(result_value));
    }

    [[nodiscard]] std::error_code error_code() const noexcept {
        return error_code_;
    }

    [[nodiscard]] bool succeeded() const noexcept {
        return !error_code_;
    }

    [[nodiscard]] bool failed() const noexcept {
        return static_cast<bool>(error_code_);
    }

    [[nodiscard]] const result_storage_type& result() const& noexcept RUVIA_LIFETIMEBOUND
        requires(!std::is_void_v<result_type>)
    {
        return result_;
    }
    const result_storage_type& result() const&& = delete;

    [[nodiscard]] result_storage_type take_result() && noexcept(std::is_nothrow_move_constructible_v<result_storage_type>)
        requires(!std::is_void_v<result_type>)
    {
        return std::move(result_);
    }

private:
    asio_completion(std::error_code error_code, result_storage_type&& result_value) noexcept(std::is_nothrow_move_constructible_v<result_storage_type>) :
        error_code_(error_code),
        result_(std::move(result_value)) {}

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
                if (state_.complete(asio_completion<void>::completed(ec))) {
                    continuation.resume();
                }
            });
        } else {
            initiate_([this, continuation](std::error_code ec, result_type result_value) mutable {
                if (state_.complete(asio_completion<result_type>::completed(ec, std::move(result_value)))) {
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
    detail::suspend_race_state<asio_completion<result_type>> state_;
};

// Adapts a completion-based Asio operation for a Ruvia coroutine. The
// initiation receives one handler taking (std::error_code[, result_type]).
template <typename result_type = void, typename initiate_type>
[[nodiscard]] auto async_asio(initiate_type&& initiate) {
    return asio_completion_awaiter<result_type, std::decay_t<initiate_type>>(std::forward<initiate_type>(initiate));
}

// Close a socket and cancel any outstanding I/O.
inline void close_socket(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

namespace detail {

// The worker's io_context executor for native I/O bound to that worker. It
// borrows the runtime's io_context: finish its operations before the runtime
// or attachment retires. Empty or detached worker refs throw std::logic_error;
// stopping workers still expose it so already admitted operations can finish.
[[nodiscard]] asio::io_context::executor_type worker_executor(const worker_ref& worker);

}  // namespace detail

}  // namespace ruvia
