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

#include "ruvia/core/asio_task.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/context.h"
#include "ruvia/web/error.h"

#include "context/context_access.h"
#include "context_services_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace context_request_test {
using ruvia::context;
using ruvia::http_header_view;
using ruvia::http_known_method;
using ruvia::http_request;
using ruvia::request_memory;
using ruvia::worker_memory;
using ruvia::detail::context_access;

inline http_request make_request(std::pmr::memory_resource* resource, std::string_view target,
    std::span<const http_header_view> headers, std::string_view method = "GET") {
    auto [request, error] = ruvia::make_parsed_http_request(method, target, headers, {}, resource);
    if (error.has_value()) {
        throw std::invalid_argument("invalid test request");
    }
    return std::move(request);
}

inline http_request make_request(std::pmr::memory_resource* resource,
    std::string_view target = "/", std::initializer_list<http_header_view> headers = {},
    std::string_view method = "GET") {
    return make_request(resource, target,
        std::span<const http_header_view>(headers.begin(), headers.size()), method);
}

inline asio::awaitable<void> read_header_value(ruvia::context& context_value, std::string& output) {
    if (const auto value = context_value.req().header("X-Trace")) {
        output.assign(value->data(), value->size());
    }
    co_return;
}

struct method_observation final {
    std::string method_;
    http_known_method known_method_{http_known_method::get};
};

inline asio::awaitable<void> read_method(ruvia::context& context_value, method_observation& observation_value) {
    const auto request = context_value.req();
    observation_value.method_.assign(request.method().data(), request.method().size());
    observation_value.known_method_ = request.known_method();
    co_return;
}
}  // namespace context_request_test

using namespace context_request_test;  // NOLINT(google-build-using-namespace)
