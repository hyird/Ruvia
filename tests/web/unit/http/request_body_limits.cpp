#include <array>
#include <string>

#include "ruvia/core/asio_task.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_coding.h"

#include "context_body_decoding_fixture.h"
#include "server/inbound_buffer_resource.h"

// The product limits a web request body is decoded under.

RUVIA_TEST(web_request_decode_uses_the_configured_buffered_body_limit) {
    const std::string plain(2048, 'x');
    const std::string encoded = gzip_compress(plain);
    RUVIA_CHECK(!encoded.empty());
    const auto observation_value = read_context_gzip_body(encoded, 1024);
    RUVIA_CHECK_EQ(observation_value.error_status_, ruvia::http_status::content_too_large);
    RUVIA_CHECK(observation_value.body_.empty());
}

RUVIA_TEST(web_request_decode_accepts_content_at_the_configured_limit) {
    const std::string plain(2048, 'x');
    const std::string encoded = gzip_compress(plain);
    const auto observation_value = read_context_gzip_body(encoded, plain.size());
    RUVIA_CHECK(!observation_value.error_status_.has_value());
    RUVIA_CHECK_EQ(observation_value.body_, plain);
}

RUVIA_TEST(web_request_decode_rejects_empty_encoded_representation) {
    const auto observation_value = read_context_gzip_body({}, 1024);
    RUVIA_CHECK_EQ(observation_value.error_status_, ruvia::http_status::bad_request);
    RUVIA_CHECK(observation_value.body_.empty());
}

RUVIA_TEST(context_request_cold_operation_rejects_after_request_scope_closes) {
    auto operation = make_expired_context_text_read();
    bool rejected = false;
    asio::io_context io(1);
    auto future = asio::co_spawn(io,
        ruvia::as_awaitable(await_expired_context_text_read(operation, rejected)),
        asio::use_future);
    io.run();
    future.get();
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(web_request_decodes_content_coding_stacks_in_reverse_order) {
    ruvia::worker_memory worker;
    ruvia::detail::inbound_buffer_resource budget(worker.resource(), 64 * 1024);
    constexpr std::array codings{ruvia::http_content_coding::gzip,
        ruvia::http_content_coding::deflate};
    const std::string plain(8192, 'm');
    auto encoded = ruvia::encode_http_content(codings, plain, {.max_encoded_bytes_ = plain.size()});
    RUVIA_CHECK(encoded.encoded() != nullptr);
    if (encoded.encoded() == nullptr) {
        return;
    }
    {
        ruvia::request_memory memory(worker);
        const ruvia::http_header_view headers[]{{"Content-Encoding", "gzip, deflate"}};
        auto [request, error] = ruvia::make_parsed_http_request("POST", "/", headers,
            ruvia::as_bytes(encoded.encoded()->bytes()), memory.resource());
        RUVIA_CHECK(!error);
        auto context_value = ruvia::detail::context_access::make(memory, request,
            ruvia::test::test_context_services().with_inbound_buffer_pool(budget));
        asio::io_context io(1);
        auto result_value = asio::co_spawn(io, ruvia::as_awaitable(read_context_text(context_value)), asio::use_future);
        io.run();
        RUVIA_CHECK_EQ(result_value.get(), plain);
        RUVIA_CHECK(budget.used() >= plain.size());
    }
    RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
}

RUVIA_TEST(decoded_request_body_reserves_live_inbound_memory_until_context_destruction) {
    ruvia::worker_memory worker;
    ruvia::detail::inbound_buffer_resource budget(worker.resource(), 64 * 1024);
    const std::string plain(8192, 'a');
    const auto encoded = gzip_compress(plain);
    {
        ruvia::request_memory memory(worker);
        const ruvia::http_header_view headers[]{{"Content-Encoding", "gzip"}};
        auto [request, error] = ruvia::make_parsed_http_request(
            "POST", "/", headers, ruvia::as_bytes(encoded), memory.resource());
        RUVIA_CHECK(!error);
        auto context_value = ruvia::detail::context_access::make(memory, request,
            ruvia::test::test_context_services().with_inbound_buffer_pool(budget));
        asio::io_context io(1);
        auto result_value = asio::co_spawn(io, ruvia::as_awaitable(read_context_text(context_value)), asio::use_future);
        io.run();
        RUVIA_CHECK_EQ(result_value.get(), plain);
        const auto held = budget.used();
        RUVIA_CHECK(held >= plain.size());
        io.restart();
        auto repeated = asio::co_spawn(io, ruvia::as_awaitable(read_context_text(context_value)), asio::use_future);
        io.run();
        RUVIA_CHECK_EQ(repeated.get(), plain);
        RUVIA_CHECK_EQ(budget.used(), held);
    }
    RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
}

RUVIA_TEST(decoded_request_body_budget_rejects_before_amplification_and_releases_partial_output) {
    ruvia::worker_memory worker;
    ruvia::detail::inbound_buffer_resource budget(worker.resource(), 1024);
    ruvia::request_memory memory(worker);
    const auto encoded = gzip_compress(std::string(8192, 'a'));
    const ruvia::http_header_view headers[]{{"Content-Encoding", "gzip"}};
    auto [request, error] = ruvia::make_parsed_http_request(
        "POST", "/", headers, ruvia::as_bytes(encoded), memory.resource());
    RUVIA_CHECK(!error);
    auto context_value = ruvia::detail::context_access::make(memory, request,
        ruvia::test::test_context_services().with_inbound_buffer_pool(budget));
    asio::io_context io(1);
    auto result_value = asio::co_spawn(io, ruvia::as_awaitable(read_context_text(context_value)), asio::use_future);
    io.run();
    bool rejected = false;
    try {
        (void)result_value.get();
    } catch (const ruvia::detail::inbound_buffer_limit_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
}
