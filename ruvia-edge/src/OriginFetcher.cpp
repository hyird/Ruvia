#include "ruvia/edge/OriginFetcher.h"

#include <array>
#include <charconv>
#include <string>
#include <system_error>

#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"

namespace ruvia::edge {

namespace {

using ruvia::detail::asyncAsio;

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

Task<OriginFetchResult> OriginFetcher::fetch(
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

    // 2. Resolve and connect a fresh plaintext connection.
    asio::ip::tcp::resolver resolver(executor);
    std::array<char, 8> portText;
    const auto [portEnd, portEc] =
        std::to_chars(portText.data(), portText.data() + portText.size(), port);
    if (portEc != std::errc{}) {
        co_return failure(OriginFetchOutcome::kConnectFailed);
    }
    const std::string hostText(host);
    const std::string_view portView(
        portText.data(), static_cast<std::size_t>(portEnd - portText.data()));

    auto resolveCompletion =
        co_await asyncAsio<asio::ip::tcp::resolver::results_type>(
            [&](auto handler) mutable {
                resolver.async_resolve(hostText, portView, std::move(handler));
            });
    if (resolveCompletion.errorCode()) {
        co_return failure(OriginFetchOutcome::kConnectFailed);
    }
    const auto endpoints = std::move(resolveCompletion).takeResult();

    asio::ip::tcp::socket socket(executor);
    const auto connectCompletion = co_await asyncAsio(
        [&](auto handler) mutable {
            asio::async_connect(socket, endpoints, std::move(handler));
        });
    if (connectCompletion.errorCode()) {
        co_return failure(OriginFetchOutcome::kConnectFailed);
    }

    // 3. Send the request head (the MVP sends only bodyless requests, but honor
    // an immediate content plan if one was produced).
    const auto writeCompletion = co_await asyncAsio(
        [&](auto handler) mutable {
            asio::async_write(
                socket,
                asio::buffer(prepared->head().data(), prepared->head().size()),
                std::move(handler));
        });
    if (writeCompletion.errorCode()) {
        co_return failure(OriginFetchOutcome::kWriteFailed);
    }
    if (const auto* immediate = prepared->contentPlan().immediate();
        immediate != nullptr && !immediate->bytes().empty()) {
        const auto contentCompletion = co_await asyncAsio(
            [&](auto handler) mutable {
                asio::async_write(
                    socket,
                    asio::buffer(immediate->bytes().data(), immediate->bytes().size()),
                    std::move(handler));
            });
        if (contentCompletion.errorCode()) {
            co_return failure(OriginFetchOutcome::kWriteFailed);
        }
    }

    // 4. Read and parse the response head, advancing past any informational
    // (1xx) responses until the final head arrives.
    Http1ClientResponseParser parser(*prepared);
    std::string inbound;
    std::array<char, kReadChunkBytes> readBuffer;
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
                // Not the final response: drop its bytes and keep parsing.
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
                // CONNECT tunnel or protocol upgrade: not an MVP cache path.
                co_return failure(OriginFetchOutcome::kUnsupported);
            }
            inbound.erase(0, consumed);  // inbound now holds only body bytes
            break;
        }

        // Need more bytes to complete the head.
        auto readResult = co_await asyncAsio<std::size_t>(
            [&](auto handler) mutable {
                socket.async_read_some(asio::buffer(readBuffer), std::move(handler));
            });
        if (readResult.result() > 0) {
            inbound.append(readBuffer.data(), readResult.result());
        }
        if (readResult.errorCode()) {
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
                auto readResult = co_await asyncAsio<std::size_t>(
                    [&](auto handler) mutable {
                        socket.async_read_some(
                            asio::buffer(readBuffer), std::move(handler));
                    });
                if (readResult.result() > 0) {
                    inbound.append(readBuffer.data(), readResult.result());
                }
                if (readResult.errorCode()) {
                    co_return failure(OriginFetchOutcome::kReadFailed);
                }
            }
            inbound.resize(knownLength);
            response.body = std::move(inbound);
            break;
        }

        case BodyFraming::kCloseDelimited: {
            for (;;) {
                auto readResult = co_await asyncAsio<std::size_t>(
                    [&](auto handler) mutable {
                        socket.async_read_some(
                            asio::buffer(readBuffer), std::move(handler));
                    });
                if (readResult.result() > 0) {
                    inbound.append(readBuffer.data(), readResult.result());
                    if (inbound.size() > limits_.maxResponseBytes) {
                        co_return failure(OriginFetchOutcome::kTooLarge);
                    }
                }
                if (readResult.errorCode() == asio::error::eof) {
                    break;  // EOF is the message terminator here
                }
                if (readResult.errorCode()) {
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
                // Need more bytes for the next chunk.
                inbound.erase(0, decoded.consumedBytes());
                auto readResult = co_await asyncAsio<std::size_t>(
                    [&](auto handler) mutable {
                        socket.async_read_some(
                            asio::buffer(readBuffer), std::move(handler));
                    });
                if (readResult.result() > 0) {
                    inbound.append(readBuffer.data(), readResult.result());
                }
                if (readResult.errorCode()) {
                    co_return failure(OriginFetchOutcome::kReadFailed);
                }
            }
            break;
        }
    }

    co_return OriginFetchResult{OriginFetchOutcome::kOk, std::move(response)};
}

}  // namespace ruvia::edge
