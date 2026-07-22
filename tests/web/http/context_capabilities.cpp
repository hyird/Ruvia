#include "test_harness.h"

#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/context/ContextCapabilities.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/http/request/RequestBodyLoader.h"
#include "ruvia/web/detail/http/StreamingAccess.h"
#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"
#include "ruvia/web/detail/websocket/WebSocketAccess.h"

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

template <typename Services>
concept HasBodyReaderAccessor = requires(const Services& services) {
    services.bodyReader();
};

template <typename Services>
concept HasBodyLoaderAccessor = requires(const Services& services) {
    services.bodyLoader();
};

template <typename Services>
concept HasWebSocketAccessor = requires(const Services& services) {
    services.webSocket();
};

template <typename Services>
concept HasResponseStreamAccessor = requires(const Services& services) {
    services.responseStream();
};

template <typename Services>
concept HasWithBodyReader = requires(
    const Services& services,
    ruvia::BodyReader& reader) {
    services.withBodyReader(reader);
};

template <typename Services>
concept HasWithBodyLoader = requires(
    const Services& services,
    ruvia::detail::RequestBodyLoader& loader) {
    services.withBodyLoader(loader);
};

template <typename Source>
concept ExposesRvalueRequestBodyAlternative =
    requires(Source&& source) { std::move(source).buffered(); } ||
    requires(Source&& source) { std::move(source).lazy(); } ||
    requires(Source&& source) { std::move(source).streaming(); };

template <typename Output>
concept ExposesRvalueResponseOutputAlternative =
    requires(Output&& output) { std::move(output).buffered(); } ||
    requires(Output&& output) { std::move(output).responseStream(); } ||
    requires(Output&& output) { std::move(output).webSocket(); };

static_assert(!HasBodyReaderAccessor<ruvia::detail::ContextServices>);
static_assert(!HasBodyLoaderAccessor<ruvia::detail::ContextServices>);
static_assert(!HasWebSocketAccessor<ruvia::detail::ContextServices>);
static_assert(!HasResponseStreamAccessor<ruvia::detail::ContextServices>);
static_assert(!HasWithBodyReader<ruvia::detail::ContextServices>);
static_assert(!HasWithBodyLoader<ruvia::detail::ContextServices>);
static_assert(!ExposesRvalueRequestBodyAlternative<
    ruvia::detail::ContextRequestBodySource>);
static_assert(!ExposesRvalueResponseOutputAlternative<
    ruvia::detail::ContextResponseOutput>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::detail::ContextServices&>()
                 .requestBodySource()),
    const ruvia::detail::ContextRequestBodySource&>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::detail::ContextServices&>()
                 .responseOutput()),
    const ruvia::detail::ContextResponseOutput&>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::detail::ContextServices&>().worker()),
    const ruvia::WorkerHandle&>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::Context&>().worker()),
    const ruvia::WorkerHandle&>);
static_assert(std::is_nothrow_copy_constructible_v<
    ruvia::detail::ContextRequestBodySource>);
static_assert(std::is_nothrow_copy_assignable_v<
    ruvia::detail::ContextRequestBodySource>);
static_assert(std::is_nothrow_copy_constructible_v<
    ruvia::detail::ContextResponseOutput>);
static_assert(std::is_nothrow_copy_assignable_v<
    ruvia::detail::ContextResponseOutput>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::ContextLazyRequestBodySource>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::ContextStreamingRequestBodySource>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::ContextResponseStreamOutput>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::ContextWebSocketOutput>);

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

ruvia::Task<void> endOutput(
    void*,
    std::span<const ruvia::HttpHeaderView>) {
    co_return;
}

ruvia::Task<void> sleepOutput(void*, std::chrono::milliseconds) {
    co_return;
}

void bindOutput(
    void*,
    ruvia::Context*,
    ruvia::HttpResponse (*)(ruvia::Context&)) noexcept {}

bool outputFalse(void*) noexcept {
    return false;
}

void releaseOutputContext(void*) noexcept {}

ruvia::ResponseStreamWriter makeResponseStreamWriter(OutputSink& sink) noexcept {
    return ruvia::detail::StreamingAccess::makeResponseStreamWriter(
        &sink,
        &writeOutput,
        &endOutput,
        &sleepOutput,
        &bindOutput,
        &releaseOutputContext,
        &outputFalse,
        &outputFalse);
}

ruvia::Task<std::optional<ruvia::WebSocketMessage>> readWebSocket(void*) {
    co_return std::nullopt;
}

ruvia::Task<void> writeWebSocket(
    void*,
    ruvia::WebSocketOpcode,
    std::string_view) {
    co_return;
}

ruvia::Task<void> closeWebSocket(
    void*,
    std::uint16_t,
    std::string_view) {
    co_return;
}

ruvia::HttpRequest makeRequest(std::pmr::memory_resource* resource) {
    auto request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setResource(request, resource);
    return request;
}

struct BoundBodyReader final {
    explicit BoundBodyReader(int value) noexcept : value(value) {}

    ruvia::Task<std::optional<std::string_view>> read() {
        co_return std::nullopt;
    }

    int value;
};

struct BoundBodyLoader final {
    explicit BoundBodyLoader(int value) noexcept : value(value) {}

    ruvia::Task<std::string_view> readAll() { co_return std::string_view{}; }
    ruvia::Task<void> discard() { co_return; }

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

    const ruvia::detail::ContextServices base;
    const auto streaming = base.withStreamingRequestBody(reader.facade());
    const auto lazy = base.withLazyRequestBody(loader.facade());
    RUVIA_CHECK(
        &streaming.requestBodySource().streaming()->reader() ==
        &reader.facade());
    RUVIA_CHECK(
        &lazy.requestBodySource().lazy()->loader() ==
        &loader.facade());
}

RUVIA_TEST(context_request_body_source_has_one_active_alternative) {
    ruvia::detail::RequestBodyLoader loader(
        nullptr, &loadBody, &discardBody);
    std::optional<ruvia::BodyReader> reader;
    ruvia::detail::StreamingAccess::emplaceBodyReader(
        reader, nullptr, &readBody);

    const ruvia::detail::ContextServices base;
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
    RUVIA_CHECK(
        &streaming.requestBodySource().streaming()->reader() == &*reader);

    // Functional service refinement must not mutate either earlier value.
    RUVIA_CHECK(base.requestBodySource().buffered() != nullptr);
    RUVIA_CHECK(lazy.requestBodySource().lazy() != nullptr);
}

RUVIA_TEST(context_services_borrows_address_stable_worker) {
    const ruvia::WorkerHandle handle;
    const ruvia::detail::ContextServices services(
        nullptr, nullptr, nullptr, ruvia::kDefaultMaxBufferedBodyBytes, &handle);
    const auto derived = services.withPlainTransport("127.0.0.1");
    RUVIA_CHECK(&services.worker() == &handle);
    RUVIA_CHECK(&derived.worker() == &handle);
}

RUVIA_TEST(context_response_output_has_one_active_alternative) {
    OutputSink sink;
    auto writer = makeResponseStreamWriter(sink);
    auto webSocket = ruvia::detail::WebSocketAccess::make(
        nullptr, &readWebSocket, &writeWebSocket, &closeWebSocket);

    const ruvia::detail::ContextServices base;
    RUVIA_CHECK(base.responseOutput().buffered() != nullptr);
    RUVIA_CHECK(base.responseOutput().responseStream() == nullptr);
    RUVIA_CHECK(base.responseOutput().webSocket() == nullptr);

    const auto streaming = base.withResponseStream(writer);
    RUVIA_CHECK(streaming.responseOutput().buffered() == nullptr);
    RUVIA_CHECK(streaming.responseOutput().responseStream() != nullptr);
    RUVIA_CHECK(streaming.responseOutput().webSocket() == nullptr);
    RUVIA_CHECK(
        &streaming.responseOutput().responseStream()->writer() == &writer);

    const auto webSocketOutput =
        ruvia::detail::ContextResponseOutput::webSocket(webSocket);
    RUVIA_CHECK(webSocketOutput.buffered() == nullptr);
    RUVIA_CHECK(webSocketOutput.responseStream() == nullptr);
    RUVIA_CHECK(webSocketOutput.webSocket() != nullptr);
    RUVIA_CHECK(
        &webSocketOutput.webSocket()->webSocket() ==
        &webSocket);

    RUVIA_CHECK(base.responseOutput().buffered() != nullptr);
    RUVIA_CHECK(streaming.responseOutput().responseStream() != nullptr);
}

RUVIA_TEST(context_copies_typed_capabilities_into_public_facades) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = makeRequest(memory.resource());

    std::optional<ruvia::BodyReader> reader;
    ruvia::detail::StreamingAccess::emplaceBodyReader(
        reader, nullptr, &readBody);
    auto bodyContext = ruvia::detail::ContextAccess::make(
        memory,
        request,
        ruvia::detail::ContextServices{}.withStreamingRequestBody(*reader));
    RUVIA_CHECK(&bodyContext.req().bodyReader() == &*reader);

    OutputSink sink;
    auto writer = makeResponseStreamWriter(sink);
    auto streamContext = ruvia::detail::ContextAccess::make(
        memory,
        request,
        ruvia::detail::ContextServices{}.withResponseStream(writer));
    RUVIA_CHECK(&streamContext.stream() == &writer);
    (void)streamContext.streamSse();
    const auto sseHead = ruvia::detail::ContextAccess::streamingHead(streamContext);
    RUVIA_CHECK_EQ(sseHead.header("Content-Type"), std::string_view("text/event-stream"));
    RUVIA_CHECK_EQ(sseHead.header("Cache-Control"), std::string_view("no-cache"));

    auto webSocket = ruvia::detail::WebSocketAccess::make(
        nullptr, &readWebSocket, &writeWebSocket, &closeWebSocket);
    auto webSocketContext = ruvia::detail::ContextAccess::make(
        memory,
        request);
    {
        ruvia::detail::ContextWebSocketBinding binding(
            webSocketContext,
            webSocket);
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
    auto context = ruvia::detail::ContextAccess::make(memory, request);
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
        memory,
        request,
        "/items/:id",
        0);

    const auto facade = context.req();
    RUVIA_CHECK_EQ(facade.routePath(), std::string_view("/items/:id"));
}

RUVIA_TEST(context_lazy_request_caches_share_one_typed_storage_owner) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = makeRequest(memory.resource());
    ruvia::detail::HttpRequestAccess::setQueryString(
        request, "name=ruvia&name=web");
    RUVIA_CHECK(ruvia::detail::HttpRequestAccess::addHeader(
        request, ruvia::HttpHeaderView("Cookie", "theme=dark")));

    const std::string_view names[]{"id"};
    const std::string_view values[]{"42"};
    auto context = ruvia::detail::ContextAccess::make(
        memory,
        request,
        "/items/:id",
        names,
        values,
        1,
        0);
    RUVIA_CHECK(
        ruvia::detail::ContextAccess::requestStorage(context) == nullptr);

    (void)ruvia::detail::requestHeaderFields(context.req());
    const auto* const owner =
        ruvia::detail::ContextAccess::requestStorage(context);
    RUVIA_CHECK(owner != nullptr);
    (void)ruvia::detail::requestQueryFields(context.req());
    (void)ruvia::detail::requestCookieFields(context.req());
    (void)ruvia::detail::requestParamFields(context.req());
    RUVIA_CHECK(
        ruvia::detail::ContextAccess::requestStorage(context) == owner);
}
