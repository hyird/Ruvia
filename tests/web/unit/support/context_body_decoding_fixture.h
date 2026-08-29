#pragma once

#include "content_decoding_fixture.h"
#include "context_services_fixture.h"
#include "test_harness.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http/context/ContextServices.h"

namespace context_body_decoding_test {

struct ContextBodyReadObservation final {
    std::string body;
    std::optional<ruvia::HttpStatusCode> errorStatus;
};

inline ruvia::Task<std::string_view> readContextText(ruvia::Context& context) {
    co_return co_await context.req().text();
}

inline ruvia::ScopedOperation<std::string_view> makeExpiredContextTextRead() {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
    ruvia::detail::HttpRequestAccess::setBody(request, "body");
    auto context = ruvia::detail::ContextAccess::make(
        memory, request, ruvia::test::testContextServices().withMaxDecodedBodyBytes(1024));
    return context.req().text();
}

inline ruvia::Task<void> awaitExpiredContextTextRead(
    ruvia::ScopedOperation<std::string_view>& operation, bool& rejected) {
    try {
        (void)co_await std::move(operation);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

inline ContextBodyReadObservation readContextGzipBody(
    std::string_view encoded, std::size_t maxDecodedBodyBytes) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
    const auto contentEncodingSlot = ruvia::detail::HttpRequestAccess::knownHeaderSlot(
        ruvia::detail::RequestKnownHeader::kContentEncoding);
    if (!ruvia::detail::HttpRequestAccess::addHeader(
            request, ruvia::HttpHeaderView{"Content-Encoding", "gzip"}, contentEncodingSlot)) {
        throw std::runtime_error("test request rejected Content-Encoding");
    }
    ruvia::detail::HttpRequestAccess::setBody(request, encoded);

    auto context = ruvia::detail::ContextAccess::make(memory, request,
        ruvia::test::testContextServices().withMaxDecodedBodyBytes(maxDecodedBodyBytes));
    asio::io_context io(1);
    auto future = asio::co_spawn(
        io, ruvia::detail::taskAsAwaitable(readContextText(context)), asio::use_future);
    io.run();

    ContextBodyReadObservation observation;
    try {
        observation.body = future.get();
    } catch (const ruvia::HttpProtocolError& error) {
        observation.errorStatus = error.status();
    }
    return observation;
}

}  // namespace context_body_decoding_test

using namespace content_decoding_test;       // NOLINT(google-build-using-namespace)
using namespace context_body_decoding_test;  // NOLINT(google-build-using-namespace)
