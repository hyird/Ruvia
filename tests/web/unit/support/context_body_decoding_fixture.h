#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/Bytes.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http/context/ContextServices.h"

#include "content_decoding_fixture.h"
#include "context_services_fixture.h"
#include "test_harness.h"

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
    auto [request, parseError] = ruvia::makeParsedHttpRequest(
        "GET", "/", {}, ruvia::asBytes(std::string_view("body")), memory.resource());
    if (parseError) {
        throw std::logic_error("invalid context text test request");
    }
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
    const ruvia::HttpHeaderView headers[]{{"Content-Encoding", "gzip"}};
    auto [request, parseError] = ruvia::makeParsedHttpRequest(
        "GET", "/", headers, ruvia::asBytes(encoded), memory.resource());
    if (parseError) {
        throw std::runtime_error("test request rejected Content-Encoding");
    }

    auto context = ruvia::detail::ContextAccess::make(memory, request,
        ruvia::test::testContextServices().withMaxDecodedBodyBytes(maxDecodedBodyBytes));
    asio::io_context io(1);
    auto future = asio::co_spawn(
        io, ruvia::asAwaitable(readContextText(context)), asio::use_future);
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
