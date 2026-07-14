#include "test_harness.h"

#include "ruvia/core/Task.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/ContextCapabilities.h"
#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/web/detail/http/ContextServices.h"
#include "ruvia/web/detail/http/RequestBodyLoader.h"
#include "ruvia/web/detail/http/StreamingInternal.h"
#include "ruvia/web/detail/websocket/WebSocketInternal.h"
#include "ruvia/web/detail/ContextValues.h"

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
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

static_assert(!HasBodyReaderAccessor<ruvia::detail::ContextServices>);
static_assert(!HasBodyLoaderAccessor<ruvia::detail::ContextServices>);
static_assert(!HasWebSocketAccessor<ruvia::detail::ContextServices>);
static_assert(!HasResponseStreamAccessor<ruvia::detail::ContextServices>);
static_assert(!HasWithBodyReader<ruvia::detail::ContextServices>);
static_assert(!HasWithBodyLoader<ruvia::detail::ContextServices>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::detail::ContextServices&>()
                 .requestBodySource()),
    const ruvia::detail::ContextRequestBodySource&>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::detail::ContextServices&>()
                 .responseOutput()),
    const ruvia::detail::ContextResponseOutput&>);
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

struct TrackedContextValue final {
    TrackedContextValue(int& live, int& destroyed, int value) noexcept
        : live_(&live), destroyed_(&destroyed), value(value) {
        ++*live_;
    }

    TrackedContextValue(const TrackedContextValue&) = delete;
    TrackedContextValue& operator=(const TrackedContextValue&) = delete;

    ~TrackedContextValue() {
        --*live_;
        ++*destroyed_;
    }

    int* live_;
    int* destroyed_;
    int value;
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

std::pmr::string& outputScratch(void* target) noexcept {
    return static_cast<OutputSink*>(target)->scratch;
}

bool outputFalse(void*) noexcept {
    return false;
}

ruvia::ResponseStreamWriter makeResponseStreamWriter(OutputSink& sink) noexcept {
    return ruvia::detail::StreamingAccess::makeResponseStreamWriter(
        &sink,
        &writeOutput,
        &endOutput,
        &sleepOutput,
        &bindOutput,
        &outputScratch,
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

}  // namespace

RUVIA_TEST(context_value_store_transfers_entry_ownership_without_assignment) {
    std::pmr::monotonic_buffer_resource resource;
    int live = 0;
    int destroyed = 0;
    {
        ruvia::detail::ContextValueStore values(&resource);
        values.setAs<TrackedContextValue>("same", live, destroyed, 1);
        RUVIA_CHECK_EQ(live, 1);

        auto& replacement = values.setAs<TrackedContextValue>(
            "same", live, destroyed, 2);
        RUVIA_CHECK_EQ(replacement.value, 2);
        RUVIA_CHECK_EQ(live, 1);
        RUVIA_CHECK_EQ(destroyed, 1);

        for (int i = 0; i < 32; ++i) {
            const auto name = std::to_string(i);
            values.setAs<TrackedContextValue>(name, live, destroyed, i);
        }
        RUVIA_CHECK_EQ(live, 33);
        RUVIA_CHECK_EQ(destroyed, 1);
        RUVIA_CHECK_EQ(values.get<TrackedContextValue>("same").value, 2);
    }
    RUVIA_CHECK_EQ(live, 0);
    RUVIA_CHECK_EQ(destroyed, 34);
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

    const auto webSocketServices = streaming.withWebSocket(webSocket);
    RUVIA_CHECK(webSocketServices.responseOutput().buffered() == nullptr);
    RUVIA_CHECK(webSocketServices.responseOutput().responseStream() == nullptr);
    RUVIA_CHECK(webSocketServices.responseOutput().webSocket() != nullptr);
    RUVIA_CHECK(
        &webSocketServices.responseOutput().webSocket()->webSocket() ==
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
        request,
        ruvia::detail::ContextServices{}.withWebSocket(webSocket));
    RUVIA_CHECK(&webSocketContext.webSocket() == &webSocket);
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
