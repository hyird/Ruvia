#include <array>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_coding.h"

#include "context_body_decoding_fixture.h"

RUVIA_TEST(web_request_decode_accepts_gzip_content) {
    const std::string plain(2048, 'x');
    const std::string encoded = gzip_compress(plain);
    RUVIA_CHECK(!encoded.empty());
    const auto observation_value = read_context_gzip_body(encoded);
    RUVIA_CHECK(!observation_value.error_status_.has_value());
    RUVIA_CHECK_EQ(observation_value.body_, plain);
}

RUVIA_TEST(web_request_decode_accepts_empty_encoded_representation) {
    const auto observation_value = read_context_gzip_body({});
    RUVIA_CHECK(!observation_value.error_status_.has_value());
    RUVIA_CHECK(observation_value.body_.empty());
}

RUVIA_TEST(context_request_cold_operation_rejects_after_request_scope_closes) {
    auto operation = make_expired_context_text_read();
    bool rejected = false;
    asio::io_context io(1);
    auto future = asio::co_spawn(io,
        ruvia::as_awaitable(await_expired_context_text_read(*operation, rejected)),
        asio::use_future);
    io.run();
    future.get();
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(web_request_decodes_content_coding_stacks_in_reverse_order) {
    constexpr std::array codings{ruvia::http_content_coding::gzip,
        ruvia::http_content_coding::deflate};
    const std::string plain(8192, 'm');
    auto encoded = ruvia::encode_http_content(codings, plain, {.max_encoded_bytes_ = plain.size()});
    RUVIA_CHECK(encoded.encoded() != nullptr);
    if (encoded.encoded() == nullptr) {
        return;
    }
    (void)context_request_test::with_context(
        ruvia::test_request::post("/").header("Content-Encoding", "gzip, deflate").body(encoded.encoded()->bytes()),
        [&](ruvia::context& context_value) -> ruvia::task<void> {
            const auto body = co_await context_value.req().text();
            RUVIA_CHECK_EQ(body, std::string_view(plain));
        });
}
