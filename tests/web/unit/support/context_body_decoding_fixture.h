#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/bytes.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/context.h"
#include "ruvia/web/error.h"

#include "content_decoding_fixture.h"
#include "context/context_access.h"
#include "context/context_services.h"
#include "context_services_fixture.h"
#include "test_harness.h"

namespace context_body_decoding_test {

struct context_body_read_observation final {
    std::string body_;
    std::optional<ruvia::http_status_code> error_status_;
};

inline ruvia::task<std::string_view> read_context_text(ruvia::context& context_value) {
    co_return co_await context_value.req().text();
}

inline ruvia::scoped_operation<std::string_view> make_expired_context_text_read() {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto [request, parse_error] = ruvia::make_parsed_http_request(
        "GET", "/", {}, ruvia::as_bytes(std::string_view("body")), memory.resource());
    if (parse_error) {
        throw std::logic_error("invalid context text test request");
    }
    auto context_value = ruvia::detail::context_access::make(
        memory, request, ruvia::test::test_context_services().with_max_decoded_body_bytes(1024));
    return context_value.req().text();
}

inline ruvia::task<void> await_expired_context_text_read(
    ruvia::scoped_operation<std::string_view>& operation, bool& rejected) {
    try {
        (void)co_await std::move(operation);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

inline context_body_read_observation read_context_gzip_body(
    std::string_view encoded, std::size_t max_decoded_body_bytes) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    const ruvia::http_header_view headers[]{{"Content-Encoding", "gzip"}};
    auto [request, parse_error] = ruvia::make_parsed_http_request(
        "GET", "/", headers, ruvia::as_bytes(encoded), memory.resource());
    if (parse_error) {
        throw std::runtime_error("test request rejected Content-Encoding");
    }

    auto context_value = ruvia::detail::context_access::make(memory, request,
        ruvia::test::test_context_services().with_max_decoded_body_bytes(max_decoded_body_bytes));
    asio::io_context io(1);
    auto future = asio::co_spawn(
        io, ruvia::as_awaitable(read_context_text(context_value)), asio::use_future);
    io.run();

    context_body_read_observation observation;
    try {
        observation.body_ = future.get();
    } catch (const ruvia::http_protocol_error& error) {
        observation.error_status_ = error.status();
    }
    return observation;
}

}  // namespace context_body_decoding_test

using namespace content_decoding_test;       // NOLINT(google-build-using-namespace)
using namespace context_body_decoding_test;  // NOLINT(google-build-using-namespace)
