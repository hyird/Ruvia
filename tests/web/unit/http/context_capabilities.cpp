#include "test_harness.h"
#include "context_services_fixture.h"

#include "ruvia/core/Task.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/context/ContextCapabilities.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/http/SessionAccess.h"
#include "ruvia/web/detail/http/request/RequestBodyLoader.h"
#include "ruvia/web/detail/server/RequestDeadline.h"
#include "ruvia/web/detail/http/StreamingAccess.h"
#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"
#include "ruvia/web/detail/websocket/WebSocketAccess.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/Db.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/Redis.h"
#endif

#include <chrono>
#include <concepts>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <asio/io_context.hpp>

namespace {

template <typename Services>
concept HasBodyReaderAccessor = requires(const Services& services) { services.bodyReader(); };

template <typename Services>
concept HasBodyLoaderAccessor = requires(const Services& services) { services.bodyLoader(); };

static_assert(!std::constructible_from<ruvia::detail::ContextServices, const ruvia::WorkerHandle&>);
static_assert(std::constructible_from<ruvia::detail::ContextServices, const ruvia::WorkerHandle&,
    const ruvia::StopToken&>);
static_assert(!std::constructible_from<ruvia::detail::ContextServices, ruvia::WorkerHandle&&,
    const ruvia::StopToken&>);
static_assert(!std::constructible_from<ruvia::detail::ContextServices, const ruvia::WorkerHandle&,
    ruvia::StopToken&&>);
static_assert(!std::constructible_from<ruvia::detail::ContextServices,
    ruvia::detail::WorkerClientRegistryView, ruvia::detail::RateLimiter*, std::size_t>);

template <typename Services>
concept HasWebSocketAccessor = requires(const Services& services) { services.webSocket(); };

template <typename Services>
concept HasResponseStreamAccessor =
    requires(const Services& services) { services.responseStream(); };

template <typename Services>
concept HasWithBodyReader = requires(
    const Services& services, ruvia::BodyReader& reader) { services.withBodyReader(reader); };

template <typename Services>
concept HasWithBodyLoader = requires(const Services& services,
    ruvia::detail::RequestBodyLoader& loader) { services.withBodyLoader(loader); };

template <typename Services>
concept HasWorkerRefinement = requires(
    const Services& services, const ruvia::WorkerHandle& worker) { services.withWorker(worker); };

template <typename Services>
concept AcceptsTemporaryEnv =
    requires(const Services& services, ruvia::Env&& env) { services.withEnv(std::move(env)); };

template <typename Services>
concept AcceptsTemporaryRoutes = requires(const Services& services,
    ruvia::detail::RouteTable&& routes) { services.withRoutes(std::move(routes)); };

template <typename Services>
concept AcceptsTemporaryWorkerStates = requires(const Services& services,
    ruvia::detail::WorkerStateRegistry&& states) { services.withWorkerStates(std::move(states)); };

template <typename Access>
concept MakesContextWithoutServices = requires(ruvia::RequestMemory& memory,
    const ruvia::HttpRequest& request) { Access::make(memory, request); };

template <typename Source>
concept ExposesRvalueRequestBodyAlternative =
    requires(Source&& source) { std::move(source).buffered(); } || requires(Source&& source) {
        std::move(source).lazy();
    } || requires(Source&& source) { std::move(source).streaming(); };

template <typename Output>
concept ExposesRvalueResponseOutputAlternative =
    requires(Output&& output) { std::move(output).buffered(); } || requires(Output&& output) {
        std::move(output).responseStream();
    } || requires(Output&& output) { std::move(output).webSocket(); };

static_assert(!HasBodyReaderAccessor<ruvia::detail::ContextServices>);
static_assert(!HasBodyLoaderAccessor<ruvia::detail::ContextServices>);
static_assert(!HasWebSocketAccessor<ruvia::detail::ContextServices>);
static_assert(!HasResponseStreamAccessor<ruvia::detail::ContextServices>);
static_assert(!HasWithBodyReader<ruvia::detail::ContextServices>);
static_assert(!HasWithBodyLoader<ruvia::detail::ContextServices>);
static_assert(!HasWorkerRefinement<ruvia::detail::ContextServices>);
static_assert(!AcceptsTemporaryEnv<ruvia::detail::ContextServices>);
static_assert(!AcceptsTemporaryRoutes<ruvia::detail::ContextServices>);
static_assert(!AcceptsTemporaryWorkerStates<ruvia::detail::ContextServices>);
static_assert(!std::default_initializable<ruvia::detail::ContextServices>);
static_assert(!MakesContextWithoutServices<ruvia::detail::ContextAccess>);
static_assert(!ExposesRvalueRequestBodyAlternative<ruvia::detail::ContextRequestBodySource>);
static_assert(!ExposesRvalueResponseOutputAlternative<ruvia::detail::ContextResponseOutput>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::detail::ContextServices&>().requestBodySource()),
    const ruvia::detail::ContextRequestBodySource&>);
static_assert(
    std::is_same_v<decltype(std::declval<const ruvia::detail::ContextServices&>().responseOutput()),
        const ruvia::detail::ContextResponseOutput&>);
static_assert(
    std::is_same_v<decltype(std::declval<const ruvia::detail::ContextServices&>().worker()),
        const ruvia::WorkerHandle&>);
static_assert(std::is_same_v<decltype(std::declval<const ruvia::Context&>().worker()),
    const ruvia::WorkerHandle&>);
static_assert(
    std::is_same_v<decltype(std::declval<const ruvia::detail::ContextServices&>().stopToken()),
        const ruvia::StopToken&>);
static_assert(
    std::is_same_v<decltype(std::declval<const ruvia::Context&>().stopToken()), ruvia::StopToken>);
static_assert(std::is_nothrow_copy_constructible_v<ruvia::detail::ContextRequestBodySource>);
static_assert(std::is_nothrow_copy_assignable_v<ruvia::detail::ContextRequestBodySource>);
static_assert(std::is_nothrow_copy_constructible_v<ruvia::detail::ContextResponseOutput>);
static_assert(std::is_nothrow_copy_assignable_v<ruvia::detail::ContextResponseOutput>);
static_assert(!std::is_default_constructible_v<ruvia::detail::ContextLazyRequestBodySource>);
static_assert(!std::is_default_constructible_v<ruvia::detail::ContextStreamingRequestBodySource>);
static_assert(!std::is_default_constructible_v<ruvia::detail::ContextResponseStreamOutput>);
static_assert(!std::is_default_constructible_v<ruvia::detail::ContextWebSocketOutput>);

ruvia::Task<std::string_view> loadBody(void*) {
    co_return "lazy-body";
}

ruvia::Task<void> discardBody(void*) {
    co_return;
}

ruvia::Task<std::optional<std::string_view>> readBody(void*) {
    co_return std::nullopt;
}

struct OutputSink final {
    std::pmr::string scratch{std::pmr::get_default_resource()};
};

ruvia::Task<void> writeOutput(void*, std::string_view) {
    co_return;
}

ruvia::Task<void> endOutput(void*, std::span<const ruvia::HttpHeaderView>) {
    co_return;
}

ruvia::Task<ruvia::TimerSleepResult> sleepOutput(
    void*, std::chrono::milliseconds, const ruvia::StopToken&) {
    co_return ruvia::TimerSleepResult::kElapsed;
}

void bindOutput(void*, ruvia::Context*, ruvia::HttpResponse (*)(ruvia::Context&)) noexcept {}

bool outputFalse(void*) noexcept {
    return false;
}

void releaseOutputContext(void*) noexcept {}

ruvia::ResponseStreamWriter makeResponseStreamWriter(OutputSink& sink) noexcept {
    return ruvia::detail::StreamingAccess::makeResponseStreamWriter(&sink, &writeOutput, &endOutput,
        &sleepOutput, &bindOutput, &releaseOutputContext, &outputFalse, &outputFalse);
}

ruvia::Task<std::optional<ruvia::WebSocketMessage>> readWebSocket(void*) {
    co_return std::nullopt;
}

ruvia::Task<void> writeWebSocket(void*, ruvia::WebSocketOpcode, std::string_view) {
    co_return;
}

ruvia::Task<void> closeWebSocket(void*, ruvia::WebSocketCloseOptions) {
    co_return;
}

ruvia::HttpRequest makeRequest(std::pmr::memory_resource* resource) {
    auto request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setResource(request, resource);
    return request;
}

struct BoundBodyReader final {
    explicit BoundBodyReader(int value) noexcept
        : value(value) {}

    ruvia::Task<std::optional<std::string_view>> read() {
        co_return std::nullopt;
    }

    int value;
};

struct BoundBodyLoader final {
    explicit BoundBodyLoader(int value) noexcept
        : value(value) {}

    ruvia::Task<std::string_view> readAll() {
        co_return std::string_view{};
    }
    ruvia::Task<void> discard() {
        co_return;
    }

    int value;
};

}  // namespace

RUVIA_TEST(request_body_capability_binding_constructs_target_and_facade_atomically) {
    ruvia::detail::BodyReaderBinding<BoundBodyReader> reader(17);
    ruvia::detail::RequestBodyLoaderBinding<BoundBodyLoader> loader(23);

    static_assert(!std::is_move_constructible_v<decltype(reader)>);
    static_assert(!std::is_move_constructible_v<decltype(loader)>);
    RUVIA_CHECK_EQ(reader.reader().value, 17);
    RUVIA_CHECK_EQ(loader.loader().value, 23);

    const auto base = ruvia::test::testContextServices();
    const auto streaming = base.withStreamingRequestBody(reader.facade());
    const auto lazy = base.withLazyRequestBody(loader.facade());
    RUVIA_CHECK(&streaming.requestBodySource().streaming()->reader() == &reader.facade());
    RUVIA_CHECK(&lazy.requestBodySource().lazy()->loader() == &loader.facade());
}

RUVIA_TEST(context_request_body_source_has_one_active_alternative) {
    ruvia::detail::RequestBodyLoader loader(nullptr, &loadBody, &discardBody);
    std::optional<ruvia::BodyReader> reader;
    ruvia::detail::StreamingAccess::emplaceBodyReader(reader, nullptr, &readBody);

    const auto base = ruvia::test::testContextServices();
    RUVIA_CHECK(base.requestBodySource().buffered() != nullptr);
    RUVIA_CHECK(base.requestBodySource().lazy() == nullptr);
    RUVIA_CHECK(base.requestBodySource().streaming() == nullptr);

    const auto lazy = base.withLazyRequestBody(loader);
    RUVIA_CHECK(lazy.requestBodySource().buffered() == nullptr);
    RUVIA_CHECK(lazy.requestBodySource().lazy() != nullptr);
    RUVIA_CHECK(lazy.requestBodySource().streaming() == nullptr);
    RUVIA_CHECK(&lazy.requestBodySource().lazy()->loader() == &loader);

    const auto streaming = lazy.withStreamingRequestBody(*reader);
    RUVIA_CHECK(streaming.requestBodySource().buffered() == nullptr);
    RUVIA_CHECK(streaming.requestBodySource().lazy() == nullptr);
    RUVIA_CHECK(streaming.requestBodySource().streaming() != nullptr);
    RUVIA_CHECK(&streaming.requestBodySource().streaming()->reader() == &*reader);

    // Functional service refinement must not mutate either earlier value.
    RUVIA_CHECK(base.requestBodySource().buffered() != nullptr);
    RUVIA_CHECK(lazy.requestBodySource().lazy() != nullptr);
}

RUVIA_TEST(context_services_borrows_address_stable_worker_and_stop_token) {
    asio::io_context ioContext;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
    const auto handle = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    const ruvia::StopToken stopToken;
    const ruvia::detail::ContextServices services(handle, stopToken);
    const auto derived = services.withPlainTransport("127.0.0.1");
    RUVIA_CHECK(&services.worker() == &handle);
    RUVIA_CHECK(&derived.worker() == &handle);
    RUVIA_CHECK(&services.stopToken() == &stopToken);
    RUVIA_CHECK(&derived.stopToken() == &stopToken);
}

RUVIA_TEST(context_services_rejects_an_invalid_worker_binding) {
    const ruvia::WorkerHandle worker;
    const ruvia::StopToken stopToken;
    bool rejected = false;
    try {
        const ruvia::detail::ContextServices services(worker, stopToken);
        static_cast<void>(services);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(context_rejects_unconfigured_worker_clients_consistently) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = makeRequest(memory.resource());
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());

    bool httpClientRejected = false;
    try {
        static_cast<void>(context.httpClient());
    } catch (const ruvia::HttpClientError& error) {
        httpClientRejected = error.code() == ruvia::HttpClientError::Code::kNotConfigured;
    }
    RUVIA_CHECK(httpClientRejected);

#ifdef RUVIA_ENABLE_DATABASE
    bool databaseRejected = false;
    try {
        static_cast<void>(context.db());
    } catch (const ruvia::DbError& error) {
        databaseRejected = error.code() == ruvia::DbError::Code::kNotConfigured;
    }
    RUVIA_CHECK(databaseRejected);
#endif

#ifdef RUVIA_ENABLE_REDIS
    bool redisRejected = false;
    try {
        static_cast<void>(context.redis());
    } catch (const ruvia::RedisError& error) {
        redisRejected = error.code() == ruvia::RedisError::Code::kNotConfigured;
    }
    RUVIA_CHECK(redisRejected);
#endif
}

RUVIA_TEST(context_session_capability_requires_explicit_middleware_binding) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = makeRequest(memory.resource());
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());

    RUVIA_CHECK(!context.trySession().has_value());
    bool rejected = false;
    try {
        static_cast<void>(context.session());
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);

    ruvia::detail::SessionAccess::bind(context);
    auto session = context.session();
    session.set("user=42");
    RUVIA_CHECK_EQ(session.data(), std::string_view("user=42"));
    RUVIA_CHECK(context.trySession().has_value());
}

RUVIA_TEST(context_exposes_the_server_shutdown_stop_token) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = makeRequest(memory.resource());
    ruvia::StopSource source;
    const auto token = source.token();
    const ruvia::detail::ContextServices services(ruvia::test::testWorkerHandle(), token);
    const auto context = ruvia::detail::ContextAccess::make(memory, request, services);

    RUVIA_CHECK(context.stopToken().stoppable());
    RUVIA_CHECK(!context.stopToken().stopRequested());
    source.requestStop();
    RUVIA_CHECK(context.stopToken().stopRequested());
}

RUVIA_TEST(context_services_bind_request_deadline_and_stop_token_atomically) {
    ruvia::StopSource workerStop;
    const auto workerToken = workerStop.token();
    const ruvia::detail::ContextServices base(ruvia::test::testWorkerHandle(), workerToken);
    ruvia::detail::RequestDeadline deadline(workerToken);

    const auto request = base.withRequestDeadline(deadline);

    RUVIA_CHECK(request.requestDeadline() == &deadline);
    RUVIA_CHECK(&request.stopToken() == &deadline.token());
    RUVIA_CHECK(base.requestDeadline() == nullptr);
    RUVIA_CHECK(&base.stopToken() == &workerToken);
}

RUVIA_TEST(context_response_output_has_one_active_alternative) {
    OutputSink sink;
    auto writer = makeResponseStreamWriter(sink);
    auto webSocket = ruvia::detail::WebSocketAccess::make(
        nullptr, &readWebSocket, &writeWebSocket, &closeWebSocket);

    const auto base = ruvia::test::testContextServices();
    RUVIA_CHECK(base.responseOutput().buffered() != nullptr);
    RUVIA_CHECK(base.responseOutput().responseStream() == nullptr);
    RUVIA_CHECK(base.responseOutput().webSocket() == nullptr);

    const auto streaming = base.withResponseStream(writer);
    RUVIA_CHECK(streaming.responseOutput().buffered() == nullptr);
    RUVIA_CHECK(streaming.responseOutput().responseStream() != nullptr);
    RUVIA_CHECK(streaming.responseOutput().webSocket() == nullptr);
    RUVIA_CHECK(&streaming.responseOutput().responseStream()->writer() == &writer);

    const auto webSocketOutput = ruvia::detail::ContextResponseOutput::webSocket(webSocket);
    RUVIA_CHECK(webSocketOutput.buffered() == nullptr);
    RUVIA_CHECK(webSocketOutput.responseStream() == nullptr);
    RUVIA_CHECK(webSocketOutput.webSocket() != nullptr);
    RUVIA_CHECK(&webSocketOutput.webSocket()->webSocket() == &webSocket);

    RUVIA_CHECK(base.responseOutput().buffered() != nullptr);
    RUVIA_CHECK(streaming.responseOutput().responseStream() != nullptr);
}

RUVIA_TEST(context_copies_typed_capabilities_into_public_facades) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = makeRequest(memory.resource());

    std::optional<ruvia::BodyReader> reader;
    ruvia::detail::StreamingAccess::emplaceBodyReader(reader, nullptr, &readBody);
    auto bodyContext = ruvia::detail::ContextAccess::make(
        memory, request, ruvia::test::testContextServices().withStreamingRequestBody(*reader));
    RUVIA_CHECK(&bodyContext.req().bodyReader() == &*reader);

    OutputSink sink;
    auto writer = makeResponseStreamWriter(sink);
    auto streamContext = ruvia::detail::ContextAccess::make(
        memory, request, ruvia::test::testContextServices().withResponseStream(writer));
    RUVIA_CHECK(&streamContext.stream() == &writer);
    (void)streamContext.streamSse();
    const auto sseHead = ruvia::detail::ContextAccess::streamingHead(streamContext);
    RUVIA_CHECK_EQ(sseHead.header("Content-Type"), std::string_view("text/event-stream"));
    RUVIA_CHECK_EQ(sseHead.header("Cache-Control"), std::string_view("no-cache"));

    auto webSocket = ruvia::detail::WebSocketAccess::make(
        nullptr, &readWebSocket, &writeWebSocket, &closeWebSocket);
    auto webSocketContext =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    {
        ruvia::detail::ContextWebSocketBinding binding(webSocketContext, webSocket);
        RUVIA_CHECK(&webSocketContext.webSocket() == &webSocket);
    }
    bool unavailableAfterScope = false;
    try {
        (void)webSocketContext.webSocket();
    } catch (const std::logic_error&) {
        unavailableAfterScope = true;
    }
    RUVIA_CHECK(unavailableAfterScope);
}

RUVIA_TEST(context_websocket_binding_restores_capability_during_unwind) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = makeRequest(memory.resource());
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    auto webSocket = ruvia::detail::WebSocketAccess::make(
        nullptr, &readWebSocket, &writeWebSocket, &closeWebSocket);

    try {
        ruvia::detail::ContextWebSocketBinding binding(context, webSocket);
        RUVIA_CHECK(&context.webSocket() == &webSocket);
        throw std::runtime_error("leave websocket scope");
    } catch (const std::runtime_error&) {
    }

    bool unavailableAfterUnwind = false;
    try {
        (void)context.webSocket();
    } catch (const std::logic_error&) {
        unavailableAfterUnwind = true;
    }
    RUVIA_CHECK(unavailableAfterUnwind);
}

RUVIA_TEST(context_request_exposes_matched_route_path) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = makeRequest(memory.resource());
    auto context = ruvia::detail::ContextAccess::make(
        memory, request, "/items/:id", 0, ruvia::test::testContextServices());

    const auto facade = context.req();
    RUVIA_CHECK_EQ(facade.routePath(), std::string_view("/items/:id"));
}

RUVIA_TEST(context_lazy_request_caches_share_one_typed_storage_owner) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = makeRequest(memory.resource());
    ruvia::detail::HttpRequestAccess::setQueryString(request, "name=ruvia&name=web");
    RUVIA_CHECK(ruvia::detail::HttpRequestAccess::addHeader(
        request, ruvia::HttpHeaderView("Cookie", "theme=dark")));

    const std::string_view names[]{"id"};
    const std::string_view values[]{"42"};
    auto context = ruvia::detail::ContextAccess::make(
        memory, request, "/items/:id", names, values, 1, 0, ruvia::test::testContextServices());
    RUVIA_CHECK(ruvia::detail::ContextAccess::requestStorage(context) == nullptr);

    (void)context.req().headerFields();
    const auto* const owner = ruvia::detail::ContextAccess::requestStorage(context);
    RUVIA_CHECK(owner != nullptr);
    (void)context.req().queryFields();
    (void)context.req().cookieFields();
    (void)context.req().paramFields();
    RUVIA_CHECK(ruvia::detail::ContextAccess::requestStorage(context) == owner);
}
