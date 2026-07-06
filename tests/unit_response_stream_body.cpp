#include "test_harness.h"

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/use_awaitable.hpp>

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "net/server/ConnectionScanner.h"
#include "net/server/HttpResponseHeadBuffer.h"
#include "net/server/HttpResponseWriter.h"
#include "runtime/AsioAwait.h"
#include "ruvia/http/HttpBodyStream.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/memory/MemoryPool.h"
#include "../src/http/HttpResponseBodyAccess.h"

namespace {

using asio::ip::tcp;

// A stack-owned test producer exposed as an HttpBodyStream: it yields a fixed list of chunks, each
// held in `current` so the returned view stays valid until the next call.
struct TestProducer {
    std::vector<std::string> chunks;
    std::size_t index = 0;
    std::string current;

    static ruvia::Task<std::string_view> next(void* self) {
        auto* producer = static_cast<TestProducer*>(self);
        if (producer->index < producer->chunks.size()) {
            producer->current = producer->chunks[producer->index++];
            co_return std::string_view(producer->current);
        }
        co_return std::string_view{};
    }
    static void destroy(void*) noexcept {}  // stack-owned; nothing to free
};

}  // namespace

// writeStreamingResponse must emit a valid HTTP/1.1 chunked response whose reassembled body is the
// concatenation of the producer's chunks -- this is the h1 half of the streaming/proxy write path.
RUVIA_TEST(response_stream_body_writes_chunked) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    std::string received;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket client(io);
            co_await client.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            char buffer[512];
            for (;;) {
                auto [ec, n] = co_await client.async_read_some(
                    asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
                if (ec || n == 0) {
                    break;
                }
                received.append(buffer, n);
            }
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket server = co_await acceptor.async_accept(asio::use_awaitable);
            TestProducer producer{.chunks = {"Hello, ", "streamed ", "world!"}};
            ruvia::HttpResponse response(std::pmr::get_default_resource());
            response.status(200);
            response.header("X-Test", "yes");
            ruvia::detail::setResponseStreamBody(
                response,
                ruvia::HttpBodyStream(&producer, &TestProducer::next, &TestProducer::destroy));

            ruvia::WorkerMemory worker;
            ruvia::detail::ResponseHeadBuffer head(worker.allocator<char>());
            ruvia::detail::ConnectionScanner::Entry entry;
            std::error_code ec;
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::writeStreamingResponse(
                server, head, entry, response, /*http11=*/true, /*skipBody=*/false, ec));
            std::error_code ignored;
            server.shutdown(tcp::socket::shutdown_send, ignored);
        },
        asio::detached);

    io.run();

    RUVIA_CHECK(received.find("HTTP/1.1 200") != std::string::npos);
    RUVIA_CHECK(received.find("Transfer-Encoding: chunked") != std::string::npos);
    RUVIA_CHECK(received.find("X-Test: yes") != std::string::npos);
    RUVIA_CHECK(received.find("Content-Length") == std::string::npos);  // never for a stream
    // Chunk data (each frame is "<hexsize>\r\n<data>\r\n").
    RUVIA_CHECK(received.find("7\r\nHello, \r\n") != std::string::npos);
    RUVIA_CHECK(received.find("9\r\nstreamed \r\n") != std::string::npos);
    RUVIA_CHECK(received.find("6\r\nworld!\r\n") != std::string::npos);
    // Last-chunk terminator.
    RUVIA_CHECK(received.find("0\r\n\r\n") != std::string::npos);
}
