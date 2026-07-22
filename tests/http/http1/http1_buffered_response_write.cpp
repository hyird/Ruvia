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
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <optional>
#include <random>
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
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpResponseFileAccess.h"
#include "ruvia/http/detail/http1/Http1ResponseHeadPlan.h"
#include "ruvia/http/detail/http1/Http1ServerConnectionPlan.h"
#include "ruvia/http/detail/server/HttpResponseHead.h"
#include "ruvia/http/detail/server/HttpResponseHeadBuffer.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/web/detail/server/http1/Http1BufferedResponseWrite.h"
#include "ruvia/web/detail/server/file/HttpFileFallback.h"
#include "ruvia/web/detail/server/file/HttpFileWrite.h"
#include "ruvia/web/detail/server/file/HttpNativeFile.h"
#include "ruvia/web/detail/server/HttpResponseWriter.h"

namespace {

using ruvia::HttpKnownMethod;
using ruvia::HttpResponse;
using ruvia::WorkerMemory;
using ruvia::detail::Http1BufferedResponseWriteResult;
using ruvia::detail::Http1ServerConnectionPlan;
using ruvia::detail::ResponseFileBody;
using ruvia::detail::ResponseHeadBuffer;
using ruvia::detail::appendResponseHead;
using ruvia::detail::classifyHttp1BufferedResponseWrite;
using ruvia::detail::http1BufferedResponsePlan;
using ruvia::detail::httpBufferedResponseWritePlan;
using ruvia::detail::responseBody;
using ruvia::detail::setResponseFileBody;
using ruvia::detail::taskAsAwaitable;
using ruvia::detail::writeFileFallback;
using ruvia::detail::writeHttpResponseFile;
using ruvia::detail::writeResponseWithScratch;

template <typename Alternative>
concept HasStatus = requires(const Alternative& alternative) {
    { alternative.status() } -> std::same_as<ruvia::HttpStatusCode>;
};

template <typename Alternative>
concept HasError = requires(const Alternative& alternative) {
    { alternative.error() } -> std::same_as<const std::error_code&>;
};

template <typename Result>
concept HasLegacyCompletedFlag = requires(const Result& result) {
    { result.completed() } -> std::same_as<bool>;
};

template <typename Result>
concept HasLegacyWriteOutcome = requires(const Result& result) {
    result.outcome();
};

template <typename Result>
concept ExposesAnyRvalueWriteAlternative =
    requires(const Result&& result) { std::move(result).completed(); } ||
    requires(const Result&& result) {
        std::move(result).failedBeforeCommit();
    } ||
    requires(const Result&& result) {
        std::move(result).failedAfterCommit();
    };

static_assert(!std::is_default_constructible_v<
    Http1BufferedResponseWriteResult>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1BufferedResponseWriteCompleted>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1BufferedResponseWriteFailedBeforeCommit>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1BufferedResponseWriteFailedAfterCommit>);
static_assert(!HasStatus<Http1BufferedResponseWriteResult>);
static_assert(!HasError<Http1BufferedResponseWriteResult>);
static_assert(!HasLegacyCompletedFlag<Http1BufferedResponseWriteResult>);
static_assert(!HasLegacyWriteOutcome<Http1BufferedResponseWriteResult>);
static_assert(!ExposesAnyRvalueWriteAlternative<
    Http1BufferedResponseWriteResult>);
static_assert(std::same_as<
    decltype(std::declval<const Http1BufferedResponseWriteResult&>()
                 .completed()),
    const ruvia::detail::Http1BufferedResponseWriteCompleted*>);
static_assert(std::same_as<
    decltype(std::declval<const Http1BufferedResponseWriteResult&>()
                 .failedBeforeCommit()),
    const ruvia::detail::Http1BufferedResponseWriteFailedBeforeCommit*>);
static_assert(std::same_as<
    decltype(std::declval<const Http1BufferedResponseWriteResult&>()
                 .failedAfterCommit()),
    const ruvia::detail::Http1BufferedResponseWriteFailedAfterCommit*>);
static_assert(std::same_as<
    decltype(std::declval<const Http1BufferedResponseWriteResult&>()
                 .committedStatus()),
    std::optional<ruvia::HttpStatusCode>>);
static_assert(std::is_trivially_copyable_v<
    Http1BufferedResponseWriteResult>);
static_assert(sizeof(Http1BufferedResponseWriteResult) <= 4);
static_assert(std::same_as<
    decltype(writeHttpResponseFile(
        std::declval<asio::ip::tcp::socket&>(),
        std::declval<WorkerMemory&>(),
        std::declval<std::pmr::string*>(),
        std::declval<ResponseFileBody>())),
    ruvia::Task<std::error_code>>);
static_assert(std::same_as<
    decltype(writeFileFallback(
        std::declval<asio::ip::tcp::socket&>(),
        std::declval<WorkerMemory&>(),
        std::declval<std::pmr::string*>(),
        std::declval<ResponseFileBody>())),
    ruvia::Task<std::error_code>>);

template <typename Result>
[[nodiscard]] Result runTask(
    asio::io_context& context,
    ruvia::Task<Result> task) {
    std::optional<Result> result;
    std::exception_ptr exception;
    asio::co_spawn(
        context,
        [task = std::move(task), &result, &exception]() mutable
            -> asio::awaitable<void> {
            try {
                result.emplace(co_await taskAsAwaitable(std::move(task)));
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
        throw std::logic_error("task produced no result");
    }
    return std::move(*result);
}

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

[[nodiscard]] std::filesystem::path makeTestFilePath(
    std::string_view name) {
    std::random_device entropy;
    return std::filesystem::temp_directory_path() /
        (std::string(name) + "." + std::to_string(entropy()));
}

class ScopedTestFile final {
public:
    ScopedTestFile(const char* name, std::string_view contents)
        : path_(makeTestFilePath(name)) {
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("failed to create temporary file");
        }
        output.write(
            contents.data(),
            static_cast<std::streamsize>(contents.size()));
        if (!output) {
            throw std::runtime_error("failed to write temporary file");
        }
    }

    ~ScopedTestFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    ScopedTestFile(const ScopedTestFile&) = delete;
    ScopedTestFile& operator=(const ScopedTestFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::error_code runHttpResponseFileWrite(
    ResponseFileBody fileBody) {
    asio::io_context context(1);
    asio::ip::tcp::socket socket(context);
    WorkerMemory memory;
    return runTask(
        context,
        writeHttpResponseFile(socket, memory, nullptr, fileBody));
}

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
    response.status(ruvia::http_status::kMultiStatus);
    const std::string body(bodyBytes, 'x');
    ruvia::detail::setResponseBodyBorrowedView(response, body);
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
    return runTask(
        context,
        writeResponseWithScratch(
            stream,
            memory,
            responseHead,
            nullptr,
            response,
            responsePlan));
}

[[nodiscard]] Http1BufferedResponseWriteResult runBufferedFileWrite(
    const std::filesystem::path& path,
    std::uint64_t size) {
    WorkerMemory memory;
    HttpResponse response(std::pmr::get_default_resource());
    response.status(ruvia::http_status::kMultiStatus);
    setResponseFileBody(response, path, size);
    const auto responsePlan = http1BufferedResponsePlan(
        httpBufferedResponseWritePlan(HttpKnownMethod::kGet, response),
        Http1ServerConnectionPlan::http11Close());
    ResponseHeadBuffer measuredHead(memory.allocator<char>());
    appendResponseHead(response, measuredHead, responsePlan.headPlan());
    const auto headBytes = measuredHead.view().size();
    if (headBytes == 0) {
        throw std::logic_error("HTTP/1 file response head is empty");
    }

    const std::vector<WriteStep> steps{{headBytes, {}}};
    asio::io_context context(1);
    ScriptedWriteStream stream(context, steps);
    ResponseHeadBuffer responseHead(memory.allocator<char>());
    return runTask(
        context,
        writeResponseWithScratch(
            stream,
            memory,
            responseHead,
            nullptr,
            response,
            responsePlan));
}

}  // namespace

RUVIA_TEST(http1_buffered_write_completion_owns_plan_status) {
    const auto result = runBufferedWrite(WriteScenario::kCompleted);
    RUVIA_CHECK(result.completed() != nullptr);
    RUVIA_CHECK(result.failedBeforeCommit() == nullptr);
    RUVIA_CHECK(result.failedAfterCommit() == nullptr);
    RUVIA_CHECK_EQ(result.completed()->status(), ruvia::http_status::kMultiStatus);
    RUVIA_CHECK(result.committedStatus().has_value());
    RUVIA_CHECK_EQ(
        result.committedStatus(),
        std::optional<ruvia::HttpStatusCode>{ruvia::http_status::kMultiStatus});
}

RUVIA_TEST(http1_buffered_write_partial_head_has_no_status) {
    const auto result = runBufferedWrite(
        WriteScenario::kFailedBeforeCommit);
    RUVIA_CHECK(result.completed() == nullptr);
    RUVIA_CHECK(result.failedBeforeCommit() != nullptr);
    RUVIA_CHECK(result.failedAfterCommit() == nullptr);
    RUVIA_CHECK(!result.committedStatus().has_value());
}

RUVIA_TEST(http1_buffered_write_body_failure_keeps_committed_status) {
    const auto result = runBufferedWrite(
        WriteScenario::kFailedAfterCommit);
    RUVIA_CHECK(result.completed() == nullptr);
    RUVIA_CHECK(result.failedBeforeCommit() == nullptr);
    RUVIA_CHECK(result.failedAfterCommit() != nullptr);
    RUVIA_CHECK_EQ(result.failedAfterCommit()->status(), ruvia::http_status::kMultiStatus);
    RUVIA_CHECK(result.committedStatus().has_value());
    RUVIA_CHECK_EQ(
        result.committedStatus(),
        std::optional<ruvia::HttpStatusCode>{ruvia::http_status::kMultiStatus});
}

RUVIA_TEST(http1_buffered_scatter_write_keeps_committed_status) {
    const auto result = runBufferedWrite(
        WriteScenario::kFailedAfterCommit,
        1024);
    RUVIA_CHECK(result.completed() == nullptr);
    RUVIA_CHECK(result.failedBeforeCommit() == nullptr);
    RUVIA_CHECK(result.failedAfterCommit() != nullptr);
    RUVIA_CHECK(result.committedStatus().has_value());
    RUVIA_CHECK_EQ(
        result.committedStatus(),
        std::optional<ruvia::HttpStatusCode>{ruvia::http_status::kMultiStatus});
}

RUVIA_TEST(http1_buffered_write_cannot_complete_without_a_full_head) {
    HttpResponse response(std::pmr::get_default_resource());
    response.status(ruvia::http_status::kMultiStatus);
    const auto responsePlan = http1BufferedResponsePlan(
        httpBufferedResponseWritePlan(HttpKnownMethod::kGet, response),
        Http1ServerConnectionPlan::http11Close());
    const auto result = classifyHttp1BufferedResponseWrite(
        responsePlan,
        64,
        {},
        63);
    RUVIA_CHECK(result.completed() == nullptr);
    RUVIA_CHECK(result.failedBeforeCommit() != nullptr);
    RUVIA_CHECK(result.failedAfterCommit() == nullptr);
    RUVIA_CHECK(!result.committedStatus().has_value());
}

RUVIA_TEST(http_response_file_writer_hides_native_capability) {
    const ScopedTestFile file(
        "ruvia_http_response_file_write.bin",
        "");
    HttpResponse response(std::pmr::get_default_resource());
    setResponseFileBody(response, file.path(), 0);
    const auto fileBody = responseBody(response).file();
    RUVIA_CHECK(fileBody.has_value());
    if (!fileBody.has_value()) {
        return;
    }

    RUVIA_CHECK(!runHttpResponseFileWrite(*fileBody));
}

RUVIA_TEST(http_response_file_writer_preserves_open_failure) {
    const auto path = makeTestFilePath(
        "ruvia_http_response_file_write_missing.bin");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    HttpResponse response(std::pmr::get_default_resource());
    setResponseFileBody(response, path, 1);
    const auto fileBody = responseBody(response).file();
    RUVIA_CHECK(fileBody.has_value());
    if (!fileBody.has_value()) {
        return;
    }

    RUVIA_CHECK(static_cast<bool>(runHttpResponseFileWrite(*fileBody)));
}

RUVIA_TEST(http_response_file_native_open_rejects_same_size_replacement) {
#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
    constexpr std::string_view oldContents = "old-native-body";
    constexpr std::string_view newContents = "new-native-body";
    static_assert(oldContents.size() == newContents.size());
    const ScopedTestFile original(
        "ruvia_http_response_file_identity.bin", oldContents);
    const ScopedTestFile replacement(
        "ruvia_http_response_file_identity_new.bin", newContents);

    std::error_code error;
    const auto snapshot = ruvia::detail::snapshotResponseFile(
        original.path().c_str(), error);
    RUVIA_CHECK(!error);
    RUVIA_CHECK(snapshot.identity.requiresValidation());
#if defined(_WIN32)
    std::filesystem::remove(original.path(), error);
    RUVIA_CHECK(!error);
#endif
    std::filesystem::rename(replacement.path(), original.path(), error);
    RUVIA_CHECK(!error);

    HttpResponse response(std::pmr::get_default_resource());
    setResponseFileBody(
        response,
        original.path(),
        snapshot.size,
        0,
        snapshot.size,
        snapshot.identity);
    const auto fileBody = responseBody(response).file();
    RUVIA_CHECK(fileBody.has_value());
    if (fileBody.has_value()) {
        auto input = ruvia::detail::openNativeFileForRead(*fileBody, error);
        RUVIA_CHECK(static_cast<bool>(error));
        RUVIA_CHECK_EQ(
            error,
            std::make_error_code(std::errc::state_not_recoverable));
    }
#endif
}

RUVIA_TEST(http1_buffered_file_fallback_completion_owns_status) {
    constexpr std::string_view contents = "file response body";
    const ScopedTestFile file(
        "ruvia_http_file_fallback_result.bin",
        contents);
    const auto result = runBufferedFileWrite(file.path(), contents.size());
    RUVIA_CHECK(result.completed() != nullptr);
    RUVIA_CHECK(result.failedBeforeCommit() == nullptr);
    RUVIA_CHECK(result.failedAfterCommit() == nullptr);
    RUVIA_CHECK(result.committedStatus().has_value());
    RUVIA_CHECK_EQ(
        result.committedStatus(),
        std::optional<ruvia::HttpStatusCode>{ruvia::http_status::kMultiStatus});
}

RUVIA_TEST(http1_buffered_file_open_failure_preserves_committed_status) {
    const auto path = makeTestFilePath(
        "ruvia_http_file_fallback_missing.bin");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    const auto result = runBufferedFileWrite(path, 1);
    RUVIA_CHECK(result.completed() == nullptr);
    RUVIA_CHECK(result.failedBeforeCommit() == nullptr);
    RUVIA_CHECK(result.failedAfterCommit() != nullptr);
    RUVIA_CHECK(result.committedStatus().has_value());
    RUVIA_CHECK_EQ(
        result.committedStatus(),
        std::optional<ruvia::HttpStatusCode>{ruvia::http_status::kMultiStatus});
}
