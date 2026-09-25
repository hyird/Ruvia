#pragma once

#include <chrono>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"

#include "context_services_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace context_request_test {
using ruvia::Context;
using ruvia::HttpHeaderView;
using ruvia::HttpKnownMethod;
using ruvia::HttpRequest;
using ruvia::RequestMemory;
using ruvia::WorkerMemory;
using ruvia::detail::ContextAccess;

inline HttpRequest makeRequest(std::pmr::memory_resource* resource, std::string_view target,
    std::span<const HttpHeaderView> headers, std::string_view method = "GET") {
    auto [request, error] = ruvia::makeParsedHttpRequest(method, target, headers, {}, resource);
    if (error.has_value()) {
        throw std::invalid_argument("invalid test request");
    }
    return std::move(request);
}

inline HttpRequest makeRequest(std::pmr::memory_resource* resource,
    std::string_view target = "/", std::initializer_list<HttpHeaderView> headers = {},
    std::string_view method = "GET") {
    return makeRequest(resource, target,
        std::span<const HttpHeaderView>(headers.begin(), headers.size()), method);
}

inline asio::awaitable<void> readHeaderValue(ruvia::Context& context, std::string& output) {
    if (const auto value = context.req().header("X-Trace")) {
        output.assign(value->data(), value->size());
    }
    co_return;
}

struct MethodObservation final {
    std::string method;
    HttpKnownMethod knownMethod{HttpKnownMethod::kGet};
};

inline asio::awaitable<void> readMethod(ruvia::Context& context, MethodObservation& observation) {
    const auto request = context.req();
    observation.method.assign(request.method().data(), request.method().size());
    observation.knownMethod = request.knownMethod();
    co_return;
}
}  // namespace context_request_test

using namespace context_request_test;  // NOLINT(google-build-using-namespace)
