#include "ruvia/core/AsioTask.h"
#include "ruvia/web/detail/server/inbound_buffer_resource.h"

#include "context_body_decoding_fixture.h"

// The product limits a web request body is decoded under.

RUVIA_TEST(web_request_decode_uses_the_configured_buffered_body_limit) {
    const std::string plain(2048, 'x');
    const std::string encoded = gzipCompress(plain);
    RUVIA_CHECK(!encoded.empty());
    const auto observation = readContextGzipBody(encoded, 1024);
    RUVIA_CHECK_EQ(observation.errorStatus, ruvia::http_status::kContentTooLarge);
    RUVIA_CHECK(observation.body.empty());
}

RUVIA_TEST(web_request_decode_accepts_content_at_the_configured_limit) {
    const std::string plain(2048, 'x');
    const std::string encoded = gzipCompress(plain);
    const auto observation = readContextGzipBody(encoded, plain.size());
    RUVIA_CHECK(!observation.errorStatus.has_value());
    RUVIA_CHECK_EQ(observation.body, plain);
}

RUVIA_TEST(web_request_decode_rejects_empty_encoded_representation) {
    const auto observation = readContextGzipBody({}, 1024);
    RUVIA_CHECK_EQ(observation.errorStatus, ruvia::http_status::kBadRequest);
    RUVIA_CHECK(observation.body.empty());
}

RUVIA_TEST(context_request_cold_operation_rejects_after_request_scope_closes) {
    auto operation = makeExpiredContextTextRead();
    bool rejected = false;
    asio::io_context io(1);
    auto future = asio::co_spawn(io,
        ruvia::asAwaitable(awaitExpiredContextTextRead(operation, rejected)),
        asio::use_future);
    io.run();
    future.get();
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(decoded_request_body_reserves_live_inbound_memory_until_context_destruction) {
    ruvia::WorkerMemory worker;
    ruvia::detail::inbound_buffer_resource budget(worker.resource(), 64 * 1024);
    const std::string plain(8192, 'a');
    const auto encoded = gzipCompress(plain);
    {
        ruvia::RequestMemory memory(worker);
        const ruvia::HttpHeaderView headers[]{{"Content-Encoding", "gzip"}};
        auto [request, error] = ruvia::makeParsedHttpRequest(
            "POST", "/", headers, ruvia::asBytes(encoded), memory.resource());
        RUVIA_CHECK(!error);
        auto context = ruvia::detail::ContextAccess::make(memory, request,
            ruvia::test::testContextServices().with_inbound_buffer_pool(budget));
        asio::io_context io(1);
        auto result = asio::co_spawn(io, ruvia::asAwaitable(readContextText(context)), asio::use_future);
        io.run();
        RUVIA_CHECK_EQ(result.get(), plain);
        const auto held = budget.used();
        RUVIA_CHECK(held >= plain.size());
        io.restart();
        auto repeated = asio::co_spawn(io, ruvia::asAwaitable(readContextText(context)), asio::use_future);
        io.run();
        RUVIA_CHECK_EQ(repeated.get(), plain);
        RUVIA_CHECK_EQ(budget.used(), held);
    }
    RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
}

RUVIA_TEST(decoded_request_body_budget_rejects_before_amplification_and_releases_partial_output) {
    ruvia::WorkerMemory worker;
    ruvia::detail::inbound_buffer_resource budget(worker.resource(), 1024);
    ruvia::RequestMemory memory(worker);
    const auto encoded = gzipCompress(std::string(8192, 'a'));
    const ruvia::HttpHeaderView headers[]{{"Content-Encoding", "gzip"}};
    auto [request, error] = ruvia::makeParsedHttpRequest(
        "POST", "/", headers, ruvia::asBytes(encoded), memory.resource());
    RUVIA_CHECK(!error);
    auto context = ruvia::detail::ContextAccess::make(memory, request,
        ruvia::test::testContextServices().with_inbound_buffer_pool(budget));
    asio::io_context io(1);
    auto result = asio::co_spawn(io, ruvia::asAwaitable(readContextText(context)), asio::use_future);
    io.run();
    bool rejected = false;
    try {
        (void)result.get();
    } catch (const ruvia::detail::inbound_buffer_limit_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
}
