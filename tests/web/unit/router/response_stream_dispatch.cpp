#include "test_harness.h"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/coding/HttpAcceptEncoding.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/stream/HttpResponseStreamDispatch.h"
#include "ruvia/web/detail/server/stream/HttpResponseStreamSink.h"

namespace {

using ruvia::Context;
using ruvia::HttpHeaderView;
using ruvia::HttpKnownMethod;
using ruvia::HttpResponse;
using ruvia::Task;
using ruvia::detail::ControllerMiddlewareDescriptor;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::ResponseStreamCommitPlan;
using ruvia::detail::ResponseStreamDispatchResult;
using ruvia::detail::ResponseStreamFraming;
using ruvia::detail::ResponseStreamKind;
using ruvia::detail::ResponseTrailerIntent;
using ruvia::detail::RouteStreamHandler;
using ruvia::detail::HttpResponseCodingSelection;

struct BorrowTestStream final {};

struct BorrowTestScannerEntry final {
    void touch() noexcept {}
};

using Http1BorrowTestSink = ruvia::detail::ResponseStreamSink<BorrowTestStream, BorrowTestScannerEntry>;

static_assert(std::constructible_from<Http1BorrowTestSink, BorrowTestStream&, ruvia::WorkerMemory&, ruvia::detail::ResponseHeadBuffer&, BorrowTestScannerEntry&, const ruvia::WorkerHandle&, ResponseStreamKind, ruvia::detail::Http1ResponseStreamPlan, HttpResponseCodingSelection, ruvia::detail::HttpResponseCodingAvailability>);
static_assert(!std::constructible_from<Http1BorrowTestSink, BorrowTestStream&, ruvia::WorkerMemory&, ruvia::detail::ResponseHeadBuffer&, BorrowTestScannerEntry&, const ruvia::WorkerHandle&, ResponseStreamKind, ruvia::detail::Http1ResponseStreamPlan, HttpResponseCodingSelection>);
static_assert(!std::constructible_from<Http1BorrowTestSink, BorrowTestStream&, ruvia::WorkerMemory&, ruvia::detail::ResponseHeadBuffer&, BorrowTestScannerEntry&, ruvia::WorkerHandle&&, ResponseStreamKind, ruvia::detail::Http1ResponseStreamPlan, HttpResponseCodingSelection, ruvia::detail::HttpResponseCodingAvailability>);

template <typename Result>
concept HasLegacyStreamedPredicate = requires(const Result& result) {
    { result.streamed() } -> std::same_as<bool>;
};

template <typename Result>
concept HasLegacySharedResponseTake = requires(Result& result) {
    { result.takeResponse() } -> std::same_as<HttpResponse>;
};

template <typename Result>
concept HasLegacyNestedStreamOutcome = requires(const Result& result) { result.outcome(); };

template <typename Result>
concept HasAnyRvalueResponseStreamDispatchBorrow = requires(Result&& value) { std::move(value).completed(); } || requires(Result&& value) { std::move(value).peerAbortedBeforeCommit(); } || requires(Result&& value) { std::move(value).peerAbortedAfterCommit(); } || requires(Result&& value) { std::move(value).failedAfterCommit(); } || requires(Result&& value) { std::move(value).routeResponse(); } || requires(Result&& value) { std::move(value).recoveredFailure(); };

static_assert(!std::default_initializable<ResponseStreamDispatchResult>);
static_assert(!HasAnyRvalueResponseStreamDispatchBorrow<ResponseStreamDispatchResult>);
static_assert(!HasLegacyStreamedPredicate<ResponseStreamDispatchResult>);
static_assert(!HasLegacySharedResponseTake<ResponseStreamDispatchResult>);
static_assert(!HasLegacyNestedStreamOutcome<ResponseStreamDispatchResult>);
static_assert(std::same_as<decltype(std::declval<const ResponseStreamDispatchResult&>().completed()), const ruvia::detail::ResponseStreamCompleted*>);
static_assert(std::same_as<decltype(std::declval<const ResponseStreamDispatchResult&>().peerAbortedBeforeCommit()), const ruvia::detail::ResponseStreamPeerAbortedBeforeCommit*>);
static_assert(std::same_as<decltype(std::declval<const ResponseStreamDispatchResult&>().peerAbortedAfterCommit()), const ruvia::detail::ResponseStreamPeerAbortedAfterCommit*>);
static_assert(std::same_as<decltype(std::declval<const ResponseStreamDispatchResult&>().failedAfterCommit()), const ruvia::detail::ResponseStreamFailedAfterCommit*>);
static_assert(std::same_as<decltype(std::declval<ResponseStreamDispatchResult&>().routeResponse()), ruvia::detail::ResponseStreamRouteResponse*>);
static_assert(std::same_as<decltype(std::declval<ResponseStreamDispatchResult&>().recoveredFailure()), ruvia::detail::ResponseStreamRecoveredFailure*>);

class CapturingStreamSink final {
public:
    using StreamingHeadThunk = HttpResponse (*)(Context&);

    explicit CapturingStreamSink(bool failUncommittedEnd) noexcept
        : failUncommittedEnd_(failUncommittedEnd) {}

    void bindContext(Context* context, StreamingHeadThunk streamingHead) noexcept {
        context_ = context;
        streamingHead_ = streamingHead;
    }

    void releaseContext() noexcept {
        context_ = nullptr;
        streamingHead_ = nullptr;
    }

    [[nodiscard]] bool committed() const noexcept {
        return commitPlan_.has_value();
    }

    [[nodiscard]] const ResponseStreamCommitPlan* commitPlan() const noexcept {
        return commitPlan_.has_value() ? &*commitPlan_ : nullptr;
    }

    [[nodiscard]] bool aborted() const noexcept {
        return false;
    }

    Task<void> write(std::string_view chunk) {
        if (!chunk.empty()) {
            commit(ResponseTrailerIntent::kNone);
        }
        co_return;
    }

    Task<void> end(std::span<const HttpHeaderView> trailers) {
        if (failUncommittedEnd_ && !commitPlan_.has_value()) {
            throw std::runtime_error("peer aborted before the test sink committed a final head");
        }
        commit(trailers.empty() ? ResponseTrailerIntent::kNone : ResponseTrailerIntent::kPresent);
        co_return;
    }

    Task<ruvia::TimerSleepResult> sleep(std::chrono::milliseconds, const ruvia::StopToken&) {
        co_return ruvia::TimerSleepResult::kElapsed;
    }

private:
    void commit(ResponseTrailerIntent trailerIntent) {
        if (commitPlan_.has_value()) {
            return;
        }
        if (context_ == nullptr || streamingHead_ == nullptr) {
            throw std::logic_error("test response stream context is not bound");
        }
        const auto response = streamingHead_(*context_);
        commitPlan_.emplace(ruvia::detail::httpResponseStreamCommitPlan(ResponseStreamFraming::kHttp1Chunked, HttpKnownMethod::kGet, response.status(), trailerIntent));
    }

    Context* context_{nullptr};
    StreamingHeadThunk streamingHead_{nullptr};
    std::optional<ResponseStreamCommitPlan> commitPlan_;
    bool failUncommittedEnd_{false};
};

Task<void> streamWithStatus(void* target, Context& context) {
    context.status(*static_cast<const ruvia::HttpStatusCode*>(target));
    co_await context.stream().write("payload");
}

Task<void> streamWithoutCommit(void* target, Context& context) {
    context.status(*static_cast<const ruvia::HttpStatusCode*>(target));
    co_return;
}

Task<void> failAfterCommit(void* target, Context& context) {
    context.status(*static_cast<const ruvia::HttpStatusCode*>(target));
    co_await context.stream().write("partial");
    throw std::runtime_error("stream failed after commit");
}

[[nodiscard]] std::pmr::string routePath(std::string_view value) {
    return std::pmr::string(value, std::pmr::get_default_resource());
}

[[nodiscard]] ResponseStreamDispatchResult dispatchStream(RouteStreamHandler handler, bool peerAborted) {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerResponseStreamRoute(HttpKnownMethod::kGet, routePath("/stream"), handler, std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>{});
    impl.finalize();
    const auto& routes = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory requestMemory(worker);
    auto request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, "GET");
    HttpRequestAccess::setPath(request, "/stream");
    HttpRequestAccess::setResource(request, requestMemory.resource());
    const auto resolution = routes.resolve(request);
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        throw std::logic_error("test response stream route did not resolve");
    }

    CapturingStreamSink sink(peerAborted);
    asio::io_context io(1);
    std::optional<ResponseStreamDispatchResult> result;
    std::exception_ptr exception;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                result.emplace(co_await ruvia::detail::taskAsAwaitable(ruvia::detail::dispatchResponseStreamWith(sink, routes, request, *resolved, requestMemory, {}, [peerAborted]() noexcept { return peerAborted; })));
            } catch (...) {
                exception = std::current_exception();
            }
        },
        asio::detached);
    io.run();
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
    if (!result.has_value()) {
        throw std::logic_error("test response stream dispatch produced no result");
    }
    return std::move(*result);
}

}  // namespace

RUVIA_TEST(response_stream_dispatch_preserves_exact_committed_status) {
    ruvia::HttpStatusCode status = ruvia::http_status::kMultiStatus;
    auto result = dispatchStream(RouteStreamHandler(&status, &streamWithStatus), false);
    const auto* completed = result.completed();
    RUVIA_CHECK(completed != nullptr);
    if (completed != nullptr) {
        RUVIA_CHECK_EQ(completed->status(), status);
    }
    RUVIA_CHECK(result.routeResponse() == nullptr);
    RUVIA_CHECK(result.recoveredFailure() == nullptr);
}

RUVIA_TEST(response_stream_dispatch_distinguishes_precommit_peer_abort) {
    ruvia::HttpStatusCode status = ruvia::http_status::kAccepted;
    auto result = dispatchStream(RouteStreamHandler(&status, &streamWithoutCommit), true);
    RUVIA_CHECK(result.peerAbortedBeforeCommit() != nullptr);
    RUVIA_CHECK(!result.committedStatus().has_value());
}

RUVIA_TEST(response_stream_dispatch_distinguishes_committed_peer_abort) {
    ruvia::HttpStatusCode status = ruvia::http_status::kPartialContent;
    auto result = dispatchStream(RouteStreamHandler(&status, &streamWithStatus), true);
    const auto* aborted = result.peerAbortedAfterCommit();
    RUVIA_CHECK(aborted != nullptr);
    if (aborted != nullptr) {
        RUVIA_CHECK_EQ(aborted->status(), status);
    }
    RUVIA_CHECK(result.peerAbortedBeforeCommit() == nullptr);
}

RUVIA_TEST(response_stream_dispatch_end_commits_bodyless_status) {
    ruvia::HttpStatusCode status = ruvia::http_status::kNoContent;
    auto result = dispatchStream(RouteStreamHandler(&status, &streamWithoutCommit), false);
    const auto* completed = result.completed();
    RUVIA_CHECK(completed != nullptr);
    RUVIA_CHECK(result.peerAbortedBeforeCommit() == nullptr);
    RUVIA_CHECK(result.routeResponse() == nullptr);
    RUVIA_CHECK(result.recoveredFailure() == nullptr);
    if (completed != nullptr) {
        RUVIA_CHECK_EQ(completed->status(), status);
    }
}

RUVIA_TEST(response_stream_dispatch_preserves_committed_failure_status) {
    ruvia::HttpStatusCode status = ruvia::http_status::kServiceUnavailable;
    auto result = dispatchStream(RouteStreamHandler(&status, &failAfterCommit), false);
    const auto* failed = result.failedAfterCommit();
    RUVIA_CHECK(failed != nullptr);
    if (failed != nullptr) {
        RUVIA_CHECK_EQ(failed->status(), status);
    }
}

RUVIA_TEST(response_stream_dispatch_types_precommit_failure_response) {
    HttpResponse response(std::pmr::get_default_resource());
    response.status(ruvia::http_status::kBadGateway);
    auto result = ResponseStreamDispatchResult::makeRecoveredFailure(std::move(response));

    auto* recoveredFailure = result.recoveredFailure();
    RUVIA_CHECK(recoveredFailure != nullptr);
    RUVIA_CHECK(!result.committedStatus().has_value());
    RUVIA_CHECK(result.peerAbortedBeforeCommit() == nullptr);
    if (recoveredFailure != nullptr) {
        const auto recovered = std::move(*recoveredFailure).takeResponse();
        RUVIA_CHECK_EQ(recovered.status(), ruvia::http_status::kBadGateway);
    }
}
