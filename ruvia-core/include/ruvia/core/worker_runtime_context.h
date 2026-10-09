#pragma once

#include <cstddef>
#include <exception>
#include <memory>

#include <asio/io_context.hpp>

#include "ruvia/core/move_only_function.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_submission_view.h"

namespace ruvia {

// Owns the worker dispatcher endpoint and detaches escaped handles on teardown.
// The io_context remains owned by the runtime that supplied it.
class worker_runtime_context final {
public:
    worker_runtime_context(asio::io_context& io_context, std::size_t queue_capacity);
    ~worker_runtime_context();

    worker_runtime_context(const worker_runtime_context&) = delete;
    worker_runtime_context& operator=(const worker_runtime_context&) = delete;
    worker_runtime_context(worker_runtime_context&&) = delete;
    worker_runtime_context& operator=(worker_runtime_context&&) = delete;

    [[nodiscard]] asio::io_context& io_context() const noexcept;
    [[nodiscard]] const worker_handle& handle() const noexcept;
    [[nodiscard]] worker_submission_view submission() const& noexcept;
    worker_submission_view submission() const&& = delete;

    void run();
    void run(move_only_function<void(std::exception_ptr)> failure_handler);
    void run(move_only_function<void()> startup_handler,
        move_only_function<void(std::exception_ptr)> failure_handler,
        move_only_function<void()> shutdown_handler);

    void close() noexcept;
    void defer_or_terminate(move_only_function<void()> task) noexcept;
    void stop_timers() noexcept;
    void detach() noexcept;

private:
    class impl_type;
    std::unique_ptr<impl_type> impl_;
};

}  // namespace ruvia
