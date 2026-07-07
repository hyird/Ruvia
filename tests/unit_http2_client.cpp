#include "test_harness.h"

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>

#include "client/Http2ClientSession.h"
#include "net/http2/Http2FrameCodec.h"
#include "net/http2/Http2FramePayload.h"
#include "net/http2/Http2FrameTypes.h"
#include "net/http2/Http2Hpack.h"
#include "runtime/AsioAwait.h"
#include "ruvia/http/HttpClient.h"

namespace {

using asio::ip::tcp;
using namespace ruvia::detail;

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

// A minimal cleartext HTTP/2 server for tests. It completes the preface/SETTINGS handshake,
// then for every request stream responds 200 with a body of `x-echo` (if present) else the
// request `:path`. Response bodies larger than one frame are split to exercise reassembly.
asio::awaitable<void> mockH2Server(tcp::socket sock, std::pmr::memory_resource* resource) {
    auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
        auto [ec, n] = co_await asio::async_read(
            sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
        co_return !ec && n == size;
    };
    auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
        auto [ec, n] = co_await asio::async_write(
            sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
        co_return !ec;
    };

    char preface[24];
    if (!co_await readExact(preface, sizeof(preface))) {
        co_return;
    }
    // Server SETTINGS must be the first frame we send.
    char settings[kHttp2FrameHeaderBytes];
    http2WriteFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
    if (!co_await writeAll(std::string_view(settings, sizeof(settings)))) {
        co_return;
    }

    HpackDecoder decoder(resource);
    for (;;) {
        char headerBytes[kHttp2FrameHeaderBytes];
        if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
            break;
        }
        const auto header = http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
        std::string payload(header.length, '\0');
        if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
            break;
        }
        const auto type = static_cast<Http2FrameType>(header.type);
        if (type == Http2FrameType::kSettings) {
            if ((header.flags & kHttp2FlagAck) == 0) {
                char ack[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(ack, 0, Http2FrameType::kSettings, kHttp2FlagAck, 0);
                if (!co_await writeAll(std::string_view(ack, sizeof(ack)))) {
                    break;
                }
            }
            continue;
        }
        if (type != Http2FrameType::kHeaders) {
            continue;  // ignore WINDOW_UPDATE / DATA / RST / PING for these tests
        }

        std::string_view fragment;
        if (!http2DecodeHeadersPayload(header, payload, fragment)) {
            break;
        }
        struct Ctx {
            std::string path;
            std::string echo;
            bool haveEcho = false;
        } ctx;
        const auto decodeResult = decoder.decode(fragment, &ctx, [](void* target, std::string_view name, std::string_view value) {
            auto* c = static_cast<Ctx*>(target);
            if (name == ":path") {
                c->path.assign(value);
            } else if (name == "x-echo") {
                c->echo.assign(value);
                c->haveEcho = true;
            }
            return true;
        });
        if (!decodeResult.ok()) {
            co_return;
        }
        std::string body;
        if (ctx.path == "/large") {
            body.assign(40000, 'y');  // spans multiple 16 KiB DATA frames
        } else if (ctx.path == "/gzip") {
            body = gzipCompress("compressed h2");
        } else if (ctx.path == "/content-length-short") {
            body = "abc";
        } else if (ctx.haveEcho) {
            body = ctx.echo;
        } else {
            body = ctx.path;
        }

        if (ctx.path == "/early-hints" || ctx.path == "/many-early-hints" || ctx.path == "/switching-protocols") {
            const int count = ctx.path == "/many-early-hints" ? 9 : 1;
            for (int i = 0; i < count; ++i) {
                std::pmr::string early(resource);
                if (ctx.path == "/switching-protocols") {
                    HpackEncoder::encodeStatus(early, 101);
                } else {
                    HpackEncoder::encodeStatus(early, 103);
                    HpackEncoder::encodeHeader(early, "link", "</style.css>; rel=preload");
                }
                char earlyHeaders[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(
                    earlyHeaders, static_cast<std::uint32_t>(early.size()),
                    Http2FrameType::kHeaders, kHttp2FlagEndHeaders, header.streamId);
                if (!co_await writeAll(std::string_view(earlyHeaders, sizeof(earlyHeaders)))) {
                    break;
                }
                if (!co_await writeAll(std::string_view(early.data(), early.size()))) {
                    break;
                }
            }
        }

        std::pmr::string block(resource);
        const bool noContentWithData = ctx.path == "/no-content-data";
        const bool resetContentWithData = ctx.path == "/reset-content-data";
        if (ctx.path == "/redirect") {
            HpackEncoder::encodeStatus(block, 302);
            HpackEncoder::encodeHeader(block, "location", "/final");
        } else if (ctx.path == "/late-status") {
            HpackEncoder::encodeHeader(block, "x-before-status", "1");
            HpackEncoder::encodeStatus(block, 200);
        } else if (ctx.path == "/wide-status") {
            HpackEncoder::encodeHeaderWithNameIndex(block, HpackStaticIndex::kStatus200, "0200");
        } else if (noContentWithData) {
            HpackEncoder::encodeStatus(block, 204);
        } else if (resetContentWithData) {
            HpackEncoder::encodeStatus(block, 205);
        } else {
            HpackEncoder::encodeStatus(block, 200);
            if (ctx.path == "/gzip") {
                HpackEncoder::encodeHeader(block, "content-encoding", "gzip");
            } else if (ctx.path == "/bad-connection-header") {
                HpackEncoder::encodeHeader(block, "connection", "close");
            } else if (ctx.path == "/bad-te-header") {
                HpackEncoder::encodeHeader(block, "te", "gzip");
            } else if (ctx.path == "/bad-cr-header") {
                HpackEncoder::encodeHeader(block, "x-bad", std::string_view("ok\rbad", 6));
            } else if (ctx.path == "/content-length-short") {
                HpackEncoder::encodeHeader(block, "content-length", "5");
            }
        }
        char responseHeaders[kHttp2FrameHeaderBytes];
        const bool redirect = ctx.path == "/redirect";
        http2WriteFrameHeader(
            responseHeaders, static_cast<std::uint32_t>(block.size()),
            Http2FrameType::kHeaders,
            static_cast<std::uint8_t>(kHttp2FlagEndHeaders | (redirect ? kHttp2FlagEndStream : 0)),
            header.streamId);
        if (!co_await writeAll(std::string_view(responseHeaders, sizeof(responseHeaders)))) {
            break;
        }
        if (!co_await writeAll(std::string_view(block.data(), block.size()))) {
            break;
        }
        if (redirect) {
            continue;
        }

        // "/timeout" gets headers only and never an END_STREAM — the client must time out.
        if (ctx.path == "/timeout") {
            continue;
        }

        // "/trailer" ends the stream with a trailing HEADERS block instead of END_STREAM on DATA.
        const bool useTrailer = (ctx.path == "/trailer");
        const bool useBadTrailer = (ctx.path == "/bad-trailer");
        const bool useBadTrailerConnection = (ctx.path == "/bad-trailer-connection-header");
        const bool useBadTrailerContentType = (ctx.path == "/bad-trailer-content-type");
        if (noContentWithData || resetContentWithData) {
            body = "illegal-body";
        }

        // Emit the body in <= 16 KiB DATA frames; END_STREAM on the last unless trailers follow.
        constexpr std::size_t kMaxFrame = 16 * 1024;
        std::size_t offset = 0;
        do {
            const std::size_t chunk = std::min(body.size() - offset, kMaxFrame);
            const bool last = (offset + chunk == body.size());
            const bool endsWithData =
                last &&
                !useTrailer &&
                !useBadTrailer &&
                !useBadTrailerConnection &&
                !useBadTrailerContentType;
            char dataHeader[kHttp2FrameHeaderBytes];
            http2WriteFrameHeader(
                dataHeader, static_cast<std::uint32_t>(chunk), Http2FrameType::kData,
                static_cast<std::uint8_t>(endsWithData ? kHttp2FlagEndStream : 0),
                header.streamId);
            if (!co_await writeAll(std::string_view(dataHeader, sizeof(dataHeader)))) {
                co_return;
            }
            if (chunk != 0 && !co_await writeAll(std::string_view(body.data() + offset, chunk))) {
                co_return;
            }
            offset += chunk;
        } while (offset < body.size());

        if (useTrailer || useBadTrailer || useBadTrailerConnection || useBadTrailerContentType) {
            std::pmr::string trailerBlock(resource);
            if (useBadTrailer) {
                HpackEncoder::encodeStatus(trailerBlock, 200);
            } else if (useBadTrailerConnection) {
                HpackEncoder::encodeHeader(trailerBlock, "connection", "close");
            } else if (useBadTrailerContentType) {
                HpackEncoder::encodeHeader(trailerBlock, "content-type", "text/plain");
            }
            char trailer[kHttp2FrameHeaderBytes];
            http2WriteFrameHeader(
                trailer, static_cast<std::uint32_t>(trailerBlock.size()), Http2FrameType::kHeaders,
                static_cast<std::uint8_t>(kHttp2FlagEndHeaders | kHttp2FlagEndStream), header.streamId);
            if (!co_await writeAll(std::string_view(trailer, sizeof(trailer)))) {
                co_return;
            }
            if (!trailerBlock.empty() && !co_await writeAll(std::string_view(trailerBlock.data(), trailerBlock.size()))) {
                co_return;
            }
        }
    }
}

struct H2Result {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

struct H2BadTrailerOutcome {
    bool clientFailed = false;
    bool serverSawClose = false;
    bool timedOut = false;
    std::string error;
};

struct H2BadFrameOutcome {
    bool clientFailed = false;
    bool serverSawClose = false;
    bool timedOut = false;
    int status = 0;
    std::string body;
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

std::string runH2UploadFetch() {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    std::string responseBody;
    std::string error;
    TestBodyProducer producer({std::string_view("alpha"), std::string_view("beta")});

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };

            char preface[24];
            if (!co_await readExact(preface, sizeof(preface))) {
                co_return;
            }
            char settings[kHttp2FrameHeaderBytes];
            http2WriteFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
            if (!co_await writeAll(std::string_view(settings, sizeof(settings)))) {
                co_return;
            }

            std::uint32_t requestStream = 0;
            bool bodyDone = false;
            std::string uploaded;
            HpackDecoder decoder(resource);
            while (!bodyDone) {
                char headerBytes[kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                    co_return;
                }
                const auto header = http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                    co_return;
                }
                const auto type = static_cast<Http2FrameType>(header.type);
                if (type == Http2FrameType::kSettings) {
                    if ((header.flags & kHttp2FlagAck) == 0) {
                        char ack[kHttp2FrameHeaderBytes];
                        http2WriteFrameHeader(ack, 0, Http2FrameType::kSettings, kHttp2FlagAck, 0);
                        if (!co_await writeAll(std::string_view(ack, sizeof(ack)))) {
                            co_return;
                        }
                    }
                    continue;
                }
                if (type == Http2FrameType::kWindowUpdate) {
                    continue;
                }
                if (type == Http2FrameType::kHeaders) {
                    requestStream = header.streamId;
                    std::string_view fragment;
                    if (!http2DecodeHeadersPayload(header, payload, fragment)) {
                        co_return;
                    }
                    struct DecodeCtx {};
                    DecodeCtx ctx;
                    const auto decoded = decoder.decode(
                        fragment, &ctx,
                        [](void*, std::string_view, std::string_view) { return true; });
                    if (!decoded.ok()) {
                        co_return;
                    }
                    bodyDone = (header.flags & kHttp2FlagEndStream) != 0;
                    continue;
                }
                if (type == Http2FrameType::kData && header.streamId == requestStream) {
                    uploaded.append(payload.data(), payload.size());
                    bodyDone = (header.flags & kHttp2FlagEndStream) != 0;
                }
            }

            std::pmr::string block(resource);
            HpackEncoder::encodeStatus(block, 200);
            char responseHeaders[kHttp2FrameHeaderBytes];
            http2WriteFrameHeader(
                responseHeaders, static_cast<std::uint32_t>(block.size()),
                Http2FrameType::kHeaders, kHttp2FlagEndHeaders, requestStream);
            if (!co_await writeAll(std::string_view(responseHeaders, sizeof(responseHeaders)))) {
                co_return;
            }
            if (!co_await writeAll(std::string_view(block.data(), block.size()))) {
                co_return;
            }
            char dataHeader[kHttp2FrameHeaderBytes];
            http2WriteFrameHeader(
                dataHeader, static_cast<std::uint32_t>(uploaded.size()),
                Http2FrameType::kData, kHttp2FlagEndStream, requestStream);
            if (!co_await writeAll(std::string_view(dataHeader, sizeof(dataHeader)))) {
                co_return;
            }
            (void)co_await writeAll(uploaded);
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                options.method = "POST";
                options.bodyStream = ruvia::RequestBodyStream(&producer, &TestBodyProducer::nextChunk);
                auto response = co_await taskAsAwaitable(session->fetch("/upload", options, resource));
                responseBody.assign(response.body().data(), response.body().size());
            } catch (const std::exception& e) {
                error = e.what();
            }
            session->closeNow();
        },
        asio::detached);

    io.run();
    if (!error.empty()) {
        return std::string("error:") + error;
    }
    return responseBody;
}

// Server advertises a tiny stream window (4 bytes) and sends a full response BEFORE consuming the
// upload and without any WINDOW_UPDATE. The client sends 4 bytes, blocks on the send window, and
// must abandon the upload once the response arrives — a regression guard against the streamed-
// upload hang. Returns the response body ("done") on success, "error:<...>" otherwise.
std::string runH2EarlyResponseUpload() {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    std::string responseBody;
    std::string error;
    const std::string_view bigChunk(
        "0123456789012345678901234567890123456789");  // 40 bytes, far exceeding the 4-byte window
    TestBodyProducer producer({bigChunk});

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };

            char preface[24];
            if (!co_await readExact(preface, sizeof(preface))) {
                co_return;
            }
            // SETTINGS advertising INITIAL_WINDOW_SIZE = 4.
            std::array<char, kHttp2FrameHeaderBytes + 6> settings;
            char* out = http2WriteFrameHeader(settings.data(), 6, Http2FrameType::kSettings, 0, 0);
            http2WriteSettingsEntry(out, Http2SettingId::kInitialWindowSize, 4);
            if (!co_await writeAll(std::string_view(settings.data(), settings.size()))) {
                co_return;
            }

            // Read until the client's HEADERS, then respond fully (no WINDOW_UPDATE, no body read).
            std::uint32_t requestStream = 0;
            HpackDecoder decoder(resource);
            while (requestStream == 0) {
                char headerBytes[kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                    co_return;
                }
                const auto header = http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                    co_return;
                }
                const auto type = static_cast<Http2FrameType>(header.type);
                if (type == Http2FrameType::kSettings && (header.flags & kHttp2FlagAck) == 0) {
                    char ack[kHttp2FrameHeaderBytes];
                    http2WriteFrameHeader(ack, 0, Http2FrameType::kSettings, kHttp2FlagAck, 0);
                    if (!co_await writeAll(std::string_view(ack, sizeof(ack)))) {
                        co_return;
                    }
                } else if (type == Http2FrameType::kHeaders) {
                    requestStream = header.streamId;
                    std::string_view fragment;
                    if (!http2DecodeHeadersPayload(header, payload, fragment)) {
                        co_return;
                    }
                    struct DecodeCtx {} ctx;
                    (void)decoder.decode(fragment, &ctx,
                        [](void*, std::string_view, std::string_view) { return true; });
                }
            }

            std::pmr::string block(resource);
            HpackEncoder::encodeStatus(block, 200);
            char responseHeaders[kHttp2FrameHeaderBytes];
            http2WriteFrameHeader(
                responseHeaders, static_cast<std::uint32_t>(block.size()),
                Http2FrameType::kHeaders, kHttp2FlagEndHeaders, requestStream);
            if (!co_await writeAll(std::string_view(responseHeaders, sizeof(responseHeaders)))) {
                co_return;
            }
            if (!co_await writeAll(std::string_view(block.data(), block.size()))) {
                co_return;
            }
            const std::string_view respBody("done");
            char dataHeader[kHttp2FrameHeaderBytes];
            http2WriteFrameHeader(
                dataHeader, static_cast<std::uint32_t>(respBody.size()),
                Http2FrameType::kData, kHttp2FlagEndStream, requestStream);
            if (!co_await writeAll(std::string_view(dataHeader, sizeof(dataHeader)))) {
                co_return;
            }
            (void)co_await writeAll(respBody);
            // Drain whatever the client sends (RST/DATA) until it closes.
            char scratch[256];
            for (;;) {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(scratch), asio::as_tuple(asio::use_awaitable));
                (void)n;
                if (ec) {
                    break;
                }
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                options.method = "POST";
                options.bodyStream = ruvia::RequestBodyStream(&producer, &TestBodyProducer::nextChunk);
                auto response = co_await taskAsAwaitable(session->fetch("/upload", options, resource));
                responseBody.assign(response.body().data(), response.body().size());
            } catch (const std::exception& e) {
                error = e.what();
            }
            session->closeNow();
        },
        asio::detached);

    io.run();
    if (!error.empty()) {
        return std::string("error:") + error;
    }
    return responseBody;
}

// A single SETTINGS frame may carry SETTINGS_INITIAL_WINDOW_SIZE more than once
// (RFC 9113 6.5.3, processed in order). The server advertises a 2-byte stream
// window, the client sends 2 body bytes and blocks, then the server sends one
// SETTINGS frame with two INITIAL_WINDOW_SIZE entries (5 then 10). Each live
// stream's send window must move by the sum of the per-entry deltas
// ((5-2)+(10-5)=8), not just the last delta (10-5=5). With the sum applied the
// client unblocks and uploads the full 10-byte body; keeping only the last delta
// leaves the window 3 bytes short so END_STREAM never arrives (RFC 9113 6.9.2).
// Returns the full body the server received, or "" on the flow-control deadlock.
std::string runH2InitialWindowMultiEntryUpload() {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    std::string uploaded;
    bool sawEndStream = false;
    const std::string_view fullBody("0123456789");  // 10 bytes
    TestBodyProducer producer({fullBody});

    asio::steady_timer watchdog(io);
    watchdog.expires_after(std::chrono::seconds(3));
    watchdog.async_wait([&io](const std::error_code& ec) {
        if (!ec) {
            io.stop();  // break the flow-control deadlock the buggy path would hang on
        }
    });

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };

            char preface[24];
            if (!co_await readExact(preface, sizeof(preface))) {
                co_return;
            }
            // Initial SETTINGS advertising INITIAL_WINDOW_SIZE = 2.
            std::array<char, kHttp2FrameHeaderBytes + 6> settings;
            char* out = http2WriteFrameHeader(settings.data(), 6, Http2FrameType::kSettings, 0, 0);
            http2WriteSettingsEntry(out, Http2SettingId::kInitialWindowSize, 2);
            if (!co_await writeAll(std::string_view(settings.data(), settings.size()))) {
                co_return;
            }

            std::uint32_t requestStream = 0;
            bool secondSettingsSent = false;
            HpackDecoder decoder(resource);
            while (!sawEndStream) {
                char headerBytes[kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                    co_return;
                }
                const auto header = http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                    co_return;
                }
                const auto type = static_cast<Http2FrameType>(header.type);
                if (type == Http2FrameType::kSettings) {
                    if ((header.flags & kHttp2FlagAck) == 0) {
                        char ack[kHttp2FrameHeaderBytes];
                        http2WriteFrameHeader(ack, 0, Http2FrameType::kSettings, kHttp2FlagAck, 0);
                        if (!co_await writeAll(std::string_view(ack, sizeof(ack)))) {
                            co_return;
                        }
                    }
                    continue;
                }
                if (type == Http2FrameType::kWindowUpdate) {
                    continue;
                }
                if (type == Http2FrameType::kHeaders) {
                    requestStream = header.streamId;
                    std::string_view fragment;
                    if (!http2DecodeHeadersPayload(header, payload, fragment)) {
                        co_return;
                    }
                    struct DecodeCtx {} ctx;
                    (void)decoder.decode(fragment, &ctx,
                        [](void*, std::string_view, std::string_view) { return true; });
                    sawEndStream = (header.flags & kHttp2FlagEndStream) != 0;
                    continue;
                }
                if (type == Http2FrameType::kData && header.streamId == requestStream) {
                    uploaded.append(payload.data(), payload.size());
                    sawEndStream = (header.flags & kHttp2FlagEndStream) != 0;
                    // Once the client has spent the tiny initial window, enlarge it
                    // with a single SETTINGS frame carrying two INITIAL_WINDOW_SIZE
                    // entries (5 then 10) so the delta accounting is exercised.
                    if (!secondSettingsSent && uploaded.size() >= 2 && !sawEndStream) {
                        secondSettingsSent = true;
                        std::array<char, kHttp2FrameHeaderBytes + 12> grow;
                        char* growOut =
                            http2WriteFrameHeader(grow.data(), 12, Http2FrameType::kSettings, 0, 0);
                        growOut = http2WriteSettingsEntry(growOut, Http2SettingId::kInitialWindowSize, 5);
                        http2WriteSettingsEntry(growOut, Http2SettingId::kInitialWindowSize, 10);
                        if (!co_await writeAll(std::string_view(grow.data(), grow.size()))) {
                            co_return;
                        }
                    }
                }
            }

            std::pmr::string block(resource);
            HpackEncoder::encodeStatus(block, 200);
            char responseHeaders[kHttp2FrameHeaderBytes];
            http2WriteFrameHeader(
                responseHeaders, static_cast<std::uint32_t>(block.size()),
                Http2FrameType::kHeaders, kHttp2FlagEndHeaders | kHttp2FlagEndStream, requestStream);
            (void)co_await writeAll(std::string_view(responseHeaders, sizeof(responseHeaders)));
            (void)co_await writeAll(std::string_view(block.data(), block.size()));
            watchdog.cancel();
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                options.method = "POST";
                options.bodyStream = ruvia::RequestBodyStream(&producer, &TestBodyProducer::nextChunk);
                (void)co_await taskAsAwaitable(session->fetch("/upload", options, resource));
            } catch (const std::exception&) {
            }
            session->closeNow();
        },
        asio::detached);

    io.run();
    return sawEndStream ? uploaded : std::string{};
}

// Run `count` concurrent GETs to the given paths against the mock server on one h2 session.
std::vector<H2Result> runH2Fetches(
    std::vector<std::string> paths,
    std::vector<std::pair<std::string, std::string>> echoHeaders = {}) {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            co_await mockH2Server(std::move(sock), resource);
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);

    std::vector<H2Result> results(paths.size());
    auto pending = std::make_shared<std::size_t>(paths.size());

    for (std::size_t i = 0; i < paths.size(); ++i) {
        asio::co_spawn(
            io,
            [&, i]() -> asio::awaitable<void> {
                try {
                    ruvia::FetchOptions options;
                    std::array<ruvia::HttpHeaderView, 1> headers{};
                    if (i < echoHeaders.size() && !echoHeaders[i].first.empty()) {
                        headers[0] = {echoHeaders[i].first, echoHeaders[i].second};
                        options.headers = headers;
                    }
                    auto response = co_await taskAsAwaitable(
                        session->fetch(paths[i], options, std::pmr::get_default_resource()));
                    results[i].ok = true;
                    results[i].status = response.status();
                    results[i].body.assign(response.body().data(), response.body().size());
                } catch (const std::exception& e) {
                    results[i].error = e.what();
                }
                if (--(*pending) == 0) {
                    session->closeNow();
                }
            },
            asio::detached);
    }

    io.run();
    return results;
}

H2BadTrailerOutcome runH2TrailerWithoutEndStream() {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    H2BadTrailerOutcome out;
    std::shared_ptr<tcp::socket> serverSocket;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                serverSocket = std::make_shared<tcp::socket>(
                    co_await acceptor.async_accept(asio::use_awaitable));
                auto& sock = *serverSocket;
                auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == size;
                };
                auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_write(
                        sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == bytes.size();
                };

                char preface[24];
                if (!co_await readExact(preface, sizeof(preface))) {
                    co_return;
                }
                char settings[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
                if (!co_await writeAll(std::string_view(settings, sizeof(settings)))) {
                    co_return;
                }

                std::uint32_t streamId = 0;
                for (;;) {
                    char headerBytes[kHttp2FrameHeaderBytes];
                    if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                        co_return;
                    }
                    const auto header = http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                    std::string payload(header.length, '\0');
                    if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                        co_return;
                    }
                    if (static_cast<Http2FrameType>(header.type) == Http2FrameType::kSettings) {
                        if ((header.flags & kHttp2FlagAck) == 0) {
                            char ack[kHttp2FrameHeaderBytes];
                            http2WriteFrameHeader(ack, 0, Http2FrameType::kSettings, kHttp2FlagAck, 0);
                            if (!co_await writeAll(std::string_view(ack, sizeof(ack)))) {
                                co_return;
                            }
                        }
                        continue;
                    }
                    if (static_cast<Http2FrameType>(header.type) == Http2FrameType::kHeaders) {
                        streamId = header.streamId;
                        break;
                    }
                }

                std::pmr::string block(resource);
                HpackEncoder::encodeStatus(block, 200);
                char responseHeaders[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(
                    responseHeaders, static_cast<std::uint32_t>(block.size()),
                    Http2FrameType::kHeaders, kHttp2FlagEndHeaders, streamId);
                if (!co_await writeAll(std::string_view(responseHeaders, sizeof(responseHeaders))) ||
                    !co_await writeAll(std::string_view(block.data(), block.size()))) {
                    co_return;
                }

                const std::string body = "body";
                char dataHeader[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(
                    dataHeader, static_cast<std::uint32_t>(body.size()), Http2FrameType::kData, 0, streamId);
                if (!co_await writeAll(std::string_view(dataHeader, sizeof(dataHeader))) ||
                    !co_await writeAll(body)) {
                    co_return;
                }

                char trailer[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(trailer, 0, Http2FrameType::kHeaders, kHttp2FlagEndHeaders, streamId);
                if (!co_await writeAll(std::string_view(trailer, sizeof(trailer)))) {
                    co_return;
                }

                char next[kHttp2FrameHeaderBytes];
                for (;;) {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(next, sizeof(next)), asio::as_tuple(asio::use_awaitable));
                    if (ec || n == 0) {
                        out.serverSawClose = true;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);
    auto watchdog = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(300));
    watchdog->async_wait([&](const std::error_code& ec) {
        if (ec) {
            return;
        }
        out.timedOut = true;
        if (serverSocket) {
            std::error_code ignored;
            serverSocket->close(ignored);
        }
        session->closeNow();
        std::error_code ignored;
        acceptor.close(ignored);
    });

    asio::co_spawn(
        io,
        [&, watchdog]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                (void)co_await taskAsAwaitable(session->fetch("/trailer-no-end", options, resource));
            } catch (...) {
                out.clientFailed = true;
            }
            std::error_code ignored;
            watchdog->cancel(ignored);
            session->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

H2BadFrameOutcome runH2DataBeforeHeaders() {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    H2BadFrameOutcome out;
    std::shared_ptr<tcp::socket> serverSocket;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                serverSocket = std::make_shared<tcp::socket>(
                    co_await acceptor.async_accept(asio::use_awaitable));
                auto& sock = *serverSocket;
                auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == size;
                };
                auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_write(
                        sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == bytes.size();
                };

                char preface[24];
                if (!co_await readExact(preface, sizeof(preface))) {
                    co_return;
                }
                char settings[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
                if (!co_await writeAll(std::string_view(settings, sizeof(settings)))) {
                    co_return;
                }

                std::uint32_t streamId = 0;
                for (;;) {
                    char headerBytes[kHttp2FrameHeaderBytes];
                    if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                        co_return;
                    }
                    const auto header = http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                    std::string payload(header.length, '\0');
                    if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                        co_return;
                    }
                    if (static_cast<Http2FrameType>(header.type) == Http2FrameType::kSettings) {
                        if ((header.flags & kHttp2FlagAck) == 0) {
                            char ack[kHttp2FrameHeaderBytes];
                            http2WriteFrameHeader(ack, 0, Http2FrameType::kSettings, kHttp2FlagAck, 0);
                            if (!co_await writeAll(std::string_view(ack, sizeof(ack)))) {
                                co_return;
                            }
                        }
                        continue;
                    }
                    if (static_cast<Http2FrameType>(header.type) == Http2FrameType::kHeaders) {
                        streamId = header.streamId;
                        break;
                    }
                }

                const std::string body = "bad";
                char dataHeader[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(
                    dataHeader, static_cast<std::uint32_t>(body.size()),
                    Http2FrameType::kData, kHttp2FlagEndStream, streamId);
                if (!co_await writeAll(std::string_view(dataHeader, sizeof(dataHeader))) ||
                    !co_await writeAll(body)) {
                    co_return;
                }

                char next[kHttp2FrameHeaderBytes];
                for (;;) {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(next, sizeof(next)), asio::as_tuple(asio::use_awaitable));
                    if (ec || n == 0) {
                        out.serverSawClose = true;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);
    auto watchdog = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(300));
    watchdog->async_wait([&](const std::error_code& ec) {
        if (ec) {
            return;
        }
        out.timedOut = true;
        if (serverSocket) {
            std::error_code ignored;
            serverSocket->close(ignored);
        }
        session->closeNow();
        std::error_code ignored;
        acceptor.close(ignored);
    });

    asio::co_spawn(
        io,
        [&, watchdog]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto response = co_await taskAsAwaitable(session->fetch("/data-before-headers", options, resource));
                out.status = response.status();
                out.body.assign(response.body().data(), response.body().size());
            } catch (...) {
                out.clientFailed = true;
            }
            std::error_code ignored;
            watchdog->cancel(ignored);
            session->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

H2BadFrameOutcome runH2DataAfterEndStream() {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    H2BadFrameOutcome out;
    std::shared_ptr<tcp::socket> serverSocket;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                serverSocket = std::make_shared<tcp::socket>(
                    co_await acceptor.async_accept(asio::use_awaitable));
                auto& sock = *serverSocket;
                auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == size;
                };
                auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_write(
                        sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == bytes.size();
                };

                char preface[24];
                if (!co_await readExact(preface, sizeof(preface))) {
                    co_return;
                }
                char settings[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
                if (!co_await writeAll(std::string_view(settings, sizeof(settings)))) {
                    co_return;
                }

                std::uint32_t streamId = 0;
                for (;;) {
                    char headerBytes[kHttp2FrameHeaderBytes];
                    if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                        co_return;
                    }
                    const auto header = http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                    std::string payload(header.length, '\0');
                    if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                        co_return;
                    }
                    if (static_cast<Http2FrameType>(header.type) == Http2FrameType::kSettings) {
                        if ((header.flags & kHttp2FlagAck) == 0) {
                            char ack[kHttp2FrameHeaderBytes];
                            http2WriteFrameHeader(ack, 0, Http2FrameType::kSettings, kHttp2FlagAck, 0);
                            if (!co_await writeAll(std::string_view(ack, sizeof(ack)))) {
                                co_return;
                            }
                        }
                        continue;
                    }
                    if (static_cast<Http2FrameType>(header.type) == Http2FrameType::kHeaders) {
                        streamId = header.streamId;
                        break;
                    }
                }

                std::pmr::string block(resource);
                HpackEncoder::encodeStatus(block, 200);
                char responseHeaders[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(
                    responseHeaders, static_cast<std::uint32_t>(block.size()),
                    Http2FrameType::kHeaders, kHttp2FlagEndHeaders, streamId);
                if (!co_await writeAll(std::string_view(responseHeaders, sizeof(responseHeaders))) ||
                    !co_await writeAll(std::string_view(block.data(), block.size()))) {
                    co_return;
                }

                const std::string body = "ok";
                char dataHeader[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(
                    dataHeader, static_cast<std::uint32_t>(body.size()), Http2FrameType::kData, 0, streamId);
                if (!co_await writeAll(std::string_view(dataHeader, sizeof(dataHeader))) ||
                    !co_await writeAll(body)) {
                    co_return;
                }

                char trailer[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(
                    trailer, 0, Http2FrameType::kHeaders,
                    static_cast<std::uint8_t>(kHttp2FlagEndHeaders | kHttp2FlagEndStream), streamId);
                if (!co_await writeAll(std::string_view(trailer, sizeof(trailer)))) {
                    co_return;
                }

                const std::string extra = "bad";
                char extraDataHeader[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(
                    extraDataHeader, static_cast<std::uint32_t>(extra.size()),
                    Http2FrameType::kData, kHttp2FlagEndStream, streamId);
                if (!co_await writeAll(std::string_view(extraDataHeader, sizeof(extraDataHeader))) ||
                    !co_await writeAll(extra)) {
                    co_return;
                }

                char next[kHttp2FrameHeaderBytes];
                for (;;) {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(next, sizeof(next)), asio::as_tuple(asio::use_awaitable));
                    if (ec || n == 0) {
                        out.serverSawClose = true;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);
    auto watchdog = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(300));
    watchdog->async_wait([&](const std::error_code& ec) {
        if (ec) {
            return;
        }
        out.timedOut = true;
        if (serverSocket) {
            std::error_code ignored;
            serverSocket->close(ignored);
        }
        session->closeNow();
        std::error_code ignored;
        acceptor.close(ignored);
    });

    asio::co_spawn(
        io,
        [&, watchdog]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto response = co_await taskAsAwaitable(session->fetch("/data-after-end", options, resource));
                out.status = response.status();
                out.body.assign(response.body().data(), response.body().size());
            } catch (...) {
                out.clientFailed = true;
            }
            std::error_code ignored;
            watchdog->cancel(ignored);
            session->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

H2BadFrameOutcome runH2HeadersAfterEndStream() {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    H2BadFrameOutcome out;
    std::shared_ptr<tcp::socket> serverSocket;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                serverSocket = std::make_shared<tcp::socket>(
                    co_await acceptor.async_accept(asio::use_awaitable));
                auto& sock = *serverSocket;
                auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == size;
                };
                auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_write(
                        sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == bytes.size();
                };

                char preface[24];
                if (!co_await readExact(preface, sizeof(preface))) {
                    co_return;
                }
                char settings[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
                if (!co_await writeAll(std::string_view(settings, sizeof(settings)))) {
                    co_return;
                }

                std::uint32_t streamId = 0;
                for (;;) {
                    char headerBytes[kHttp2FrameHeaderBytes];
                    if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                        co_return;
                    }
                    const auto header = http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                    std::string payload(header.length, '\0');
                    if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                        co_return;
                    }
                    if (static_cast<Http2FrameType>(header.type) == Http2FrameType::kSettings) {
                        if ((header.flags & kHttp2FlagAck) == 0) {
                            char ack[kHttp2FrameHeaderBytes];
                            http2WriteFrameHeader(ack, 0, Http2FrameType::kSettings, kHttp2FlagAck, 0);
                            if (!co_await writeAll(std::string_view(ack, sizeof(ack)))) {
                                co_return;
                            }
                        }
                        continue;
                    }
                    if (static_cast<Http2FrameType>(header.type) == Http2FrameType::kHeaders) {
                        streamId = header.streamId;
                        break;
                    }
                }

                std::pmr::string block(resource);
                HpackEncoder::encodeStatus(block, 200);
                char responseHeaders[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(
                    responseHeaders, static_cast<std::uint32_t>(block.size()),
                    Http2FrameType::kHeaders, kHttp2FlagEndHeaders, streamId);
                if (!co_await writeAll(std::string_view(responseHeaders, sizeof(responseHeaders))) ||
                    !co_await writeAll(std::string_view(block.data(), block.size()))) {
                    co_return;
                }

                const std::string body = "ok";
                char dataHeader[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(
                    dataHeader, static_cast<std::uint32_t>(body.size()), Http2FrameType::kData, 0, streamId);
                if (!co_await writeAll(std::string_view(dataHeader, sizeof(dataHeader))) ||
                    !co_await writeAll(body)) {
                    co_return;
                }

                char trailer[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(
                    trailer, 0, Http2FrameType::kHeaders,
                    static_cast<std::uint8_t>(kHttp2FlagEndHeaders | kHttp2FlagEndStream), streamId);
                if (!co_await writeAll(std::string_view(trailer, sizeof(trailer)))) {
                    co_return;
                }

                std::pmr::string lateBlock(resource);
                HpackEncoder::encodeHeader(lateBlock, "x-late", "1");
                char lateHeaders[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(
                    lateHeaders, static_cast<std::uint32_t>(lateBlock.size()),
                    Http2FrameType::kHeaders,
                    static_cast<std::uint8_t>(kHttp2FlagEndHeaders | kHttp2FlagEndStream), streamId);
                if (!co_await writeAll(std::string_view(lateHeaders, sizeof(lateHeaders))) ||
                    !co_await writeAll(std::string_view(lateBlock.data(), lateBlock.size()))) {
                    co_return;
                }

                char next[kHttp2FrameHeaderBytes];
                for (;;) {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(next, sizeof(next)), asio::as_tuple(asio::use_awaitable));
                    if (ec || n == 0) {
                        out.serverSawClose = true;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);
    auto watchdog = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(300));
    watchdog->async_wait([&](const std::error_code& ec) {
        if (ec) {
            return;
        }
        out.timedOut = true;
        if (serverSocket) {
            std::error_code ignored;
            serverSocket->close(ignored);
        }
        session->closeNow();
        std::error_code ignored;
        acceptor.close(ignored);
    });

    asio::co_spawn(
        io,
        [&, watchdog]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto response = co_await taskAsAwaitable(session->fetch("/headers-after-end", options, resource));
                out.status = response.status();
                out.body.assign(response.body().data(), response.body().size());
            } catch (...) {
                out.clientFailed = true;
            }
            std::error_code ignored;
            watchdog->cancel(ignored);
            session->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

H2BadFrameOutcome runH2DataOnIdleStream() {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    H2BadFrameOutcome out;
    std::shared_ptr<tcp::socket> serverSocket;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                serverSocket = std::make_shared<tcp::socket>(
                    co_await acceptor.async_accept(asio::use_awaitable));
                auto& sock = *serverSocket;
                auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == size;
                };
                auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_write(
                        sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == bytes.size();
                };

                char preface[24];
                if (!co_await readExact(preface, sizeof(preface))) {
                    co_return;
                }
                char settings[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
                if (!co_await writeAll(std::string_view(settings, sizeof(settings)))) {
                    co_return;
                }

                const std::string body = "bad";
                char dataHeader[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(
                    dataHeader, static_cast<std::uint32_t>(body.size()),
                    Http2FrameType::kData, kHttp2FlagEndStream, 99);
                if (!co_await writeAll(std::string_view(dataHeader, sizeof(dataHeader))) ||
                    !co_await writeAll(body)) {
                    co_return;
                }

                char next[kHttp2FrameHeaderBytes];
                for (;;) {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(next, sizeof(next)), asio::as_tuple(asio::use_awaitable));
                    if (ec || n == 0) {
                        out.serverSawClose = true;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);
    auto watchdog = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(300));
    watchdog->async_wait([&](const std::error_code& ec) {
        if (ec) {
            return;
        }
        out.timedOut = true;
        if (serverSocket) {
            std::error_code ignored;
            serverSocket->close(ignored);
        }
        session->closeNow();
        std::error_code ignored;
        acceptor.close(ignored);
    });

    asio::co_spawn(
        io,
        [&, watchdog]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto response = co_await taskAsAwaitable(session->fetch("/idle-data", options, resource));
                out.status = response.status();
                out.body.assign(response.body().data(), response.body().size());
            } catch (...) {
                out.clientFailed = true;
            }
            std::error_code ignored;
            watchdog->cancel(ignored);
            session->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

H2BadFrameOutcome runH2TruncatedGoawayOnActiveStream() {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    H2BadFrameOutcome out;
    std::shared_ptr<tcp::socket> serverSocket;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                serverSocket = std::make_shared<tcp::socket>(
                    co_await acceptor.async_accept(asio::use_awaitable));
                auto& sock = *serverSocket;
                auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == size;
                };
                auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_write(
                        sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == bytes.size();
                };

                char preface[24];
                if (!co_await readExact(preface, sizeof(preface))) {
                    co_return;
                }
                char settings[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
                if (!co_await writeAll(std::string_view(settings, sizeof(settings)))) {
                    co_return;
                }

                for (;;) {
                    char headerBytes[kHttp2FrameHeaderBytes];
                    if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                        co_return;
                    }
                    const auto header = http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                    std::string payload(header.length, '\0');
                    if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                        co_return;
                    }
                    if (static_cast<Http2FrameType>(header.type) == Http2FrameType::kSettings) {
                        if ((header.flags & kHttp2FlagAck) == 0) {
                            char ack[kHttp2FrameHeaderBytes];
                            http2WriteFrameHeader(ack, 0, Http2FrameType::kSettings, kHttp2FlagAck, 0);
                            if (!co_await writeAll(std::string_view(ack, sizeof(ack)))) {
                                co_return;
                            }
                        }
                        continue;
                    }
                    if (static_cast<Http2FrameType>(header.type) == Http2FrameType::kHeaders) {
                        break;
                    }
                }

                char goaway[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(goaway, 0, Http2FrameType::kGoaway, 0, 0);
                if (!co_await writeAll(std::string_view(goaway, sizeof(goaway)))) {
                    co_return;
                }

                char next[kHttp2FrameHeaderBytes];
                for (;;) {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(next, sizeof(next)), asio::as_tuple(asio::use_awaitable));
                    if (ec || n == 0) {
                        out.serverSawClose = true;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);
    auto watchdog = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(300));
    watchdog->async_wait([&](const std::error_code& ec) {
        if (ec) {
            return;
        }
        out.timedOut = true;
        if (serverSocket) {
            std::error_code ignored;
            serverSocket->close(ignored);
        }
        session->closeNow();
        std::error_code ignored;
        acceptor.close(ignored);
    });

    asio::co_spawn(
        io,
        [&, watchdog]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto response = co_await taskAsAwaitable(session->fetch("/truncated-goaway", options, resource));
                out.status = response.status();
                out.body.assign(response.body().data(), response.body().size());
            } catch (...) {
                out.clientFailed = true;
            }
            std::error_code ignored;
            watchdog->cancel(ignored);
            session->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

H2BadFrameOutcome runH2MalformedPriority() {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    H2BadFrameOutcome out;
    std::shared_ptr<tcp::socket> serverSocket;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                serverSocket = std::make_shared<tcp::socket>(
                    co_await acceptor.async_accept(asio::use_awaitable));
                auto& sock = *serverSocket;
                auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == size;
                };
                auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_write(
                        sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == bytes.size();
                };

                char preface[24];
                if (!co_await readExact(preface, sizeof(preface))) {
                    co_return;
                }
                char settings[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
                if (!co_await writeAll(std::string_view(settings, sizeof(settings)))) {
                    co_return;
                }

                char priority[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(priority, 0, Http2FrameType::kPriority, 0, 0);
                if (!co_await writeAll(std::string_view(priority, sizeof(priority)))) {
                    co_return;
                }

                char next[kHttp2FrameHeaderBytes];
                for (;;) {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(next, sizeof(next)), asio::as_tuple(asio::use_awaitable));
                    if (ec || n == 0) {
                        out.serverSawClose = true;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);
    auto watchdog = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(300));
    watchdog->async_wait([&](const std::error_code& ec) {
        if (ec) {
            return;
        }
        out.timedOut = true;
        if (serverSocket) {
            std::error_code ignored;
            serverSocket->close(ignored);
        }
        session->closeNow();
        std::error_code ignored;
        acceptor.close(ignored);
    });

    asio::co_spawn(
        io,
        [&, watchdog]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto response = co_await taskAsAwaitable(session->fetch("/malformed-priority", options, resource));
                out.status = response.status();
                out.body.assign(response.body().data(), response.body().size());
            } catch (...) {
                out.clientFailed = true;
            }
            std::error_code ignored;
            watchdog->cancel(ignored);
            session->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

H2BadFrameOutcome runH2SelfDependentPriority() {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    H2BadFrameOutcome out;
    std::shared_ptr<tcp::socket> serverSocket;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                serverSocket = std::make_shared<tcp::socket>(
                    co_await acceptor.async_accept(asio::use_awaitable));
                auto& sock = *serverSocket;
                auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == size;
                };
                auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_write(
                        sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == bytes.size();
                };

                char preface[24];
                if (!co_await readExact(preface, sizeof(preface))) {
                    co_return;
                }
                char settings[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
                if (!co_await writeAll(std::string_view(settings, sizeof(settings)))) {
                    co_return;
                }

                std::uint32_t streamId = 0;
                for (;;) {
                    char headerBytes[kHttp2FrameHeaderBytes];
                    if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                        co_return;
                    }
                    const auto header = http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                    std::string payload(header.length, '\0');
                    if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                        co_return;
                    }
                    if (static_cast<Http2FrameType>(header.type) == Http2FrameType::kSettings) {
                        if ((header.flags & kHttp2FlagAck) == 0) {
                            char ack[kHttp2FrameHeaderBytes];
                            http2WriteFrameHeader(ack, 0, Http2FrameType::kSettings, kHttp2FlagAck, 0);
                            if (!co_await writeAll(std::string_view(ack, sizeof(ack)))) {
                                co_return;
                            }
                        }
                        continue;
                    }
                    if (static_cast<Http2FrameType>(header.type) == Http2FrameType::kHeaders) {
                        streamId = header.streamId;
                        break;
                    }
                }

                char payload[5]{};
                http2Write32(payload, streamId);
                payload[4] = 16;
                char priority[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(priority, sizeof(payload), Http2FrameType::kPriority, 0, streamId);
                if (!co_await writeAll(std::string_view(priority, sizeof(priority))) ||
                    !co_await writeAll(std::string_view(payload, sizeof(payload)))) {
                    co_return;
                }

                char next[kHttp2FrameHeaderBytes];
                for (;;) {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(next, sizeof(next)), asio::as_tuple(asio::use_awaitable));
                    if (ec || n == 0) {
                        out.serverSawClose = true;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);
    auto watchdog = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(300));
    watchdog->async_wait([&](const std::error_code& ec) {
        if (ec) {
            return;
        }
        out.timedOut = true;
        if (serverSocket) {
            std::error_code ignored;
            serverSocket->close(ignored);
        }
        session->closeNow();
        std::error_code ignored;
        acceptor.close(ignored);
    });

    asio::co_spawn(
        io,
        [&, watchdog]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto response = co_await taskAsAwaitable(session->fetch("/self-dependent-priority", options, resource));
                out.status = response.status();
                out.body.assign(response.body().data(), response.body().size());
            } catch (...) {
                out.clientFailed = true;
            }
            std::error_code ignored;
            watchdog->cancel(ignored);
            session->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

H2BadFrameOutcome runH2SelfDependentPriorityHeaders() {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    H2BadFrameOutcome out;
    std::shared_ptr<tcp::socket> serverSocket;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                serverSocket = std::make_shared<tcp::socket>(
                    co_await acceptor.async_accept(asio::use_awaitable));
                auto& sock = *serverSocket;
                auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == size;
                };
                auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                    auto [ec, n] = co_await asio::async_write(
                        sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                    co_return !ec && n == bytes.size();
                };

                char preface[24];
                if (!co_await readExact(preface, sizeof(preface))) {
                    co_return;
                }
                char settings[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
                if (!co_await writeAll(std::string_view(settings, sizeof(settings)))) {
                    co_return;
                }

                std::uint32_t streamId = 0;
                for (;;) {
                    char headerBytes[kHttp2FrameHeaderBytes];
                    if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                        co_return;
                    }
                    const auto header = http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                    std::string payload(header.length, '\0');
                    if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                        co_return;
                    }
                    if (static_cast<Http2FrameType>(header.type) == Http2FrameType::kSettings) {
                        if ((header.flags & kHttp2FlagAck) == 0) {
                            char ack[kHttp2FrameHeaderBytes];
                            http2WriteFrameHeader(ack, 0, Http2FrameType::kSettings, kHttp2FlagAck, 0);
                            if (!co_await writeAll(std::string_view(ack, sizeof(ack)))) {
                                co_return;
                            }
                        }
                        continue;
                    }
                    if (static_cast<Http2FrameType>(header.type) == Http2FrameType::kHeaders) {
                        streamId = header.streamId;
                        break;
                    }
                }

                std::pmr::string block(resource);
                block.resize(5);
                http2Write32(block.data(), streamId);
                block[4] = 16;
                HpackEncoder::encodeStatus(block, 200);
                char responseHeaders[kHttp2FrameHeaderBytes];
                http2WriteFrameHeader(
                    responseHeaders, static_cast<std::uint32_t>(block.size()),
                    Http2FrameType::kHeaders,
                    static_cast<std::uint8_t>(
                        kHttp2FlagEndHeaders | kHttp2FlagEndStream | kHttp2FlagPriority),
                    streamId);
                if (!co_await writeAll(std::string_view(responseHeaders, sizeof(responseHeaders))) ||
                    !co_await writeAll(std::string_view(block.data(), block.size()))) {
                    co_return;
                }

                char next[kHttp2FrameHeaderBytes];
                for (;;) {
                    auto [ec, n] = co_await asio::async_read(
                        sock, asio::buffer(next, sizeof(next)), asio::as_tuple(asio::use_awaitable));
                    if (ec || n == 0) {
                        out.serverSawClose = true;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                out.error += std::string("server:") + e.what();
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);
    auto watchdog = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(300));
    watchdog->async_wait([&](const std::error_code& ec) {
        if (ec) {
            return;
        }
        out.timedOut = true;
        if (serverSocket) {
            std::error_code ignored;
            serverSocket->close(ignored);
        }
        session->closeNow();
        std::error_code ignored;
        acceptor.close(ignored);
    });

    asio::co_spawn(
        io,
        [&, watchdog]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto response = co_await taskAsAwaitable(
                    session->fetch("/self-dependent-priority-headers", options, resource));
                out.status = response.status();
                out.body.assign(response.body().data(), response.body().size());
            } catch (...) {
                out.clientFailed = true;
            }
            std::error_code ignored;
            watchdog->cancel(ignored);
            session->closeNow();
        },
        asio::detached);

    io.run();
    return out;
}

}  // namespace

// --- Single request round-trip -------------------------------------------
RUVIA_TEST(http2_single_get) {
    const auto results = runH2Fetches({"/hello"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(results[0].error.empty());
    RUVIA_CHECK(results[0].ok);
    RUVIA_CHECK_EQ(results[0].status, 200);
    RUVIA_CHECK_EQ(results[0].body, std::string("/hello"));
}

// --- Multiplexed concurrent requests on one connection -------------------
RUVIA_TEST(http2_multiplexed_requests) {
    const auto results = runH2Fetches({"/a", "/bb", "/ccc", "/dddd"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{4});
    for (std::size_t i = 0; i < results.size(); ++i) {
        RUVIA_CHECK(results[i].error.empty());
        RUVIA_CHECK(results[i].ok);
        RUVIA_CHECK_EQ(results[i].status, 200);
    }
    // Each stream must carry its own path back independently.
    if (results.size() == 4) {
        RUVIA_CHECK_EQ(results[0].body, std::string("/a"));
        RUVIA_CHECK_EQ(results[1].body, std::string("/bb"));
        RUVIA_CHECK_EQ(results[2].body, std::string("/ccc"));
        RUVIA_CHECK_EQ(results[3].body, std::string("/dddd"));
    }
}

// --- Request header encoding + response decoding round-trip --------------
RUVIA_TEST(http2_request_header_roundtrip) {
    const auto results = runH2Fetches({"/echo"}, {{"x-echo", "custom-value"}});
    RUVIA_CHECK(results[0].error.empty());
    RUVIA_CHECK(results[0].ok);
    RUVIA_CHECK_EQ(results[0].body, std::string("custom-value"));
}

RUVIA_TEST(http2_request_te_header_must_be_trailers) {
    const auto results = runH2Fetches({"/echo"}, {{"te", "gzip"}});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(!results[0].ok);
    RUVIA_CHECK(!results[0].error.empty());
}

RUVIA_TEST(http2_request_trailer_header_is_client_managed) {
    const auto results = runH2Fetches({"/echo"}, {{"trailer", "Digest"}});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(!results[0].ok);
    RUVIA_CHECK(!results[0].error.empty());
}

// --- Streamed request body uses DATA frames over HTTP/2 ------------------
RUVIA_TEST(http2_streamed_request_body) {
    const auto body = runH2UploadFetch();
    RUVIA_CHECK_EQ(body, std::string("alphabeta"));
}

// The upload blocks on a 4-byte send window; an early full response must unblock and abandon it
// (regression guard against the streamed-upload hang) rather than deadlock.
RUVIA_TEST(http2_streamed_request_body_early_response) {
    const auto body = runH2EarlyResponseUpload();
    RUVIA_CHECK_EQ(body, std::string("done"));
}

// A single SETTINGS frame with two INITIAL_WINDOW_SIZE entries must move each live
// stream's send window by the sum of the per-entry deltas, not just the last one.
RUVIA_TEST(http2_initial_window_multiple_entries_apply_cumulative_delta) {
    const auto body = runH2InitialWindowMultiEntryUpload();
    RUVIA_CHECK_EQ(body, std::string("0123456789"));
}

RUVIA_TEST(http2_redirect_root_relative) {
    const auto results = runH2Fetches({"/redirect"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(results[0].error.empty());
    RUVIA_CHECK(results[0].ok);
    RUVIA_CHECK_EQ(results[0].status, 200);
    RUVIA_CHECK_EQ(results[0].body, std::string("/final"));
}

RUVIA_TEST(http2_fetch_gzip_content_encoding) {
    const auto results = runH2Fetches({"/gzip"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(results[0].error.empty());
    RUVIA_CHECK(results[0].ok);
    RUVIA_CHECK_EQ(results[0].status, 200);
    RUVIA_CHECK_EQ(results[0].body, std::string("compressed h2"));
}

RUVIA_TEST(http2_connection_specific_response_header_is_rejected) {
    const auto results = runH2Fetches({"/bad-connection-header"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(!results[0].ok);
    RUVIA_CHECK(!results[0].error.empty());
}

RUVIA_TEST(http2_connection_specific_trailer_header_is_rejected) {
    const auto results = runH2Fetches({"/bad-trailer-connection-header"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(!results[0].ok);
    RUVIA_CHECK(!results[0].error.empty());
}

RUVIA_TEST(http2_semantic_trailer_header_is_rejected) {
    const auto results = runH2Fetches({"/bad-trailer-content-type"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(!results[0].ok);
    RUVIA_CHECK(!results[0].error.empty());
}

RUVIA_TEST(http2_invalid_te_response_header_is_rejected) {
    const auto results = runH2Fetches({"/bad-te-header"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(!results[0].ok);
    RUVIA_CHECK(!results[0].error.empty());
}

RUVIA_TEST(http2_invalid_response_header_value_is_rejected) {
    const auto results = runH2Fetches({"/bad-cr-header"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(!results[0].ok);
    RUVIA_CHECK(!results[0].error.empty());
}

RUVIA_TEST(http2_content_length_mismatch_is_rejected) {
    const auto results = runH2Fetches({"/content-length-short"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(!results[0].ok);
    RUVIA_CHECK(!results[0].error.empty());
}

RUVIA_TEST(http2_status_after_regular_header_is_rejected) {
    const auto results = runH2Fetches({"/late-status"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(!results[0].ok);
    RUVIA_CHECK(!results[0].error.empty());
}

RUVIA_TEST(http2_status_value_must_be_three_digits) {
    const auto results = runH2Fetches({"/wide-status"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(!results[0].ok);
    RUVIA_CHECK(!results[0].error.empty());
}

RUVIA_TEST(http2_204_data_is_rejected) {
    const auto results = runH2Fetches({"/no-content-data"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(!results[0].ok);
    RUVIA_CHECK(!results[0].error.empty());
}

RUVIA_TEST(http2_205_data_is_rejected) {
    const auto results = runH2Fetches({"/reset-content-data"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(!results[0].ok);
    RUVIA_CHECK(!results[0].error.empty());
}

RUVIA_TEST(http2_informational_headers_do_not_complete_response) {
    const auto results = runH2Fetches({"/early-hints"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(results[0].error.empty());
    RUVIA_CHECK(results[0].ok);
    RUVIA_CHECK_EQ(results[0].status, 200);
    RUVIA_CHECK_EQ(results[0].body, std::string("/early-hints"));
}

RUVIA_TEST(http2_too_many_informational_headers_is_rejected) {
    const auto results = runH2Fetches({"/many-early-hints"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(!results[0].ok);
    RUVIA_CHECK(!results[0].error.empty());
}

RUVIA_TEST(http2_status_101_switching_protocols_is_rejected) {
    const auto results = runH2Fetches({"/switching-protocols"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(!results[0].ok);
    RUVIA_CHECK(!results[0].error.empty());
}

// --- Response terminated by trailers (must decode the trailer block) -----
RUVIA_TEST(http2_response_trailers) {
    // Two requests on one connection: the first ends with a trailer HEADERS block. If the client
    // failed to feed that block through HPACK, the connection decoder would desync and the second
    // request would fail. Both succeeding confirms the trailer path keeps HPACK in sync.
    const auto results = runH2Fetches({"/trailer", "/after"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{2});
    RUVIA_CHECK(results[0].error.empty());
    RUVIA_CHECK(results[0].ok);
    RUVIA_CHECK_EQ(results[0].body, std::string("/trailer"));
    RUVIA_CHECK(results[1].error.empty());
    RUVIA_CHECK(results[1].ok);
    RUVIA_CHECK_EQ(results[1].body, std::string("/after"));
}

RUVIA_TEST(http2_trailer_pseudo_header_is_rejected) {
    const auto results = runH2Fetches({"/bad-trailer"});
    RUVIA_CHECK_EQ(results.size(), std::size_t{1});
    RUVIA_CHECK(!results[0].ok);
    RUVIA_CHECK(!results[0].error.empty());
}

RUVIA_TEST(http2_trailer_without_end_stream_is_rejected) {
    const auto out = runH2TrailerWithoutEndStream();
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(!out.timedOut);
    RUVIA_CHECK(out.clientFailed);
    RUVIA_CHECK(out.serverSawClose);
}

RUVIA_TEST(http2_data_before_headers_is_rejected) {
    const auto out = runH2DataBeforeHeaders();
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(!out.timedOut);
    RUVIA_CHECK(out.clientFailed);
    RUVIA_CHECK(out.serverSawClose);
}

RUVIA_TEST(http2_data_after_end_stream_is_rejected) {
    const auto out = runH2DataAfterEndStream();
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(!out.timedOut);
    RUVIA_CHECK(out.clientFailed);
    RUVIA_CHECK(out.serverSawClose);
}

RUVIA_TEST(http2_headers_after_end_stream_is_rejected) {
    const auto out = runH2HeadersAfterEndStream();
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(!out.timedOut);
    RUVIA_CHECK(out.clientFailed);
    RUVIA_CHECK(out.serverSawClose);
}

RUVIA_TEST(http2_data_on_idle_stream_is_rejected) {
    const auto out = runH2DataOnIdleStream();
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(!out.timedOut);
    RUVIA_CHECK(out.clientFailed);
    RUVIA_CHECK(out.serverSawClose);
}

RUVIA_TEST(http2_truncated_goaway_with_active_stream_is_rejected) {
    const auto out = runH2TruncatedGoawayOnActiveStream();
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(!out.timedOut);
    RUVIA_CHECK(out.clientFailed);
    RUVIA_CHECK(out.serverSawClose);
}

RUVIA_TEST(http2_malformed_priority_is_rejected) {
    const auto out = runH2MalformedPriority();
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(!out.timedOut);
    RUVIA_CHECK(out.clientFailed);
    RUVIA_CHECK(out.serverSawClose);
}

RUVIA_TEST(http2_self_dependent_priority_is_rejected) {
    const auto out = runH2SelfDependentPriority();
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(!out.timedOut);
    RUVIA_CHECK(out.clientFailed);
    RUVIA_CHECK(out.serverSawClose);
}

RUVIA_TEST(http2_self_dependent_priority_headers_is_rejected) {
    const auto out = runH2SelfDependentPriorityHeaders();
    RUVIA_CHECK(out.error.empty());
    RUVIA_CHECK(!out.timedOut);
    RUVIA_CHECK(out.clientFailed);
    RUVIA_CHECK(out.serverSawClose);
}

// --- A request with no response END_STREAM trips the deadline ------------
RUVIA_TEST(http2_request_timeout) {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            co_await mockH2Server(std::move(sock), resource);
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;
    config.proxyReadTimeout = std::chrono::milliseconds(100);

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);

    auto scanTimer = std::make_shared<asio::steady_timer>(io);
    std::function<void()> armScan = [&]() {
        scanTimer->expires_after(std::chrono::milliseconds(10));
        scanTimer->async_wait([&](const std::error_code& ec) {
            if (ec) {
                return;
            }
            session->scanDeadlines(std::chrono::steady_clock::now());
            armScan();
        });
    };
    armScan();

    bool timedOut = false;
    bool succeeded = false;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                (void)co_await taskAsAwaitable(session->fetch("/timeout", options, resource));
                succeeded = true;
            } catch (...) {
                timedOut = true;
            }
            scanTimer->cancel();
            session->closeNow();
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(timedOut);
    RUVIA_CHECK(!succeeded);
}

// A STREAMING response whose server sends the headers then stalls (no DATA/END_STREAM) must be
// bounded by the per-stream inactivity timeout (proxy_read_timeout) -- readChunk() must throw,
// not hang. Before the inactivity change, streaming streams had no deadline and hung forever.
RUVIA_TEST(http2_stream_read_inactivity_timeout) {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            co_await mockH2Server(std::move(sock), resource);
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;
    config.proxyReadTimeout = std::chrono::milliseconds(100);

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);

    auto scanTimer = std::make_shared<asio::steady_timer>(io);
    std::function<void()> armScan = [&]() {
        scanTimer->expires_after(std::chrono::milliseconds(10));
        scanTimer->async_wait([&](const std::error_code& ec) {
            if (ec) {
                return;
            }
            session->scanDeadlines(std::chrono::steady_clock::now());
            armScan();
        });
    };
    armScan();

    bool readThrew = false;
    bool gotHeaders = false;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                // "/timeout": server sends 200 headers then never any DATA or END_STREAM.
                auto stream = co_await taskAsAwaitable(session->fetchStream("/timeout", options, resource));
                gotHeaders = stream.status() == 200;
                (void)co_await taskAsAwaitable(stream.readChunk());  // must time out, not hang
            } catch (...) {
                readThrew = true;
            }
            scanTimer->cancel();
            session->closeNow();
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(gotHeaders);
    RUVIA_CHECK(readThrew);
}

// A peer that advertises SETTINGS_MAX_CONCURRENT_STREAMS = 0 must not hang a fetch forever: the
// slot wait in beginRequest (which happens BEFORE a stream exists, so the per-stream deadline
// scan can't see it) is bounded by the request timeout via scanDeadlines.
RUVIA_TEST(http2_slot_wait_timeout) {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            char preface[24];
            auto [ecP, nP] = co_await asio::async_read(
                sock, asio::buffer(preface, sizeof(preface)), asio::as_tuple(asio::use_awaitable));
            if (ecP) {
                co_return;
            }
            // Initial SETTINGS advertising MAX_CONCURRENT_STREAMS = 0 (one 6-byte entry).
            char frame[kHttp2FrameHeaderBytes + 6];
            http2WriteFrameHeader(frame, 6, Http2FrameType::kSettings, 0, 0);
            frame[kHttp2FrameHeaderBytes + 0] = 0x00;  // id high
            frame[kHttp2FrameHeaderBytes + 1] = 0x03;  // SETTINGS_MAX_CONCURRENT_STREAMS
            frame[kHttp2FrameHeaderBytes + 2] = 0x00;
            frame[kHttp2FrameHeaderBytes + 3] = 0x00;
            frame[kHttp2FrameHeaderBytes + 4] = 0x00;
            frame[kHttp2FrameHeaderBytes + 5] = 0x00;  // value = 0
            co_await asio::async_write(
                sock, asio::buffer(frame, sizeof(frame)), asio::as_tuple(asio::use_awaitable));
            // Never open a slot: just drain whatever the client sends until it hangs up.
            char buf[256];
            for (;;) {
                auto [ec, n] = co_await sock.async_read_some(
                    asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
                if (ec) {
                    co_return;
                }
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;
    config.proxyReadTimeout = std::chrono::milliseconds(100);

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);

    auto scanTimer = std::make_shared<asio::steady_timer>(io);
    std::function<void()> armScan = [&]() {
        scanTimer->expires_after(std::chrono::milliseconds(10));
        scanTimer->async_wait([&](const std::error_code& ec) {
            if (ec) {
                return;
            }
            session->scanDeadlines(std::chrono::steady_clock::now());
            armScan();
        });
    };
    armScan();

    bool timedOut = false;
    bool succeeded = false;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                (void)co_await taskAsAwaitable(session->fetch("/", options, resource));
                succeeded = true;
            } catch (...) {
                timedOut = true;
            }
            scanTimer->cancel();
            session->closeNow();
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(timedOut);
    RUVIA_CHECK(!succeeded);
}

// --- Multi-frame response body reassembly (> one DATA frame) -------------
RUVIA_TEST(http2_large_response_body) {
    // "/large" makes the server emit a 40000-byte body across multiple DATA frames.
    const auto results = runH2Fetches({"/large"});
    RUVIA_CHECK(results[0].error.empty());
    RUVIA_CHECK(results[0].ok);
    RUVIA_CHECK_EQ(results[0].body.size(), std::size_t{40000});
    RUVIA_CHECK_EQ(results[0].body, std::string(40000, 'y'));
}

// --- Streaming: pull a multi-frame body incrementally over HTTP/2 --------
RUVIA_TEST(http2_stream_large_body) {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            co_await mockH2Server(std::move(sock), resource);
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);

    bool ok = false;
    int status = 0;
    std::string body;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto stream = co_await taskAsAwaitable(
                    session->fetchStream("/large", options, resource));
                status = stream.status();
                for (;;) {
                    auto chunk = co_await taskAsAwaitable(stream.readChunk());
                    if (chunk.empty()) {
                        break;
                    }
                    body.append(chunk.data(), chunk.size());
                }
                ok = true;
            } catch (...) {
            }
            session->closeNow();
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(ok);
    RUVIA_CHECK_EQ(status, 200);
    RUVIA_CHECK_EQ(body.size(), std::size_t{40000});
    RUVIA_CHECK_EQ(body, std::string(40000, 'y'));
}

// --- Streaming with on-the-fly gzip decode over HTTP/2 -------------------
// "/gzip" returns a gzip-encoded body with Content-Encoding: gzip. With options.decodeStream the
// streamed body must be decoded on the fly; without it, the raw compressed bytes come through.
RUVIA_TEST(http2_stream_gzip_decoded) {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            co_await mockH2Server(std::move(sock), resource);
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;

    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);

    bool ok = false;
    std::string body;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                options.decodeStream = true;
                auto stream = co_await taskAsAwaitable(
                    session->fetchStream("/gzip", options, resource));
                for (;;) {
                    auto chunk = co_await taskAsAwaitable(stream.readChunk());
                    if (chunk.empty()) {
                        break;
                    }
                    body.append(chunk.data(), chunk.size());
                }
                ok = true;
            } catch (...) {
            }
            session->closeNow();
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(ok);
    RUVIA_CHECK_EQ(body, std::string("compressed h2"));
}

// A streaming stream whose buffered-but-undrained DATA is aborted by RST_STREAM must
// return the DATA's withheld connection-level flow-control credit as a stream-0
// WINDOW_UPDATE (== the DATA size), so the multiplexed connection's receive window
// is not permanently leaked. Without the fix failStream never repays that credit.
RUVIA_TEST(http2_client_credits_connection_window_when_streamed_data_is_reset) {
    asio::io_context io;
    auto* resource = std::pmr::get_default_resource();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    constexpr std::uint32_t kDataSize = 13;  // distinctive; not the initial-window increment
    bool clientThrew = false;
    bool sawConnectionCreditForData = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            auto readExact = [&sock](void* d, std::size_t n) -> asio::awaitable<bool> {
                auto [ec, got] = co_await asio::async_read(
                    sock, asio::buffer(d, n), asio::as_tuple(asio::use_awaitable));
                co_return !ec && got == n;
            };
            auto writeAll = [&sock](std::string_view b) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    sock, asio::buffer(b.data(), b.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };

            char preface[24];
            if (!co_await readExact(preface, sizeof(preface))) co_return;
            char settings[kHttp2FrameHeaderBytes];
            http2WriteFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
            if (!co_await writeAll(std::string_view(settings, sizeof(settings)))) co_return;

            std::uint32_t requestStream = 0;
            while (requestStream == 0) {
                char hb[kHttp2FrameHeaderBytes];
                if (!co_await readExact(hb, sizeof(hb))) co_return;
                const auto h = http2ParseFrameHeader(std::string_view(hb, sizeof(hb)));
                std::string payload(h.length, '\0');
                if (h.length != 0 && !co_await readExact(payload.data(), payload.size())) co_return;
                const auto type = static_cast<Http2FrameType>(h.type);
                if (type == Http2FrameType::kSettings && (h.flags & kHttp2FlagAck) == 0) {
                    char ack[kHttp2FrameHeaderBytes];
                    http2WriteFrameHeader(ack, 0, Http2FrameType::kSettings, kHttp2FlagAck, 0);
                    if (!co_await writeAll(std::string_view(ack, sizeof(ack)))) co_return;
                } else if (type == Http2FrameType::kHeaders) {
                    requestStream = h.streamId;
                }
            }

            // Response HEADERS (200, no END_STREAM), a DATA frame (no END_STREAM, so the
            // client buffers it and defers the connection credit), then RST_STREAM.
            std::pmr::string block(resource);
            HpackEncoder::encodeStatus(block, 200);
            char rh[kHttp2FrameHeaderBytes];
            http2WriteFrameHeader(
                rh, static_cast<std::uint32_t>(block.size()),
                Http2FrameType::kHeaders, kHttp2FlagEndHeaders, requestStream);
            if (!co_await writeAll(std::string_view(rh, sizeof(rh)))) co_return;
            if (!co_await writeAll(std::string_view(block.data(), block.size()))) co_return;
            char dh[kHttp2FrameHeaderBytes];
            http2WriteFrameHeader(dh, kDataSize, Http2FrameType::kData, 0, requestStream);
            if (!co_await writeAll(std::string_view(dh, sizeof(dh)))) co_return;
            if (!co_await writeAll(std::string(kDataSize, 'x'))) co_return;
            char rst[kHttp2FrameHeaderBytes + 4];
            char* rp = http2WriteFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, requestStream);
            rp[0] = 0; rp[1] = 0; rp[2] = 0; rp[3] = 2;  // INTERNAL_ERROR
            if (!co_await writeAll(std::string_view(rst, sizeof(rst)))) co_return;

            // Capture the client's post-reset connection-level (stream 0) WINDOW_UPDATE.
            for (;;) {
                char hb[kHttp2FrameHeaderBytes];
                if (!co_await readExact(hb, sizeof(hb))) co_return;
                const auto h = http2ParseFrameHeader(std::string_view(hb, sizeof(hb)));
                std::string payload(h.length, '\0');
                if (h.length != 0 && !co_await readExact(payload.data(), payload.size())) co_return;
                if (static_cast<Http2FrameType>(h.type) == Http2FrameType::kWindowUpdate &&
                    h.streamId == 0 && payload.size() == 4) {
                    const auto incr =
                        (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[0])) & 0x7FU) << 24 |
                        static_cast<std::uint32_t>(static_cast<unsigned char>(payload[1])) << 16 |
                        static_cast<std::uint32_t>(static_cast<unsigned char>(payload[2])) << 8 |
                        static_cast<std::uint32_t>(static_cast<unsigned char>(payload[3]));
                    if (incr == kDataSize) sawConnectionCreditForData = true;
                }
            }
        },
        asio::detached);

    ruvia::HttpClientConfig config;
    config.host = std::pmr::string("127.0.0.1", resource);
    config.port = port;
    config.tls = false;
    config.http2 = true;
    auto session = std::make_unique<Http2ClientSession>(io, std::move(config), resource);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                ruvia::FetchOptions options;
                auto stream = co_await taskAsAwaitable(session->fetchStream("/", options, resource));
                // Delay before reading so the reset (and failStream's credit) is processed
                // first: the failure path -- not readChunk's drain -- must repay the credit.
                asio::steady_timer settle(io, std::chrono::milliseconds(60));
                co_await settle.async_wait(asio::as_tuple(asio::use_awaitable));
                for (;;) {
                    auto chunk = co_await taskAsAwaitable(stream.readChunk());
                    if (chunk.empty()) break;
                }
            } catch (const std::exception&) {
                clientThrew = true;
            }
            // Deliberately no closeNow(): keep the session alive so its flusher sends the
            // WINDOW_UPDATE the server is waiting to read.
        },
        asio::detached);

    // Watchdog: end the test whether or not the credit arrives (without the fix it never
    // does and the server's capture loop would otherwise block).
    asio::steady_timer watchdog(io, std::chrono::milliseconds(500));
    watchdog.async_wait([&io](const std::error_code& ec) {
        if (!ec) io.stop();
    });

    io.run();
    RUVIA_CHECK(clientThrew);
    RUVIA_CHECK(sawConnectionCreditForData);
}
