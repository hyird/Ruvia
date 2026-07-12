#include "test_harness.h"

#include <algorithm>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/http1/Http1ResponseHeadPlan.h"
#include "ruvia/http/detail/http1/Http1ServerConnectionPlan.h"
#include "ruvia/http/detail/server/HttpResponseHead.h"
#include "ruvia/http/detail/server/HttpResponseHeadBuffer.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/web/detail/server/Http1BufferedResponseWrite.h"
#include "ruvia/web/detail/server/HttpResponseWriter.h"

namespace {

using ruvia::HttpKnownMethod;
using ruvia::HttpResponse;
using ruvia::WorkerMemory;
using ruvia::detail::Http1BufferedResponseWriteFailedAfterCommit;
using ruvia::detail::Http1BufferedResponseWriteFailedBeforeCommit;
using ruvia::detail::Http1BufferedResponseWriteResult;
using ruvia::detail::Http1ServerConnectionPlan;
using ruvia::detail::ResponseHeadBuffer;
using ruvia::detail::appendResponseHead;
using ruvia::detail::classifyHttp1BufferedResponseWrite;
using ruvia::detail::http1BufferedResponsePlan;
using ruvia::detail::httpBufferedResponseWritePlan;
using ruvia::detail::taskAsAwaitable;
using ruvia::detail::writeResponseWithScratch;

template <typename Alternative>
concept HasStatus = requires(const Alternative& alternative) {
    { alternative.status() } -> std::same_as<std::uint16_t>;
};

template <typename Alternative>
concept HasError = requires(const Alternative& alternative) {
    { alternative.error() } -> std::same_as<const std::error_code&>;
};

static_assert(!std::is_default_constructible_v<
    Http1BufferedResponseWriteResult>);
static_assert(!HasStatus<Http1BufferedResponseWriteResult>);
static_assert(!HasError<Http1BufferedResponseWriteResult>);
static_assert(!HasStatus<Http1BufferedResponseWriteFailedBeforeCommit>);
static_assert(HasError<Http1BufferedResponseWriteFailedBeforeCommit>);
static_assert(HasStatus<Http1BufferedResponseWriteFailedAfterCommit>);
static_assert(HasError<Http1BufferedResponseWriteFailedAfterCommit>);

struct WriteStep final {
    std::size_t bytes;
    std::error_code error;
};

// Minimal AsyncWriteStream used to make asio::async_write stop at an exact
// byte boundary. Each step completes asynchronously, preserving the composed
// operation's cumulative bytes-transferred count.
class ScriptedWriteStream final {
public:
    using executor_type = asio::io_context::executor_type;

    ScriptedWriteStream(
        asio::io_context& context,
        std::span<const WriteStep> steps) noexcept
        : executor_(context.get_executor()),
          steps_(steps) {}

    [[nodiscard]] executor_type get_executor() const noexcept {
        return executor_;
    }

    template <typename ConstBufferSequence, typename Handler>
    void async_write_some(
        const ConstBufferSequence& buffers,
        Handler&& handler) {
        const auto available = asio::buffer_size(buffers);
        WriteStep step{available, {}};
        if (stepIndex_ < steps_.size()) {
            step = steps_[stepIndex_++];
        }
        const auto transferred = std::min(available, step.bytes);
        asio::post(
            executor_,
            [handler = std::forward<Handler>(handler),
             error = step.error,
             transferred]() mutable {
                std::move(handler)(error, transferred);
            });
    }

private:
    executor_type executor_;
    std::span<const WriteStep> steps_;
    std::size_t stepIndex_{0};
};

enum class WriteScenario {
    kCompleted,
    kFailedBeforeCommit,
    kFailedAfterCommit
};

[[nodiscard]] Http1BufferedResponseWriteResult runBufferedWrite(
    WriteScenario scenario,
    std::size_t bodyBytes = 7) {
    WorkerMemory memory;
    HttpResponse response(std::pmr::get_default_resource());
    response.status(207);
    const std::string body(bodyBytes, 'x');
    response.setBodyView(body);
    const auto responsePlan = http1BufferedResponsePlan(
        httpBufferedResponseWritePlan(HttpKnownMethod::kGet, response),
        Http1ServerConnectionPlan::http11Close());

    ResponseHeadBuffer measuredHead(memory.allocator<char>());
    appendResponseHead(response, measuredHead, responsePlan.headPlan());
    const auto headBytes = measuredHead.view().size();
    if (headBytes <= 1) {
        throw std::logic_error("HTTP/1 response head is unexpectedly empty");
    }

    const auto reset = std::error_code(asio::error::connection_reset);
    std::vector<WriteStep> steps;
    switch (scenario) {
        case WriteScenario::kCompleted:
            steps.push_back(WriteStep{headBytes + body.size(), {}});
            break;
        case WriteScenario::kFailedBeforeCommit:
            steps.push_back(WriteStep{headBytes - 1, {}});
            steps.push_back(WriteStep{0, reset});
            break;
        case WriteScenario::kFailedAfterCommit:
            steps.push_back(WriteStep{headBytes, {}});
            steps.push_back(WriteStep{0, reset});
            break;
    }

    asio::io_context context(1);
    ScriptedWriteStream stream(context, steps);
    ResponseHeadBuffer responseHead(memory.allocator<char>());
    std::optional<Http1BufferedResponseWriteResult> result;
    std::exception_ptr exception;
    asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            try {
                result.emplace(co_await taskAsAwaitable(
                    writeResponseWithScratch(
                        stream,
                        memory,
                        responseHead,
                        nullptr,
                        response,
                        responsePlan)));
            } catch (...) {
                exception = std::current_exception();
            }
        },
        asio::detached);
    context.run();
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
    if (!result.has_value()) {
        throw std::logic_error("HTTP/1 buffered write produced no result");
    }
    return std::move(*result);
}

}  // namespace

RUVIA_TEST(http1_buffered_write_completion_owns_plan_status) {
    const auto result = runBufferedWrite(WriteScenario::kCompleted);
    const auto* completed = result.completed();
    RUVIA_CHECK(completed != nullptr);
    if (completed == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(completed->status(), std::uint16_t{207});
    RUVIA_CHECK(result.failedBeforeCommit() == nullptr);
    RUVIA_CHECK(result.failedAfterCommit() == nullptr);
}

RUVIA_TEST(http1_buffered_write_partial_head_has_no_status) {
    const auto result = runBufferedWrite(
        WriteScenario::kFailedBeforeCommit);
    const auto* failed = result.failedBeforeCommit();
    RUVIA_CHECK(failed != nullptr);
    if (failed == nullptr) {
        return;
    }
    RUVIA_CHECK(
        failed->error() ==
        asio::error::connection_reset);
    RUVIA_CHECK(result.completed() == nullptr);
    RUVIA_CHECK(result.failedAfterCommit() == nullptr);
}

RUVIA_TEST(http1_buffered_write_body_failure_keeps_committed_status) {
    const auto result = runBufferedWrite(
        WriteScenario::kFailedAfterCommit);
    const auto* failed = result.failedAfterCommit();
    RUVIA_CHECK(failed != nullptr);
    if (failed == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(
        failed->status(),
        std::uint16_t{207});
    RUVIA_CHECK(
        failed->error() ==
        asio::error::connection_reset);
    RUVIA_CHECK(result.completed() == nullptr);
    RUVIA_CHECK(result.failedBeforeCommit() == nullptr);
}

RUVIA_TEST(http1_buffered_scatter_write_keeps_committed_status) {
    const auto result = runBufferedWrite(
        WriteScenario::kFailedAfterCommit,
        1024);
    const auto* failed = result.failedAfterCommit();
    RUVIA_CHECK(failed != nullptr);
    if (failed == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(failed->status(), std::uint16_t{207});
    RUVIA_CHECK(
        failed->error() ==
        asio::error::connection_reset);
}

RUVIA_TEST(http1_buffered_write_cannot_complete_without_a_full_head) {
    HttpResponse response(std::pmr::get_default_resource());
    response.status(207);
    const auto responsePlan = http1BufferedResponsePlan(
        httpBufferedResponseWritePlan(HttpKnownMethod::kGet, response),
        Http1ServerConnectionPlan::http11Close());
    const auto result = classifyHttp1BufferedResponseWrite(
        responsePlan,
        64,
        {},
        63);
    const auto* failed = result.failedBeforeCommit();
    RUVIA_CHECK(failed != nullptr);
    if (failed == nullptr) {
        return;
    }
    RUVIA_CHECK(failed->error() == std::errc::io_error);
    RUVIA_CHECK(result.completed() == nullptr);
    RUVIA_CHECK(result.failedAfterCommit() == nullptr);
}
