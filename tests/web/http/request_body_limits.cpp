#include "context_body_decoding_fixture.h"

// The product limits a web request body is decoded under.

RUVIA_TEST(web_request_decode_uses_the_configured_buffered_body_limit) {
    const std::string plain(2048, 'x');
    const std::string encoded = gzipCompress(plain);
    RUVIA_CHECK(!encoded.empty());
    const auto observation = readContextGzipBody(encoded, 1024);
    RUVIA_CHECK_EQ(
        observation.errorStatus,
        ruvia::http_status::kContentTooLarge);
    RUVIA_CHECK(observation.body.empty());
}

RUVIA_TEST(web_request_decode_accepts_content_at_the_configured_limit) {
    const std::string plain(2048, 'x');
    const std::string encoded = gzipCompress(plain);
    const auto observation = readContextGzipBody(
        encoded,
        plain.size());
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
    auto future = asio::co_spawn(
        io,
        ruvia::detail::taskAsAwaitable(
            awaitExpiredContextTextRead(operation, rejected)),
        asio::use_future);
    io.run();
    future.get();
    RUVIA_CHECK(rejected);
}
