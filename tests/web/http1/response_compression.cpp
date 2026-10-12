#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/http/http_content_codec.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

// Inline buffered-response compression on one worker, whose encoders are
// reused across responses. This executable owns its application, so the
// app-wide compression policy cannot change the bodies of other suites.

namespace {

[[maybe_unused]] const bool compression_configured = [] {
    ruvia::app().compression({.min_bytes_ = 16});
    return true;
}();

[[nodiscard]] std::string make_body(std::size_t size, unsigned seed) {
    std::string body(size, '\0');
    unsigned state_value = seed * 2654435761U + 3U;
    for (std::size_t i = 0; i < size; ++i) {
        state_value = state_value * 1103515245U + 12345U;
        body[i] = static_cast<char>('a' + ((state_value >> 16U) % 6U) + ((i / 48U) % 5U));
    }
    return body;
}

// Both stay below the default 64 KiB synchronous limit, so neither is offloaded.
const std::string small_body = make_body(3'000, 1);
const std::string large_body = make_body(60'000, 2);

class response_compression_routes final {
    RUVIA_ROUTES_BEGIN(response_compression_routes)
    RUVIA_GET("/compression-small", small);
    RUVIA_GET("/compression-large", large);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> small(ruvia::context& context_value) {
        co_return context_value.text(std::string_view(small_body));
    }

    ruvia::task<ruvia::http_response> large(ruvia::context& context_value) {
        co_return context_value.text(std::string_view(large_body));
    }
};

// Sends one request and returns every byte the server wrote before closing,
// or nullopt when the connection is still open after 5 s.
[[nodiscard]] std::optional<std::string> fetch_response(
    const asio::ip::tcp::endpoint& server_endpoint, std::string_view request) {
    auto& io = ruvia::test::new_test_io_context();
    asio::ip::tcp::socket socket(io);
    socket.connect(server_endpoint);
    asio::write(socket, asio::buffer(request));
    std::string output;
    bool closed = false;
    asio::async_read(socket, asio::dynamic_buffer(output),
        [&closed](const std::error_code&, std::size_t) { closed = true; });
    io.run_for(std::chrono::seconds(5));
    if (!closed) {
        return std::nullopt;
    }
    return output;
}

[[nodiscard]] std::string lower_head(std::string_view response) {
    std::string head(response.substr(0, response.find("\r\n\r\n")));
    std::ranges::transform(head, head.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return head;
}

[[nodiscard]] std::string expected_encoding(ruvia::http_content_coding coding, std::string_view body) {
    const auto result_value = ruvia::encode_http_content(coding, body, {.max_encoded_bytes_ = body.size() - 1});
    const auto* encoded = result_value.encoded();
    return encoded != nullptr ? std::string(encoded->bytes()) : std::string();
}

}  // namespace

RUVIA_TEST(http1_inline_compression_reuses_worker_encoders_with_identical_bytes) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    constexpr ruvia::http_content_coding codings[]{ruvia::http_content_coding::gzip,
        ruvia::http_content_coding::deflate, ruvia::http_content_coding::brotli, ruvia::http_content_coding::zstd};
    // Each encoder serves several responses of alternating sizes in a row.
    for (int round = 0; round < 2; ++round) {
        for (const auto coding : codings) {
            for (const bool large : {true, false, true}) {
                const std::string_view path = large ? "/compression-large" : "/compression-small";
                const std::string_view body = large ? large_body : small_body;
                const auto token = ruvia::http_content_coding_token(coding);
                const auto output = fetch_response(server.endpoint(),
                    "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: " +
                        std::string(token) + "\r\nConnection: close\r\n\r\n");
                RUVIA_CHECK(output.has_value());
                const auto text = output.value_or("");
                RUVIA_CHECK(text.starts_with("HTTP/1.1 200 "));
                RUVIA_CHECK(lower_head(text).find("\r\ncontent-encoding: " + std::string(token) + "\r\n") !=
                            std::string::npos);
                const auto separator = text.find("\r\n\r\n");
                RUVIA_CHECK(separator != std::string::npos);
                if (separator != std::string::npos) {
                    const auto expected = expected_encoding(coding, body);
                    RUVIA_CHECK(!expected.empty());
                    RUVIA_CHECK(std::string_view(text).substr(separator + 4) == expected);
                }
            }
        }
    }
    server.finish();
}
