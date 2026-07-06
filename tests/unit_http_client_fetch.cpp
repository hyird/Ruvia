#include "test_harness.h"

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/buffers_iterator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <functional>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <zlib.h>

#include "http/client/HttpClientPool.h"
#include "runtime/AsioAwait.h"
#include "ruvia/http/HttpClient.h"

namespace {

// gzip-compress `input` (RFC 1952, windowBits 15+16) so tests can build encoded bodies.
std::string gzipCompress(std::string_view input) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }
    std::string out(input.size() + 64, '\0');
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    const int status = deflate(&stream, Z_FINISH);
    const auto produced = out.size() - stream.avail_out;
    deflateEnd(&stream);
    if (status != Z_STREAM_END) {
        return {};
    }
    out.resize(produced);
    return out;
}

// Format the decimal length of a body, for building Content-Length headers.
std::string lengthOf(std::string_view body) {
    return std::to_string(body.size());
}

struct FetchOutcome {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

struct UploadOutcome {
    bool ok = false;
    int status = 0;
    std::string body;
    int transferEncodingHeaders = 0;
    std::string error;
};

struct TestBodyProducer final {
    std::vector<std::string_view> chunks;
    std::size_t index = 0;

    explicit TestBodyProducer(std::vector<std::string_view> values)
        : chunks(std::move(values)) {}

    static ruvia::Task<std::string_view> nextChunk(void* target) {
        auto& self = *static_cast<TestBodyProducer*>(target);
        if (self.index < self.chunks.size()) {
            co_return self.chunks[self.index++];
        }
        co_return std::string_view{};
    }
};

RUVIA_TEST(request_body_stream_empty_producer_yields_eof) {
    asio::io_context io;
    bool completed = false;
    std::string_view chunk = "not empty";

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            ruvia::RequestBodyStream stream;
            chunk = co_await ruvia::detail::taskAsAwaitable(stream.nextChunk());
            completed = true;
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(completed);
    RUVIA_CHECK(chunk.empty());
}

enum class WriteMode {
    kWhole,        // one write of the entire response
    kHeadThenBody, // head, then the whole body (forces at least one socket read of the body)
    kByteWise,     // one byte per write (fragments every CRLF; stresses readLine across fill)
};

// Spin up a one-shot loopback server that replies with `cannedResponse`, then drive
// the real HttpClientPool against it end-to-end on a single io_context. A non-zero
// maxResponseBodyBytes caps the decoded body.
FetchOutcome runOneFetch(
    std::string cannedResponse, WriteMode writeMode, std::size_t maxResponseBodyBytes = 0) {
    using asio::ip::tcp;
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    FetchOutcome out;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto sock = co_await acceptor.async_accept(asio::use_awaitable);
                asio::streambuf request;
                co_await asio::async_read_until(
                    sock, request, std::string("\r\n\r\n"), asio::use_awaitable);
                if (writeMode == WriteMode::kHeadThenBody) {
                    // Flush the head first so the client must read the body off the socket
                    // (exercising the chunked reader's fill path rather than buffered data).
                    const auto headEnd = cannedResponse.find("\r\n\r\n") + 4;
                    co_await asio::async_write(
                        sock, asio::buffer(cannedResponse.data(), headEnd), asio::use_awaitable);
                    co_await asio::async_write(
                        sock,
                        asio::buffer(cannedResponse.data() + headEnd, cannedResponse.size() - headEnd),
                        asio::use_awaitable);
                } else if (writeMode == WriteMode::kByteWise) {
                    // One byte per write: every CRLF (chunk size line, data terminator, and the
                    // final trailer) is fragmented across reads, stressing readLine()/fill().
                    for (std::size_t i = 0; i < cannedResponse.size(); ++i) {
                        co_await asio::async_write(
                            sock, asio::buffer(cannedResponse.data() + i, 1), asio::use_awaitable);
                    }
                } else {
                    co_await asio::async_write(
                        sock, asio::buffer(cannedResponse), asio::use_awaitable);
                }
                std::error_code ignored;
                sock.shutdown(tcp::socket::shutdown_both, ignored);
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", std::pmr::get_default_resource());
    config.port = port;
    config.tls = false;
    config.poolSizePerWorker = 1;
    config.maxResponseBodyBytes = maxResponseBodyBytes;  // 0 = unlimited

    auto pool = std::make_unique<ruvia::detail::HttpClientPool>(
        io, std::move(config), std::pmr::get_default_resource());

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto response = co_await ruvia::detail::taskAsAwaitable(
                    pool->fetch("/", options, std::pmr::get_default_resource()));
                out.ok = true;
                out.status = response.status();
                out.body.assign(response.body().data(), response.body().size());
            } catch (const std::exception& e) {
                out.error += std::string("client:") + e.what();
            }
            pool->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

FetchOutcome runFetchWithRequestHeader(ruvia::HttpHeaderView header) {
    using asio::ip::tcp;
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    FetchOutcome out;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto sock = co_await acceptor.async_accept(asio::use_awaitable);
                asio::streambuf request;
                co_await asio::async_read_until(
                    sock, request, std::string("\r\n\r\n"), asio::use_awaitable);
                co_await asio::async_write(
                    sock,
                    asio::buffer(std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK")),
                    asio::use_awaitable);
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", std::pmr::get_default_resource());
    config.port = port;
    config.tls = false;
    config.poolSizePerWorker = 1;

    auto pool = std::make_unique<ruvia::detail::HttpClientPool>(
        io, std::move(config), std::pmr::get_default_resource());

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                std::array<ruvia::HttpHeaderView, 1> headers{header};
                ruvia::FetchOptions options;
                options.headers = headers;
                auto response = co_await ruvia::detail::taskAsAwaitable(
                    pool->fetch("/", options, std::pmr::get_default_resource()));
                out.ok = true;
                out.status = response.status();
                out.body.assign(response.body().data(), response.body().size());
            } catch (const std::exception& e) {
                out.error += std::string("client:") + e.what();
            }
            pool->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

// --- Expect: 100-continue ------------------------------------------------
enum class ContinueScenario { kSend100, kRejectFinal, kSilent };

struct ContinueOutcome {
    bool ok = false;
    int status = 0;
    std::string body;        // client-visible response body
    std::string serverBody;  // request-body bytes the server actually received
    bool serverGotExpect = false;
    std::string error;
};

// Drive a POST with options.expectContinue against a mock server that either sends 100 (then
// reads the body), rejects with a final status before the body, or stays silent (never sends
// 100). `requestBody` is fixed; the harness reports what the server received so a skipped body
// is observable.
ContinueOutcome runExpectContinueFetch(
    ContinueScenario scenario, std::chrono::milliseconds requestTimeout) {
    using asio::ip::tcp;
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    ContinueOutcome out;
    static const std::string requestBody = "hello-expect-body";  // 17 bytes

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto sock = co_await acceptor.async_accept(asio::use_awaitable);
                std::string buf;
                auto readMore = [&]() -> asio::awaitable<bool> {
                    char tmp[512];
                    auto [ec, n] = co_await sock.async_read_some(
                        asio::buffer(tmp), asio::as_tuple(asio::use_awaitable));
                    if (ec || n == 0) {
                        co_return false;
                    }
                    buf.append(tmp, n);
                    co_return true;
                };
                std::size_t headEnd = std::string::npos;
                for (;;) {
                    headEnd = buf.find("\r\n\r\n");
                    if (headEnd != std::string::npos) {
                        break;
                    }
                    if (!co_await readMore()) {
                        co_return;
                    }
                }
                headEnd += 4;
                const std::string head = buf.substr(0, headEnd);
                out.serverGotExpect = head.find("Expect: 100-continue") != std::string::npos;
                std::size_t contentLength = 0;
                auto clPos = head.find("Content-Length:");
                if (clPos != std::string::npos) {
                    clPos = head.find(':', clPos) + 1;
                    while (clPos < head.size() && head[clPos] == ' ') {
                        ++clPos;
                    }
                    while (clPos < head.size() && head[clPos] >= '0' && head[clPos] <= '9') {
                        contentLength = contentLength * 10 + static_cast<std::size_t>(head[clPos] - '0');
                        ++clPos;
                    }
                }
                auto writeAll = [&](std::string_view bytes) -> asio::awaitable<void> {
                    co_await asio::async_write(
                        sock, asio::buffer(bytes.data(), bytes.size()), asio::use_awaitable);
                };
                auto readBody = [&]() -> asio::awaitable<void> {
                    std::string body = buf.substr(headEnd);
                    while (body.size() < contentLength) {
                        if (!co_await readMore()) {
                            break;
                        }
                        body = buf.substr(headEnd);
                    }
                    out.serverBody = body.substr(0, std::min(body.size(), contentLength));
                };

                if (scenario == ContinueScenario::kRejectFinal) {
                    // Reject up front WITHOUT reading the body.
                    co_await writeAll(
                        "HTTP/1.1 417 Expectation Failed\r\nContent-Length: 8\r\n\r\ntoo-big!");
                } else {
                    if (scenario == ContinueScenario::kSend100) {
                        co_await writeAll("HTTP/1.1 100 Continue\r\n\r\n");
                    }
                    co_await readBody();  // kSilent: no 100 sent; just wait for the body
                    co_await writeAll(
                        "HTTP/1.1 200 OK\r\nContent-Length: " +
                        std::to_string(out.serverBody.size()) + "\r\n\r\n" + out.serverBody);
                }
                std::error_code ignored;
                sock.shutdown(tcp::socket::shutdown_both, ignored);
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", std::pmr::get_default_resource());
    config.port = port;
    config.tls = false;
    config.poolSizePerWorker = 1;
    config.requestTimeout = requestTimeout;

    auto pool = std::make_unique<ruvia::detail::HttpClientPool>(
        io, std::move(config), std::pmr::get_default_resource());

    asio::steady_timer watchdog(io, std::chrono::seconds(3));
    watchdog.async_wait([&](const std::error_code& ec) {
        if (!ec) {
            out.error += "watchdog:timeout";
            io.stop();
        }
    });

    // Standalone tests have no ConnectionScanner, so drive scanDeadlines manually -- this is what
    // fires the bounded continue-wait timeout when the server stays silent.
    auto scanTimer = std::make_shared<asio::steady_timer>(io);
    std::function<void()> armScan = [&]() {
        scanTimer->expires_after(std::chrono::milliseconds(10));
        scanTimer->async_wait([&](const std::error_code& ec) {
            if (ec) {
                return;
            }
            pool->scanDeadlines(std::chrono::steady_clock::now());
            armScan();
        });
    };
    armScan();

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                options.method = "POST";
                options.body = requestBody;
                options.expectContinue = true;
                auto response = co_await ruvia::detail::taskAsAwaitable(
                    pool->fetch("/", options, std::pmr::get_default_resource()));
                out.ok = true;
                out.status = response.status();
                out.body.assign(response.body().data(), response.body().size());
            } catch (const std::exception& e) {
                out.error += std::string("client:") + e.what();
            }
            watchdog.cancel();
            scanTimer->cancel();
            pool->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

// Drive a streaming fetch: pull the body chunk-by-chunk and concatenate it.
FetchOutcome runStreamFetch(std::string cannedResponse, WriteMode writeMode) {
    using asio::ip::tcp;
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    FetchOutcome out;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto sock = co_await acceptor.async_accept(asio::use_awaitable);
                asio::streambuf request;
                co_await asio::async_read_until(
                    sock, request, std::string("\r\n\r\n"), asio::use_awaitable);
                if (writeMode == WriteMode::kByteWise) {
                    for (std::size_t i = 0; i < cannedResponse.size(); ++i) {
                        co_await asio::async_write(
                            sock, asio::buffer(cannedResponse.data() + i, 1), asio::use_awaitable);
                    }
                } else {
                    co_await asio::async_write(
                        sock, asio::buffer(cannedResponse), asio::use_awaitable);
                }
                std::error_code ignored;
                sock.shutdown(tcp::socket::shutdown_both, ignored);
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", std::pmr::get_default_resource());
    config.port = port;
    config.tls = false;
    config.poolSizePerWorker = 1;

    auto pool = std::make_unique<ruvia::detail::HttpClientPool>(
        io, std::move(config), std::pmr::get_default_resource());

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto stream = co_await ruvia::detail::taskAsAwaitable(
                    pool->fetchStream("/", options, std::pmr::get_default_resource()));
                out.status = stream.status();
                for (;;) {
                    auto chunk = co_await ruvia::detail::taskAsAwaitable(stream.readChunk());
                    if (chunk.empty()) {
                        break;
                    }
                    out.body.append(chunk.data(), chunk.size());
                }
                // Reading past end-of-stream must stay safe and keep returning empty (regression
                // guard: the connection has already been released by the final readChunk).
                auto afterEof = co_await ruvia::detail::taskAsAwaitable(stream.readChunk());
                if (!afterEof.empty()) {
                    out.error += "extra-after-eof";
                }
                out.ok = true;
            } catch (const std::exception& e) {
                out.error += std::string("client:") + e.what();
            }
            pool->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

// Drive a streaming fetch with options.decodeStream set, pulling the (decoded) body chunk by
// chunk. `writeMode == kByteWise` fragments the encoded response across reads so the incremental
// decoder is exercised on tiny input slices.
FetchOutcome runStreamFetchDecoded(std::string cannedResponse, WriteMode writeMode) {
    using asio::ip::tcp;
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    FetchOutcome out;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto sock = co_await acceptor.async_accept(asio::use_awaitable);
                asio::streambuf request;
                co_await asio::async_read_until(
                    sock, request, std::string("\r\n\r\n"), asio::use_awaitable);
                if (writeMode == WriteMode::kByteWise) {
                    for (std::size_t i = 0; i < cannedResponse.size(); ++i) {
                        co_await asio::async_write(
                            sock, asio::buffer(cannedResponse.data() + i, 1), asio::use_awaitable);
                    }
                } else {
                    co_await asio::async_write(
                        sock, asio::buffer(cannedResponse), asio::use_awaitable);
                }
                std::error_code ignored;
                sock.shutdown(tcp::socket::shutdown_both, ignored);
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", std::pmr::get_default_resource());
    config.port = port;
    config.tls = false;
    config.poolSizePerWorker = 1;

    auto pool = std::make_unique<ruvia::detail::HttpClientPool>(
        io, std::move(config), std::pmr::get_default_resource());

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                options.decodeStream = true;
                auto stream = co_await ruvia::detail::taskAsAwaitable(
                    pool->fetchStream("/", options, std::pmr::get_default_resource()));
                out.status = stream.status();
                for (;;) {
                    auto chunk = co_await ruvia::detail::taskAsAwaitable(stream.readChunk());
                    if (chunk.empty()) {
                        break;
                    }
                    out.body.append(chunk.data(), chunk.size());
                }
                out.ok = true;
            } catch (const std::exception& e) {
                out.error += std::string("client:") + e.what();
            }
            pool->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

RUVIA_TEST(http_client_stream_gzip_decoded_whole) {
    const auto gz = gzipCompress("streamed-and-compressed-payload");
    std::string response =
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: " + lengthOf(gz) +
        "\r\n\r\n" + gz;
    const auto out = runStreamFetchDecoded(std::move(response), WriteMode::kWhole);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 200);
    RUVIA_CHECK_EQ(out.body, std::string("streamed-and-compressed-payload"));
}

RUVIA_TEST(http_client_stream_gzip_decoded_bytewise) {
    // Fragment the gzip stream one byte per write so the incremental decoder must span reads.
    const auto gz = gzipCompress("streamed-and-compressed-payload");
    std::string response =
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: " + lengthOf(gz) +
        "\r\n\r\n" + gz;
    const auto out = runStreamFetchDecoded(std::move(response), WriteMode::kByteWise);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.body, std::string("streamed-and-compressed-payload"));
}

RUVIA_TEST(http_client_stream_gzip_decoded_large_multichunk) {
    // A payload larger than the decoder's per-call output cap and the transport chunking, sent
    // chunked, must reassemble losslessly across many readChunk() calls.
    std::string plain;
    for (int i = 0; i < 40000; ++i) {
        plain.push_back(static_cast<char>('A' + (i % 26)));
    }
    const auto gz = gzipCompress(plain);
    std::string response =
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: " + lengthOf(gz) +
        "\r\n\r\n" + gz;
    const auto out = runStreamFetchDecoded(std::move(response), WriteMode::kWhole);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.body.size(), std::size_t{40000});
    RUVIA_CHECK_EQ(out.body, plain);
}

RUVIA_TEST(http_client_stream_gzip_truncated_is_error) {
    // A gzip stream cut short (last 5 bytes dropped) must surface a truncation error, not a
    // silently-short clean end.
    auto gz = gzipCompress("streamed-and-compressed-payload");
    gz.resize(gz.size() - 5);
    std::string response =
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: " + lengthOf(gz) +
        "\r\n\r\n" + gz;
    const auto out = runStreamFetchDecoded(std::move(response), WriteMode::kWhole);
    RUVIA_CHECK(!out.ok);
    RUVIA_CHECK(out.error.find("truncated compressed response") != std::string::npos);
}

RUVIA_TEST(http_client_stream_undecoded_without_flag_is_raw) {
    // Without decodeStream the encoded bytes are delivered as received (opt-in behavior).
    const auto gz = gzipCompress("raw-bytes-please");
    std::string response =
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: " + lengthOf(gz) +
        "\r\n\r\n" + gz;
    const auto out = runStreamFetch(std::move(response), WriteMode::kWhole);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.body, gz);  // raw compressed bytes, not decoded
}

// A close-delimited body ends at connection CLOSE. If the peer stalls mid-body and the
// per-read idle timeout fires, the streaming reader must surface a truncation error --
// not report the short body as a complete stream. The buffered fetch() and the sibling
// Content-Length/chunked stream framings already throw on a short read; only the
// streaming close-delimited path used to end silently.
RUVIA_TEST(http_client_stream_close_delimited_timeout_is_truncation_not_clean_end) {
    using asio::ip::tcp;
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    // Server: send a close-delimited head (no Content-Length, no Transfer-Encoding)
    // plus a partial body, then hold the connection open so the client's idle timeout
    // fires mid-body instead of a clean peer close.
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto sock = co_await acceptor.async_accept(asio::use_awaitable);
                asio::streambuf request;
                co_await asio::async_read_until(
                    sock, request, std::string("\r\n\r\n"), asio::use_awaitable);
                const std::string headAndPartial =
                    "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npartial-body";
                co_await asio::async_write(sock, asio::buffer(headAndPartial), asio::use_awaitable);
                asio::steady_timer stall(io, std::chrono::seconds(5));
                co_await stall.async_wait(asio::use_awaitable);
            } catch (...) {
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", std::pmr::get_default_resource());
    config.port = port;
    config.tls = false;
    config.poolSizePerWorker = 1;
    config.requestTimeout = std::chrono::milliseconds(300);  // per-read idle timeout

    auto pool = std::make_unique<ruvia::detail::HttpClientPool>(
        io, std::move(config), std::pmr::get_default_resource());

    bool threwTruncation = false;
    bool endedCleanly = false;

    // Emulate the ConnectionScanner: enforce per-connection deadlines periodically.
    // Without this the idle deadline (a recorded time, not an armed timer) never fires.
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                for (;;) {
                    asio::steady_timer tick(io, std::chrono::milliseconds(20));
                    co_await tick.async_wait(asio::use_awaitable);
                    pool->scanDeadlines(std::chrono::steady_clock::now());
                }
            } catch (...) {
            }
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto stream = co_await ruvia::detail::taskAsAwaitable(
                    pool->fetchStream("/", options, std::pmr::get_default_resource()));
                for (;;) {
                    auto chunk = co_await ruvia::detail::taskAsAwaitable(stream.readChunk());
                    if (chunk.empty()) {
                        endedCleanly = true;  // pre-fix path: truncation reported as a clean end
                        break;
                    }
                }
            } catch (const std::exception& e) {
                threwTruncation = std::string(e.what()).find("truncat") != std::string::npos;
            }
            pool->closeNow();
            io.stop();  // abandon the server's stall so io.run() returns promptly
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(threwTruncation);
    RUVIA_CHECK(!endedCleanly);
}

// Drive two sequential fetches over a single pooled connection (poolSize == 1) against
// a server that keeps the connection open, exercising the keep-alive reuse path where
// the second request skips connectOne.
struct ReuseOutcome {
    bool ok = false;
    std::string body1;
    std::string body2;
    std::string error;
};

struct StreamCloseOutcome {
    bool beforeClose = false;
    bool afterClose = true;
    std::string error;
};

struct StreamReuseOutcome {
    bool ok = false;
    std::string body;
    std::string error;
};

ReuseOutcome runReuseFetch(std::string response1, std::string response2) {
    using asio::ip::tcp;
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    ReuseOutcome out;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto sock = co_await acceptor.async_accept(asio::use_awaitable);
                for (const auto* response : {&response1, &response2}) {
                    asio::streambuf request;
                    co_await asio::async_read_until(
                        sock, request, std::string("\r\n\r\n"), asio::use_awaitable);
                    co_await asio::async_write(sock, asio::buffer(*response), asio::use_awaitable);
                }
                std::error_code ignored;
                sock.shutdown(tcp::socket::shutdown_both, ignored);
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", std::pmr::get_default_resource());
    config.port = port;
    config.tls = false;
    config.poolSizePerWorker = 1;

    auto pool = std::make_unique<ruvia::detail::HttpClientPool>(
        io, std::move(config), std::pmr::get_default_resource());

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto r1 = co_await ruvia::detail::taskAsAwaitable(
                    pool->fetch("/one", options, std::pmr::get_default_resource()));
                out.body1.assign(r1.body().data(), r1.body().size());
                auto r2 = co_await ruvia::detail::taskAsAwaitable(
                    pool->fetch("/two", options, std::pmr::get_default_resource()));
                out.body2.assign(r2.body().data(), r2.body().size());
                out.ok = true;
            } catch (const std::exception& e) {
                out.error += std::string("client:") + e.what();
            }
            pool->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

StreamCloseOutcome runStreamCloseFetch() {
    using asio::ip::tcp;
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    StreamCloseOutcome out;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto sock = co_await acceptor.async_accept(asio::use_awaitable);
                asio::streambuf request;
                co_await asio::async_read_until(
                    sock, request, std::string("\r\n\r\n"), asio::use_awaitable);
                co_await asio::async_write(
                    sock,
                    asio::buffer(std::string("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello")),
                    asio::use_awaitable);
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", std::pmr::get_default_resource());
    config.port = port;
    config.tls = false;
    config.poolSizePerWorker = 1;

    auto pool = std::make_unique<ruvia::detail::HttpClientPool>(
        io, std::move(config), std::pmr::get_default_resource());

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto stream = co_await ruvia::detail::taskAsAwaitable(
                    pool->fetchStream("/", options, std::pmr::get_default_resource()));
                out.beforeClose = static_cast<bool>(stream);
                stream.close();
                out.afterClose = static_cast<bool>(stream);
            } catch (const std::exception& e) {
                out.error += std::string("client:") + e.what();
            }
            pool->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

StreamReuseOutcome runStreamContentLengthZeroWithExtraThenFetch() {
    using asio::ip::tcp;
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    StreamReuseOutcome out;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto first = std::make_shared<tcp::socket>(
                    co_await acceptor.async_accept(asio::use_awaitable));
                asio::streambuf firstRequest;
                co_await asio::async_read_until(
                    *first, firstRequest, std::string("\r\n\r\n"), asio::use_awaitable);
                co_await asio::async_write(
                    *first,
                    asio::buffer(std::string("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\nJUNK")),
                    asio::use_awaitable);
                std::error_code ignored;
                first->shutdown(tcp::socket::shutdown_both, ignored);
                first->close(ignored);

                auto second = co_await acceptor.async_accept(asio::use_awaitable);
                asio::streambuf secondRequest;
                co_await asio::async_read_until(
                    second, secondRequest, std::string("\r\n\r\n"), asio::use_awaitable);
                co_await asio::async_write(
                    second,
                    asio::buffer(std::string("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nFRESH")),
                    asio::use_awaitable);
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", std::pmr::get_default_resource());
    config.port = port;
    config.tls = false;
    config.poolSizePerWorker = 1;

    auto pool = std::make_unique<ruvia::detail::HttpClientPool>(
        io, std::move(config), std::pmr::get_default_resource());
    auto clientWatchdog = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(1000));
    clientWatchdog->async_wait([&](const std::error_code& ec) {
        if (!ec) {
            out.error += "client-timeout";
            pool->closeNow();
            std::error_code ignored;
            acceptor.close(ignored);
        }
    });

    asio::co_spawn(
        io,
        [&, clientWatchdog]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto stream = co_await ruvia::detail::taskAsAwaitable(
                    pool->fetchStream("/one", options, std::pmr::get_default_resource()));
                auto eof = co_await ruvia::detail::taskAsAwaitable(stream.readChunk());
                if (!eof.empty()) {
                    out.error += "unexpected-stream-body";
                }

                auto response = co_await ruvia::detail::taskAsAwaitable(
                    pool->fetch("/two", options, std::pmr::get_default_resource()));
                out.body.assign(response.body().data(), response.body().size());
                out.ok = true;
            } catch (const std::exception& e) {
                out.error += std::string("client:") + e.what();
                std::error_code ignored;
                acceptor.close(ignored);
            }
            std::error_code ignored;
            clientWatchdog->cancel(ignored);
            pool->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

StreamReuseOutcome runStreamConnectionCloseThenFetch() {
    using asio::ip::tcp;
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    StreamReuseOutcome out;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto first = std::make_shared<tcp::socket>(
                    co_await acceptor.async_accept(asio::use_awaitable));
                asio::streambuf firstRequest;
                co_await asio::async_read_until(
                    *first, firstRequest, std::string("\r\n\r\n"), asio::use_awaitable);
                co_await asio::async_write(
                    *first,
                    asio::buffer(std::string(
                        "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhello")),
                    asio::use_awaitable);
                std::error_code ignored;
                first->shutdown(tcp::socket::shutdown_both, ignored);
                first->close(ignored);

                auto second = co_await acceptor.async_accept(asio::use_awaitable);
                asio::streambuf secondRequest;
                co_await asio::async_read_until(
                    second, secondRequest, std::string("\r\n\r\n"), asio::use_awaitable);
                co_await asio::async_write(
                    second,
                    asio::buffer(std::string("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nFRESH")),
                    asio::use_awaitable);
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", std::pmr::get_default_resource());
    config.port = port;
    config.tls = false;
    config.poolSizePerWorker = 1;

    auto pool = std::make_unique<ruvia::detail::HttpClientPool>(
        io, std::move(config), std::pmr::get_default_resource());
    auto clientWatchdog = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(1000));
    clientWatchdog->async_wait([&](const std::error_code& ec) {
        if (!ec) {
            out.error += "client-timeout";
            pool->closeNow();
            std::error_code ignored;
            acceptor.close(ignored);
        }
    });

    asio::co_spawn(
        io,
        [&, clientWatchdog]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto stream = co_await ruvia::detail::taskAsAwaitable(
                    pool->fetchStream("/one", options, std::pmr::get_default_resource()));
                for (;;) {
                    auto chunk = co_await ruvia::detail::taskAsAwaitable(stream.readChunk());
                    if (chunk.empty()) {
                        break;
                    }
                }

                auto response = co_await ruvia::detail::taskAsAwaitable(
                    pool->fetch("/two", options, std::pmr::get_default_resource()));
                out.body.assign(response.body().data(), response.body().size());
                out.ok = true;
            } catch (const std::exception& e) {
                out.error += std::string("client:") + e.what();
                std::error_code ignored;
                acceptor.close(ignored);
            }
            std::error_code ignored;
            clientWatchdog->cancel(ignored);
            pool->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

struct RedirectOutcome {
    bool ok = false;
    int status = 0;
    std::string body;
    std::vector<std::string> requestLines;  // the request line (e.g. "GET /x HTTP/1.1") per hop
    std::string error;
};

// Serve `responses` in order (one per request received on a single kept-alive connection),
// substituting the literal "{PORT}" with the listening port, and drive ONE client fetch that
// may follow same-origin redirects across them.
RedirectOutcome runRedirectFetch(std::vector<std::string> responses, ruvia::FetchOptions options) {
    using asio::ip::tcp;
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    const std::string portText = std::to_string(port);

    RedirectOutcome out;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto sock = co_await acceptor.async_accept(asio::use_awaitable);
                for (auto response : responses) {
                    asio::streambuf request;
                    co_await asio::async_read_until(
                        sock, request, std::string("\r\n\r\n"), asio::use_awaitable);
                    const std::string text(
                        asio::buffers_begin(request.data()), asio::buffers_end(request.data()));
                    const auto lineEnd = text.find("\r\n");
                    out.requestLines.push_back(text.substr(0, lineEnd));
                    request.consume(request.size());
                    const auto headEnd = text.find("\r\n\r\n");
                    std::string pending = headEnd == std::string::npos
                        ? std::string{}
                        : text.substr(headEnd + 4);
                    auto readMore = [&](std::size_t bytes) -> asio::awaitable<void> {
                        while (pending.size() < bytes) {
                            char buffer[256];
                            const auto n = co_await sock.async_read_some(
                                asio::buffer(buffer), asio::use_awaitable);
                            pending.append(buffer, n);
                        }
                    };
                    auto readLine = [&]() -> asio::awaitable<std::string> {
                        for (;;) {
                            const auto crlf = pending.find("\r\n");
                            if (crlf != std::string::npos) {
                                std::string line = pending.substr(0, crlf);
                                pending.erase(0, crlf + 2);
                                co_return line;
                            }
                            char buffer[256];
                            const auto n = co_await sock.async_read_some(
                                asio::buffer(buffer), asio::use_awaitable);
                            pending.append(buffer, n);
                        }
                    };
                    auto headerValue = [&](std::string_view name) -> std::string_view {
                        std::size_t start = lineEnd == std::string::npos ? std::string::npos : lineEnd + 2;
                        while (start != std::string::npos && start < headEnd) {
                            const auto end = text.find("\r\n", start);
                            const auto line = std::string_view(text).substr(start, end - start);
                            const auto colon = line.find(':');
                            if (colon != std::string_view::npos &&
                                ruvia::detail::asciiEqualsIgnoreCase(line.substr(0, colon), name)) {
                                return ruvia::detail::httpTrimOws(line.substr(colon + 1));
                            }
                            if (end == std::string::npos) {
                                break;
                            }
                            start = end + 2;
                        }
                        return {};
                    };
                    if (const auto length = headerValue("Content-Length"); !length.empty()) {
                        std::size_t bodyBytes = 0;
                        const auto [ptr, ec] = std::from_chars(
                            length.data(), length.data() + length.size(), bodyBytes);
                        if (ec == std::errc{} && ptr == length.data() + length.size()) {
                            co_await readMore(bodyBytes);
                            pending.erase(0, bodyBytes);
                        }
                    } else if (ruvia::detail::asciiEqualsIgnoreCase(headerValue("Transfer-Encoding"), "chunked")) {
                        for (;;) {
                            const auto line = co_await readLine();
                            std::size_t chunkBytes = 0;
                            const auto [ptr, ec] = std::from_chars(
                                line.data(), line.data() + line.size(), chunkBytes, 16);
                            if (ec != std::errc{} || ptr != line.data() + line.size()) {
                                break;
                            }
                            if (chunkBytes == 0) {
                                (void)co_await readLine();
                                break;
                            }
                            co_await readMore(chunkBytes + 2);
                            pending.erase(0, chunkBytes + 2);
                        }
                    }
                    for (std::size_t at = response.find("{PORT}"); at != std::string::npos;
                         at = response.find("{PORT}")) {
                        response.replace(at, 6, portText);
                    }
                    co_await asio::async_write(sock, asio::buffer(response), asio::use_awaitable);
                }
                std::error_code ignored;
                sock.shutdown(tcp::socket::shutdown_both, ignored);
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", std::pmr::get_default_resource());
    config.port = port;
    config.tls = false;
    config.poolSizePerWorker = 1;

    auto pool = std::make_unique<ruvia::detail::HttpClientPool>(
        io, std::move(config), std::pmr::get_default_resource());

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto response = co_await ruvia::detail::taskAsAwaitable(
                    pool->fetch("/start", options, std::pmr::get_default_resource()));
                out.ok = true;
                out.status = response.status();
                out.body.assign(response.body().data(), response.body().size());
            } catch (const std::exception& e) {
                out.error += std::string("client:") + e.what();
            }
            pool->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

UploadOutcome runChunkedUploadFetch() {
    using asio::ip::tcp;
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    UploadOutcome out;
    TestBodyProducer producer({std::string_view("alpha"), std::string_view("beta")});

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto sock = co_await acceptor.async_accept(asio::use_awaitable);
                asio::streambuf requestHead;
                co_await asio::async_read_until(
                    sock, requestHead, std::string("\r\n\r\n"), asio::use_awaitable);
                std::string received(
                    asio::buffers_begin(requestHead.data()), asio::buffers_end(requestHead.data()));
                requestHead.consume(requestHead.size());
                const auto headEnd = received.find("\r\n\r\n");
                std::string head = received.substr(0, headEnd + 4);
                std::string pending = received.substr(headEnd + 4);

                std::string needle = "\r\nTransfer-Encoding:";
                for (auto at = head.find(needle); at != std::string::npos; at = head.find(needle, at + 1)) {
                    ++out.transferEncodingHeaders;
                }

                auto readUntilCrlf = [&]() -> asio::awaitable<std::string> {
                    for (;;) {
                        const auto crlf = pending.find("\r\n");
                        if (crlf != std::string::npos) {
                            std::string line = pending.substr(0, crlf);
                            pending.erase(0, crlf + 2);
                            co_return line;
                        }
                        char buffer[256];
                        const auto n = co_await sock.async_read_some(
                            asio::buffer(buffer), asio::use_awaitable);
                        pending.append(buffer, n);
                    }
                };
                auto readBytes = [&](std::size_t size) -> asio::awaitable<std::string> {
                    while (pending.size() < size) {
                        char buffer[256];
                        const auto n = co_await sock.async_read_some(
                            asio::buffer(buffer), asio::use_awaitable);
                        pending.append(buffer, n);
                    }
                    std::string bytes = pending.substr(0, size);
                    pending.erase(0, size);
                    co_return bytes;
                };

                for (;;) {
                    std::string line = co_await readUntilCrlf();
                    std::size_t chunkSize = 0;
                    const auto [ptr, ec] = std::from_chars(
                        line.data(), line.data() + line.size(), chunkSize, 16);
                    if (ec != std::errc{} || ptr != line.data() + line.size()) {
                        out.error += "bad-chunk-size";
                        break;
                    }
                    if (chunkSize == 0) {
                        (void)co_await readUntilCrlf();
                        break;
                    }
                    std::string chunk = co_await readBytes(chunkSize + 2);
                    out.body.append(chunk.data(), chunkSize);
                    if (chunk.substr(chunkSize) != "\r\n") {
                        out.error += "bad-chunk-crlf";
                        break;
                    }
                }
                co_await asio::async_write(
                    sock,
                    asio::buffer(std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK")),
                    asio::use_awaitable);
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", std::pmr::get_default_resource());
    config.port = port;
    config.tls = false;
    config.poolSizePerWorker = 1;

    auto pool = std::make_unique<ruvia::detail::HttpClientPool>(
        io, std::move(config), std::pmr::get_default_resource());

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                options.method = "POST";
                options.bodyStream = ruvia::RequestBodyStream(&producer, &TestBodyProducer::nextChunk);
                auto response = co_await ruvia::detail::taskAsAwaitable(
                    pool->fetch("/upload", options, std::pmr::get_default_resource()));
                out.ok = true;
                out.status = response.status();
            } catch (const std::exception& e) {
                out.error += std::string("client:") + e.what();
            }
            pool->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

}  // namespace

// --- Content-Length body round-trips end-to-end --------------------------
RUVIA_TEST(http_client_fetch_content_length) {
    const auto out = runOneFetch(
        "HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\nHello, world", WriteMode::kWhole);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 200);
    RUVIA_CHECK_EQ(out.body, std::string("Hello, world"));
}

RUVIA_TEST(http_client_fetch_skips_100_continue) {
    const auto out = runOneFetch(
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfinal",
        WriteMode::kWhole);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 200);
    RUVIA_CHECK_EQ(out.body, std::string("final"));
}

RUVIA_TEST(http_client_expect_continue_sends_body_after_100) {
    const auto out = runExpectContinueFetch(ContinueScenario::kSend100, std::chrono::milliseconds(0));
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK(out.serverGotExpect);
    RUVIA_CHECK_EQ(out.status, 200);
    RUVIA_CHECK_EQ(out.serverBody, std::string("hello-expect-body"));  // body was sent
    RUVIA_CHECK_EQ(out.body, std::string("hello-expect-body"));
}

RUVIA_TEST(http_client_expect_continue_final_status_skips_body) {
    // The server rejects before the body; the client must NOT send it and returns the 417.
    const auto out = runExpectContinueFetch(ContinueScenario::kRejectFinal, std::chrono::milliseconds(0));
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK(out.serverGotExpect);
    RUVIA_CHECK_EQ(out.status, 417);
    RUVIA_CHECK_EQ(out.body, std::string("too-big!"));
    RUVIA_CHECK(out.serverBody.empty());  // body was withheld
}

RUVIA_TEST(http_client_expect_continue_silent_server_sends_body_after_timeout) {
    // A server that never sends 100 must not deadlock the request: after the bounded wait the
    // client sends the body anyway (RFC 7231 §5.1.1).
    const auto out = runExpectContinueFetch(ContinueScenario::kSilent, std::chrono::milliseconds(150));
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 200);
    RUVIA_CHECK_EQ(out.serverBody, std::string("hello-expect-body"));
}

RUVIA_TEST(http_client_fetch_rejects_too_many_interim_responses) {
    std::string response;
    for (int i = 0; i < 9; ++i) {
        response += "HTTP/1.1 100 Continue\r\n\r\n";
    }
    response += "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfinal";

    const auto out = runOneFetch(std::move(response), WriteMode::kWhole);
    RUVIA_CHECK(!out.ok);
    RUVIA_CHECK(out.error.find("client:http client: too many interim responses") != std::string::npos);
}

RUVIA_TEST(http_client_rejects_hop_by_hop_request_headers) {
    for (const auto header : {
             ruvia::HttpHeaderView{"Keep-Alive", "timeout=5"},
             ruvia::HttpHeaderView{"Proxy-Connection", "keep-alive"},
             ruvia::HttpHeaderView{"TE", "trailers"},
             ruvia::HttpHeaderView{"Trailer", "Digest"},
             ruvia::HttpHeaderView{"Upgrade", "websocket"},
         }) {
        const auto out = runFetchWithRequestHeader(header);
        RUVIA_CHECK(!out.ok);
        RUVIA_CHECK(out.error.find("client:http client: request header is managed by the client") !=
                    std::string::npos);
    }
}

// --- Chunked body reassembly (data buffered with the head) ---------------
RUVIA_TEST(http_client_fetch_chunked_buffered) {
    const auto out = runOneFetch(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nHello\r\n"
        "7\r\n, world\r\n"
        "0\r\nX-Trailer: yes\r\n\r\n",
        WriteMode::kWhole);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 200);
    RUVIA_CHECK_EQ(out.body, std::string("Hello, world"));
}

// --- Chunked body reassembly (body arrives after the head, forcing socket
//     reads inside the chunk decoder) -----------------------------------
RUVIA_TEST(http_client_fetch_chunked_split) {
    const auto out = runOneFetch(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "1\r\nA\r\n"
        "3\r\nBCD\r\n"
        "a\r\n0123456789\r\n"
        "0\r\n\r\n",
        WriteMode::kHeadThenBody);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 200);
    RUVIA_CHECK_EQ(out.body, std::string("ABCD0123456789"));
}

// --- Chunked decode with every CRLF fragmented across reads ---------------
RUVIA_TEST(http_client_fetch_chunked_bytewise) {
    const auto out = runOneFetch(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nHello\r\n"
        "7\r\n, world\r\n"
        "0\r\nX-Trailer: yes\r\n\r\n",
        WriteMode::kByteWise);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 200);
    RUVIA_CHECK_EQ(out.body, std::string("Hello, world"));
}

// --- Empty chunked body (immediate last chunk) ---------------------------
RUVIA_TEST(http_client_fetch_chunked_empty) {
    const auto out = runOneFetch(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        WriteMode::kByteWise);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 200);
    RUVIA_CHECK(out.body.empty());
}

// --- Content-Length body delivered one byte at a time --------------------
RUVIA_TEST(http_client_fetch_content_length_bytewise) {
    const auto out = runOneFetch(
        "HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\nHello, world", WriteMode::kByteWise);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.body, std::string("Hello, world"));
}

// --- Chunked body exceeding the configured limit is rejected -------------
RUVIA_TEST(http_client_fetch_chunked_body_limit) {
    // Two 5-byte chunks (10 bytes total) against an 8-byte cap must be rejected.
    const auto out = runOneFetch(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nHello\r\n5\r\nworld\r\n0\r\n\r\n",
        WriteMode::kWhole,
        8);
    RUVIA_CHECK(!out.ok);
    RUVIA_CHECK(!out.error.empty());
}

// --- Close-delimited body (no Content-Length, no chunked; ends at EOF) ----
RUVIA_TEST(http_client_fetch_close_delimited) {
    // The mock server shuts the socket down after writing, signalling end-of-body.
    const auto out = runOneFetch(
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nHello, world", WriteMode::kWhole);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 200);
    RUVIA_CHECK_EQ(out.body, std::string("Hello, world"));
}

RUVIA_TEST(http_client_fetch_close_delimited_bytewise) {
    const auto out = runOneFetch(
        "HTTP/1.1 200 OK\r\n\r\nabcdefghijklmnopqrstuvwxyz", WriteMode::kByteWise);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.body, std::string("abcdefghijklmnopqrstuvwxyz"));
}

// --- Content-Encoding: gzip is transparently decoded (Content-Length) -----
RUVIA_TEST(http_client_fetch_gzip_content_length) {
    const std::string plain = "The quick brown fox jumps over the lazy dog.";
    const std::string gz = gzipCompress(plain);
    RUVIA_CHECK(!gz.empty());
    const auto out = runOneFetch(
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: " + lengthOf(gz) +
            "\r\n\r\n" + gz,
        WriteMode::kWhole);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.body, plain);
}

// --- Content-Encoding over chunked transfer (decode after de-chunking) ----
RUVIA_TEST(http_client_fetch_gzip_chunked) {
    const std::string plain = "The quick brown fox jumps over the lazy dog.";
    const std::string gz = gzipCompress(plain);
    RUVIA_CHECK(!gz.empty());
    // Send the gzip stream as a single chunk.
    char sizeHex[16];
    std::snprintf(sizeHex, sizeof(sizeHex), "%zx", gz.size());
    const auto out = runOneFetch(
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n" +
            std::string(sizeHex) + "\r\n" + gz + "\r\n0\r\n\r\n",
        WriteMode::kHeadThenBody);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.body, plain);
}

// --- Content-Encoding over a close-delimited body ------------------------
RUVIA_TEST(http_client_fetch_gzip_close_delimited) {
    const std::string plain = "compressed then closed";
    const std::string gz = gzipCompress(plain);
    RUVIA_CHECK(!gz.empty());
    const auto out = runOneFetch(
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n" + gz, WriteMode::kWhole);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.body, plain);
}

// --- A corrupt Content-Encoding stream is rejected -----------------------
RUVIA_TEST(http_client_fetch_gzip_corrupt_rejected) {
    const auto out = runOneFetch(
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 5\r\n\r\nnotgz",
        WriteMode::kWhole);
    RUVIA_CHECK(!out.ok);
    RUVIA_CHECK(!out.error.empty());
}

// --- A slow/stalled body read trips the total request deadline -----------
RUVIA_TEST(http_client_fetch_request_timeout) {
    using asio::ip::tcp;
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    bool timedOut = false;
    bool succeeded = false;

    // Server: send the head plus a partial close-delimited body, then hold the connection
    // open (never closing) so the client's read stalls until its deadline fires.
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto sock = co_await acceptor.async_accept(asio::use_awaitable);
                asio::streambuf request;
                co_await asio::async_read_until(
                    sock, request, std::string("\r\n\r\n"), asio::use_awaitable);
                co_await asio::async_write(
                    sock, asio::buffer(std::string("HTTP/1.1 200 OK\r\n\r\npartial")),
                    asio::use_awaitable);
                // Block on a read; it unblocks with EOF once the client closes after timing out.
                char dummy[64];
                co_await sock.async_read_some(asio::buffer(dummy), asio::use_awaitable);
            } catch (...) {
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", std::pmr::get_default_resource());
    config.port = port;
    config.tls = false;
    config.poolSizePerWorker = 1;
    config.requestTimeout = std::chrono::milliseconds(100);

    auto pool = std::make_unique<ruvia::detail::HttpClientPool>(
        io, std::move(config), std::pmr::get_default_resource());

    // Drive the deadline scanner periodically (normally the server's connection scanner does).
    auto scanTimer = std::make_shared<asio::steady_timer>(io);
    std::function<void()> armScan = [&]() {
        scanTimer->expires_after(std::chrono::milliseconds(10));
        scanTimer->async_wait([&](const std::error_code& ec) {
            if (ec) {
                return;
            }
            pool->scanDeadlines(std::chrono::steady_clock::now());
            armScan();
        });
    };
    armScan();

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                (void)co_await ruvia::detail::taskAsAwaitable(
                    pool->fetch("/", options, std::pmr::get_default_resource()));
                succeeded = true;
            } catch (...) {
                timedOut = true;
            }
            scanTimer->cancel();
            pool->closeNow();
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(timedOut);
    RUVIA_CHECK(!succeeded);
}

// --- Keep-alive connection reuse across two requests ---------------------
RUVIA_TEST(http_client_fetch_connection_reuse) {
    // Mix framings: a Content-Length response then a chunked one, both on one connection.
    const auto out = runReuseFetch(
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n6\r\nsecond\r\n0\r\n\r\n");
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.body1, std::string("first"));
    RUVIA_CHECK_EQ(out.body2, std::string("second"));
}

// --- Streamed request body is sent as exactly one chunked transfer --------
RUVIA_TEST(http_client_fetch_streamed_request_body_chunked_once) {
    const auto out = runChunkedUploadFetch();
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 200);
    RUVIA_CHECK_EQ(out.transferEncodingHeaders, 1);
    RUVIA_CHECK_EQ(out.body, std::string("alphabeta"));
}

// --- Redirect following (same-origin only) -------------------------------
RUVIA_TEST(http_client_redirect_root_relative) {
    ruvia::FetchOptions options;  // maxRedirects defaults to 5
    const auto out = runRedirectFetch(
        {"HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: 0\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nafter"},
        options);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 200);
    RUVIA_CHECK_EQ(out.body, std::string("after"));
    RUVIA_CHECK_EQ(out.requestLines.size(), std::size_t{2});
    if (out.requestLines.size() == 2) {
        RUVIA_CHECK_EQ(out.requestLines[0], std::string("GET /start HTTP/1.1"));
        RUVIA_CHECK_EQ(out.requestLines[1], std::string("GET /final HTTP/1.1"));
    }
}

RUVIA_TEST(http_client_redirect_absolute_same_origin) {
    ruvia::FetchOptions options;
    const auto out = runRedirectFetch(
        {"HTTP/1.1 301 Moved\r\nLocation: http://127.0.0.1:{PORT}/moved\r\nContent-Length: 0\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK"},
        options);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 200);
    RUVIA_CHECK_EQ(out.body, std::string("OK"));
    if (out.requestLines.size() == 2) {
        RUVIA_CHECK_EQ(out.requestLines[1], std::string("GET /moved HTTP/1.1"));
    }
}

RUVIA_TEST(http_client_redirect_cross_origin_not_followed) {
    ruvia::FetchOptions options;
    // A different host must NOT be followed; the 3xx is returned to the caller.
    const auto out = runRedirectFetch(
        {"HTTP/1.1 302 Found\r\nLocation: http://example.com/x\r\nContent-Length: 0\r\n\r\n"},
        options);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 302);
    RUVIA_CHECK_EQ(out.requestLines.size(), std::size_t{1});
}

RUVIA_TEST(http_client_redirect_cross_port_not_followed) {
    ruvia::FetchOptions options;
    const auto out = runRedirectFetch(
        {"HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/x\r\nContent-Length: 0\r\n\r\n"},
        options);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK_EQ(out.status, 302);
    RUVIA_CHECK_EQ(out.requestLines.size(), std::size_t{1});
}

RUVIA_TEST(http_client_redirect_303_post_becomes_get) {
    ruvia::FetchOptions options;
    options.method = "POST";
    options.body = "payload";
    const auto out = runRedirectFetch(
        {"HTTP/1.1 303 See Other\r\nLocation: /result\r\nContent-Length: 0\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone"},
        options);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.body, std::string("done"));
    if (out.requestLines.size() == 2) {
        RUVIA_CHECK_EQ(out.requestLines[0], std::string("POST /start HTTP/1.1"));
        RUVIA_CHECK_EQ(out.requestLines[1], std::string("GET /result HTTP/1.1"));
    }
}

RUVIA_TEST(http_client_redirect_303_get_body_stream_becomes_bodyless_get) {
    TestBodyProducer producer({std::string_view("payload")});
    ruvia::FetchOptions options;
    options.method = "GET";
    options.bodyStream = ruvia::RequestBodyStream(&producer, &TestBodyProducer::nextChunk);
    const auto out = runRedirectFetch(
        {"HTTP/1.1 303 See Other\r\nLocation: /result\r\nContent-Length: 0\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone"},
        options);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 200);
    RUVIA_CHECK_EQ(out.body, std::string("done"));
    RUVIA_CHECK_EQ(out.requestLines.size(), std::size_t{2});
    if (out.requestLines.size() == 2) {
        RUVIA_CHECK_EQ(out.requestLines[0], std::string("GET /start HTTP/1.1"));
        RUVIA_CHECK_EQ(out.requestLines[1], std::string("GET /result HTTP/1.1"));
    }
}

RUVIA_TEST(http_client_redirect_307_preserves_method) {
    ruvia::FetchOptions options;
    options.method = "POST";
    options.body = "payload";
    const auto out = runRedirectFetch(
        {"HTTP/1.1 307 Temporary Redirect\r\nLocation: /again\r\nContent-Length: 0\r\n\r\n",
         "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi"},
        options);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    if (out.requestLines.size() == 2) {
        RUVIA_CHECK_EQ(out.requestLines[1], std::string("POST /again HTTP/1.1"));
    }
}

RUVIA_TEST(http_client_redirect_307_does_not_replay_body_stream) {
    TestBodyProducer producer({std::string_view("payload")});
    ruvia::FetchOptions options;
    options.method = "POST";
    options.bodyStream = ruvia::RequestBodyStream(&producer, &TestBodyProducer::nextChunk);
    const auto out = runRedirectFetch(
        {"HTTP/1.1 307 Temporary Redirect\r\nLocation: /again\r\nContent-Length: 0\r\n\r\n"},
        options);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 307);
    RUVIA_CHECK_EQ(out.requestLines.size(), std::size_t{1});
}

RUVIA_TEST(http_client_redirect_hop_limit) {
    ruvia::FetchOptions options;
    options.maxRedirects = 1;
    // Two redirects but only one hop allowed: the second 302 is returned unfollowed.
    const auto out = runRedirectFetch(
        {"HTTP/1.1 302 Found\r\nLocation: /b\r\nContent-Length: 0\r\n\r\n",
         "HTTP/1.1 302 Found\r\nLocation: /c\r\nContent-Length: 0\r\n\r\n"},
        options);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK_EQ(out.status, 302);
    RUVIA_CHECK_EQ(out.requestLines.size(), std::size_t{2});
}

RUVIA_TEST(http_client_redirect_disabled) {
    ruvia::FetchOptions options;
    options.maxRedirects = 0;
    const auto out = runRedirectFetch(
        {"HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: 0\r\n\r\n"},
        options);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK_EQ(out.status, 302);
    RUVIA_CHECK_EQ(out.requestLines.size(), std::size_t{1});
}

RUVIA_TEST(http_client_redirect_no_location_returned) {
    ruvia::FetchOptions options;
    const auto out = runRedirectFetch(
        {"HTTP/1.1 302 Found\r\nContent-Length: 0\r\n\r\n"},
        options);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK_EQ(out.status, 302);
    RUVIA_CHECK_EQ(out.requestLines.size(), std::size_t{1});
}

RUVIA_TEST(http_client_redirect_duplicate_location_not_followed) {
    ruvia::FetchOptions options;
    const auto out = runRedirectFetch(
        {"HTTP/1.1 302 Found\r\nLocation: /first\r\nLocation: /second\r\nContent-Length: 0\r\n\r\n"},
        options);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 302);
    RUVIA_CHECK_EQ(out.requestLines.size(), std::size_t{1});
}

// --- Streaming: Content-Length body pulled incrementally -----------------
RUVIA_TEST(http_client_stream_content_length) {
    const std::string body(5000, 'C');
    const auto out = runStreamFetch(
        "HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n" + body, WriteMode::kWhole);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.status, 200);
    RUVIA_CHECK_EQ(out.body.size(), std::size_t{5000});
    RUVIA_CHECK_EQ(out.body, body);
}

// --- Streaming: chunked body pulled incrementally, bytewise on the wire ---
RUVIA_TEST(http_client_stream_chunked) {
    const auto out = runStreamFetch(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nHello\r\n7\r\n, world\r\n0\r\n\r\n",
        WriteMode::kByteWise);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.body, std::string("Hello, world"));
}

// --- Streaming: close-delimited body (ends at EOF) -----------------------
RUVIA_TEST(http_client_stream_close_delimited) {
    const std::string body(3000, 'Z');
    const auto out = runStreamFetch(
        "HTTP/1.1 200 OK\r\n\r\n" + body, WriteMode::kWhole);
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.body.size(), std::size_t{3000});
    RUVIA_CHECK_EQ(out.body, body);
}

RUVIA_TEST(http_client_stream_close_releases_source) {
    const auto out = runStreamCloseFetch();
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.beforeClose);
    RUVIA_CHECK(!out.afterClose);
}

RUVIA_TEST(http_client_stream_cl0_with_extra_bytes_discards_connection) {
    const auto out = runStreamContentLengthZeroWithExtraThenFetch();
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.body, std::string("FRESH"));
}

RUVIA_TEST(http_client_stream_connection_close_discards_connection) {
    const auto out = runStreamConnectionCloseThenFetch();
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(out.ok);
    RUVIA_CHECK_EQ(out.body, std::string("FRESH"));
}

// --- Both Content-Length and Transfer-Encoding: rejected (smuggling) ------
RUVIA_TEST(http_client_fetch_rejects_cl_and_te) {
    const auto out = runOneFetch(
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nHello\r\n0\r\n\r\n",
        WriteMode::kWhole);
    RUVIA_CHECK(!out.ok);
    RUVIA_CHECK(!out.error.empty());
}

#endif  // RUVIA_ENABLE_HTTP_CLIENT
