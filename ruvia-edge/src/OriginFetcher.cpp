#include "ruvia/edge/OriginFetcher.h"

#include <array>
#include <charconv>
#include <string>
#include <system_error>
#include <tuple>
#include <variant>

#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/http/HttpClient.h"
#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"

namespace ruvia::edge {

namespace {

using namespace asio::experimental::awaitable_operators;

constexpr std::size_t kHeadBufferBytes = 16u * 1024u;
constexpr std::size_t kReadChunkBytes = 64u * 1024u;

[[nodiscard]] OriginFetchResult failure(OriginFetchOutcome outcome) {
    return OriginFetchResult{outcome, {}};
}

// How the response body is delimited, decided from the parsed response plan.
enum class BodyFraming : std::uint8_t {
    kNone,           // no message body (204/304/HEAD, or zero-content framing)
    kKnownLength,    // exact Content-Length bytes
    kChunked,        // chunked transfer-coding
    kCloseDelimited  // read until the origin closes the connection
};

}  // namespace

asio::awaitable<OriginFetchResult> OriginFetcher::fetch(
    asio::any_io_executor executor,
    std::string_view host,
    std::uint16_t port,
    const OriginRequest& request) const {
    // 1. Prepare the request head. This validates the whole request (method,
    // target, headers) before any I/O and generates Host/Content-Length.
    const HttpOrigin origin = HttpOrigin::http(host, port);
    HttpClientRequest clientRequest;
    clientRequest.method = request.method;
    clientRequest.target = request.target;
    clientRequest.headers = request.headers;
    clientRequest.content = HttpClientRequestContent::none();

    std::array<char, kHeadBufferBytes> headBuffer;
    const Http1ClientRequestWriter writer;
    const auto prepareResult = writer.prepare(
        origin,
        clientRequest,
        headBuffer,
        Http1ClientRequestWirePolicy::withoutExpectation(
            Http1ClientRequestClosePolicy::kCloseAfterResponse));
    const auto* prepared = prepareResult.prepared();
    if (prepared == nullptr) {
        co_return failure(OriginFetchOutcome::kUnsupported);
    }

    const auto tuple = asio::as_tuple(asio::use_awaitable);
    asio::steady_timer deadline(executor);
    asio::ip::tcp::resolver resolver(executor);
    asio::ip::tcp::socket socket(executor);
    std::string inbound;
    std::array<char, kReadChunkBytes> readBuffer;
    bool timedOut = false;
    bool readError = false;
    bool eof = false;

    // One read step, bounded by the inactivity deadline. Appends to `inbound`
    // and sets the timedOut/readError/eof flags the callers check.
    const auto readOnce = [&]() -> asio::awaitable<void> {
        deadline.expires_after(limits_.ioTimeout);
        auto readRaced = co_await (
            socket.async_read_some(asio::buffer(readBuffer), tuple) ||
            deadline.async_wait(tuple));
        if (readRaced.index() == 1) {
            timedOut = true;
            co_return;
        }
        auto& [ec, n] = std::get<0>(readRaced);
        if (n > 0) {
            inbound.append(readBuffer.data(), n);
        }
        if (ec == asio::error::eof) {
            eof = true;
        } else if (ec) {
            readError = true;
        }
    };

    // 2. Resolve and connect a fresh plaintext connection, under one deadline.
    std::array<char, 8> portText;
    const auto [portEnd, portEc] =
        std::to_chars(portText.data(), portText.data() + portText.size(), port);
    if (portEc != std::errc{}) {
        co_return failure(OriginFetchOutcome::kConnectFailed);
    }
    const std::string hostText(host);
    const std::string_view portView(
        portText.data(), static_cast<std::size_t>(portEnd - portText.data()));

    deadline.expires_after(limits_.connectTimeout);
    auto resolveRaced = co_await (
        resolver.async_resolve(hostText, portView, tuple) ||
        deadline.async_wait(tuple));
    if (resolveRaced.index() == 1) {
        co_return failure(OriginFetchOutcome::kTimeout);
    }
    if (std::get<0>(std::get<0>(resolveRaced))) {
        co_return failure(OriginFetchOutcome::kConnectFailed);
    }
    const auto endpoints = std::move(std::get<1>(std::get<0>(resolveRaced)));

    auto connectRaced = co_await (
        asio::async_connect(socket, endpoints, tuple) ||
        deadline.async_wait(tuple));
    if (connectRaced.index() == 1) {
        co_return failure(OriginFetchOutcome::kTimeout);
    }
    if (std::get<0>(std::get<0>(connectRaced))) {
        co_return failure(OriginFetchOutcome::kConnectFailed);
    }

    // 3. Send the request head (and immediate content, if any).
    deadline.expires_after(limits_.ioTimeout);
    auto writeRaced = co_await (
        asio::async_write(
            socket,
            asio::buffer(prepared->head().data(), prepared->head().size()),
            tuple) ||
        deadline.async_wait(tuple));
    if (writeRaced.index() == 1) {
        co_return failure(OriginFetchOutcome::kTimeout);
    }
    if (std::get<0>(std::get<0>(writeRaced))) {
        co_return failure(OriginFetchOutcome::kWriteFailed);
    }
    if (const auto* immediate = prepared->contentPlan().immediate();
        immediate != nullptr && !immediate->bytes().empty()) {
        deadline.expires_after(limits_.ioTimeout);
        auto contentRaced = co_await (
            asio::async_write(
                socket,
                asio::buffer(immediate->bytes().data(), immediate->bytes().size()),
                tuple) ||
            deadline.async_wait(tuple));
        if (contentRaced.index() == 1) {
            co_return failure(OriginFetchOutcome::kTimeout);
        }
        if (std::get<0>(std::get<0>(contentRaced))) {
            co_return failure(OriginFetchOutcome::kWriteFailed);
        }
    }

    // 4. Read and parse the response head, advancing past any 1xx responses.
    Http1ClientResponseParser parser(*prepared);
    OriginResponse response;
    BodyFraming framing = BodyFraming::kNone;
    std::size_t knownLength = 0;

    for (;;) {
        auto parseResult = parser.parse(inbound);
        if (parseResult.failure() != nullptr) {
            co_return failure(OriginFetchOutcome::kProtocolError);
        }
        if (auto* parsed = parseResult.parsed(); parsed != nullptr) {
            const auto& plan = parsed->plan();
            if (plan.informational() != nullptr) {
                inbound.erase(0, parsed->consumedBytes());
                continue;
            }
            response.status = parsed->head().status().value();
            for (const auto& field : parsed->head().headers()) {
                response.headers.emplace_back(
                    std::string(field.name()), std::string(field.value()));
            }
            const std::size_t consumed = parsed->consumedBytes();
            if (plan.withoutContent() != nullptr || plan.zeroContent() != nullptr) {
                framing = BodyFraming::kNone;
            } else if (const auto* known = plan.knownLength()) {
                framing = BodyFraming::kKnownLength;
                knownLength = known->contentLength();
            } else if (plan.chunked() != nullptr) {
                framing = BodyFraming::kChunked;
            } else if (plan.closeDelimited() != nullptr) {
                framing = BodyFraming::kCloseDelimited;
            } else {
                co_return failure(OriginFetchOutcome::kUnsupported);
            }
            inbound.erase(0, consumed);
            break;
        }

        co_await readOnce();
        if (timedOut) {
            co_return failure(OriginFetchOutcome::kTimeout);
        }
        if (readError || eof) {
            co_return failure(OriginFetchOutcome::kReadFailed);
        }
    }

    // 5. Read the body according to its framing.
    switch (framing) {
        case BodyFraming::kNone:
            break;

        case BodyFraming::kKnownLength: {
            if (knownLength > limits_.maxResponseBytes) {
                co_return failure(OriginFetchOutcome::kTooLarge);
            }
            while (inbound.size() < knownLength) {
                co_await readOnce();
                if (timedOut) {
                    co_return failure(OriginFetchOutcome::kTimeout);
                }
                if (readError || eof) {
                    co_return failure(OriginFetchOutcome::kReadFailed);
                }
            }
            inbound.resize(knownLength);
            response.body = std::move(inbound);
            break;
        }

        case BodyFraming::kCloseDelimited: {
            for (;;) {
                co_await readOnce();
                if (timedOut) {
                    co_return failure(OriginFetchOutcome::kTimeout);
                }
                if (inbound.size() > limits_.maxResponseBytes) {
                    co_return failure(OriginFetchOutcome::kTooLarge);
                }
                if (eof) {
                    break;  // EOF terminates a close-delimited message
                }
                if (readError) {
                    co_return failure(OriginFetchOutcome::kReadFailed);
                }
            }
            response.body = std::move(inbound);
            break;
        }

        case BodyFraming::kChunked: {
            ruvia::detail::Http1ChunkedBodyDecoder decoder(
                ProtocolByteLimit::limited(limits_.maxResponseBytes));
            for (;;) {
                const auto decoded = decoder.decode(inbound);
                if (decoded.failure() != nullptr) {
                    co_return failure(OriginFetchOutcome::kProtocolError);
                }
                if (const auto* chunk = decoded.bodyChunk()) {
                    response.body.append(chunk->bytes());
                    inbound.erase(0, decoded.consumedBytes());
                    if (response.body.size() > limits_.maxResponseBytes) {
                        co_return failure(OriginFetchOutcome::kTooLarge);
                    }
                    continue;
                }
                if (decoded.complete() != nullptr) {
                    break;
                }
                inbound.erase(0, decoded.consumedBytes());
                co_await readOnce();
                if (timedOut) {
                    co_return failure(OriginFetchOutcome::kTimeout);
                }
                if (readError || eof) {
                    co_return failure(OriginFetchOutcome::kReadFailed);
                }
            }
            break;
        }
    }

    co_return OriginFetchResult{OriginFetchOutcome::kOk, std::move(response)};
}

}  // namespace ruvia::edge
