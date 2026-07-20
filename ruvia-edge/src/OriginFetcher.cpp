#include "ruvia/edge/OriginFetcher.h"

#include <array>
#include <charconv>
#include <chrono>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <variant>

#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <openssl/ssl.h>  // SSL_set_tlsext_host_name (SNI)

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

// The result of running one request/response exchange over an existing socket.
struct ExchangeOutcome final {
    OriginFetchResult result{OriginFetchOutcome::kConnectFailed, {}};
    // The origin allows this connection to be reused for another request.
    bool reusable{false};
    // A connect-level failure (write/read error before a valid response head).
    // On a reused pooled connection this signals a stale connection worth
    // retrying on a fresh one.
    bool transportFailed{false};
};

// Send the prepared request on `stream` and read the whole response, bounded by
// the inactivity deadline. Works over any async stream (plain TCP or TLS). Does
// not close the stream; the caller decides pooling.
template <typename Stream>
asio::awaitable<ExchangeOutcome> runExchange(
    Stream& stream,
    const PreparedHttp1ClientRequest& prepared,
    const OriginFetcher::Limits& limits) {
    const auto tuple = asio::as_tuple(asio::use_awaitable);
    asio::steady_timer deadline(stream.get_executor());

    ExchangeOutcome out;
    const auto fail = [&](OriginFetchOutcome outcome, bool transport) {
        out.result = failure(outcome);
        out.transportFailed = transport;
    };

    // Send the request head (and immediate content, if any).
    deadline.expires_after(limits.ioTimeout);
    {
        auto raced = co_await (
            asio::async_write(
                stream,
                asio::buffer(prepared.head().data(), prepared.head().size()),
                tuple) ||
            deadline.async_wait(tuple));
        if (raced.index() == 1) {
            fail(OriginFetchOutcome::kTimeout, false);
            co_return out;
        }
        if (std::get<0>(std::get<0>(raced))) {
            fail(OriginFetchOutcome::kWriteFailed, true);
            co_return out;
        }
    }
    if (const auto* immediate = prepared.contentPlan().immediate();
        immediate != nullptr && !immediate->bytes().empty()) {
        deadline.expires_after(limits.ioTimeout);
        auto raced = co_await (
            asio::async_write(
                stream,
                asio::buffer(immediate->bytes().data(), immediate->bytes().size()),
                tuple) ||
            deadline.async_wait(tuple));
        if (raced.index() == 1) {
            fail(OriginFetchOutcome::kTimeout, false);
            co_return out;
        }
        if (std::get<0>(std::get<0>(raced))) {
            fail(OriginFetchOutcome::kWriteFailed, true);
            co_return out;
        }
    }

    std::string inbound;
    std::array<char, kReadChunkBytes> readBuffer;
    bool timedOut = false;
    bool readError = false;
    bool eof = false;
    const auto readOnce = [&]() -> asio::awaitable<void> {
        deadline.expires_after(limits.ioTimeout);
        auto raced = co_await (
            stream.async_read_some(asio::buffer(readBuffer), tuple) ||
            deadline.async_wait(tuple));
        if (raced.index() == 1) {
            timedOut = true;
            co_return;
        }
        auto& [ec, n] = std::get<0>(raced);
        if (n > 0) {
            inbound.append(readBuffer.data(), n);
        }
        if (ec == asio::error::eof) {
            eof = true;
        } else if (ec) {
            readError = true;
        }
    };

    // Read and parse the response head, past any informational (1xx) responses.
    // Failures here happen before any valid response, so a reused connection can
    // be retried.
    Http1ClientResponseParser parser(prepared);
    OriginResponse response;
    BodyFraming framing = BodyFraming::kNone;
    std::size_t knownLength = 0;
    bool reusable = false;

    for (;;) {
        auto parseResult = parser.parse(inbound);
        if (parseResult.failure() != nullptr) {
            fail(OriginFetchOutcome::kProtocolError, false);
            co_return out;
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
            using Persistence = Http1ClientResponsePersistence;
            if (const auto* without = plan.withoutContent()) {
                framing = BodyFraming::kNone;
                reusable = without->persistence() == Persistence::kReuse;
            } else if (plan.zeroContent() != nullptr) {
                framing = BodyFraming::kNone;  // conservative: do not pool
            } else if (const auto* known = plan.knownLength()) {
                framing = BodyFraming::kKnownLength;
                knownLength = known->contentLength();
                reusable = known->persistence() == Persistence::kReuse;
            } else if (const auto* chunked = plan.chunked()) {
                framing = BodyFraming::kChunked;
                reusable = chunked->persistence() == Persistence::kReuse;
            } else if (plan.closeDelimited() != nullptr) {
                framing = BodyFraming::kCloseDelimited;  // always closes
            } else {
                fail(OriginFetchOutcome::kUnsupported, false);
                co_return out;
            }
            inbound.erase(0, consumed);
            break;
        }

        co_await readOnce();
        if (timedOut) {
            fail(OriginFetchOutcome::kTimeout, false);
            co_return out;
        }
        if (readError || eof) {
            fail(OriginFetchOutcome::kReadFailed, true);  // no head yet: retryable
            co_return out;
        }
    }

    // Read the body according to its framing. Errors here are post-head, so the
    // request is not retried on a fresh connection.
    switch (framing) {
        case BodyFraming::kNone:
            break;

        case BodyFraming::kKnownLength: {
            if (knownLength > limits.maxResponseBytes) {
                fail(OriginFetchOutcome::kTooLarge, false);
                co_return out;
            }
            while (inbound.size() < knownLength) {
                co_await readOnce();
                if (timedOut) {
                    fail(OriginFetchOutcome::kTimeout, false);
                    co_return out;
                }
                if (readError || eof) {
                    fail(OriginFetchOutcome::kReadFailed, false);
                    co_return out;
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
                    fail(OriginFetchOutcome::kTimeout, false);
                    co_return out;
                }
                if (inbound.size() > limits.maxResponseBytes) {
                    fail(OriginFetchOutcome::kTooLarge, false);
                    co_return out;
                }
                if (eof) {
                    break;
                }
                if (readError) {
                    fail(OriginFetchOutcome::kReadFailed, false);
                    co_return out;
                }
            }
            response.body = std::move(inbound);
            break;
        }

        case BodyFraming::kChunked: {
            ruvia::detail::Http1ChunkedBodyDecoder decoder(
                ProtocolByteLimit::limited(limits.maxResponseBytes));
            for (;;) {
                const auto decoded = decoder.decode(inbound);
                if (decoded.failure() != nullptr) {
                    fail(OriginFetchOutcome::kProtocolError, false);
                    co_return out;
                }
                if (const auto* chunk = decoded.bodyChunk()) {
                    response.body.append(chunk->bytes());
                    inbound.erase(0, decoded.consumedBytes());
                    if (response.body.size() > limits.maxResponseBytes) {
                        fail(OriginFetchOutcome::kTooLarge, false);
                        co_return out;
                    }
                    continue;
                }
                if (decoded.complete() != nullptr) {
                    break;
                }
                inbound.erase(0, decoded.consumedBytes());
                co_await readOnce();
                if (timedOut) {
                    fail(OriginFetchOutcome::kTimeout, false);
                    co_return out;
                }
                if (readError || eof) {
                    fail(OriginFetchOutcome::kReadFailed, false);
                    co_return out;
                }
            }
            break;
        }
    }

    out.result = OriginFetchResult{OriginFetchOutcome::kOk, std::move(response)};
    out.reusable = reusable;
    co_return out;
}

}  // namespace

std::size_t OriginFetcher::idleConnectionCount() const noexcept {
    std::size_t total = 0;
    for (const auto& [key, bucket] : idlePool_) {
        total += bucket.size();
    }
    return total;
}

asio::awaitable<OriginFetchResult> OriginFetcher::fetch(
    asio::any_io_executor executor,
    std::string_view host,
    std::uint16_t port,
    bool https,
    const OriginRequest& request) {
    // Prepare the request head once. kAllowReuse omits Connection: close so the
    // origin may keep the connection open for pooling.
    const HttpOrigin origin = HttpOrigin::http(host, port);
    HttpClientRequest clientRequest;
    clientRequest.method = request.method;
    clientRequest.target = request.target;
    clientRequest.headers = request.headers;
    clientRequest.content = request.body
        ? HttpClientRequestContent::bytes(*request.body)
        : HttpClientRequestContent::none();

    std::array<char, kHeadBufferBytes> headBuffer;
    const Http1ClientRequestWriter writer;
    const auto prepareResult = writer.prepare(
        origin, clientRequest, headBuffer,
        Http1ClientRequestWirePolicy::withoutExpectation(
            Http1ClientRequestClosePolicy::kAllowReuse));
    const auto* prepared = prepareResult.prepared();
    if (prepared == nullptr) {
        co_return failure(OriginFetchOutcome::kUnsupported);
    }

    std::string key(host);
    key.push_back(':');
    {
        std::array<char, 8> portText;
        const auto [end, ec] =
            std::to_chars(portText.data(), portText.data() + portText.size(), port);
        if (ec != std::errc{}) {
            co_return failure(OriginFetchOutcome::kConnectFailed);
        }
        key.append(portText.data(), static_cast<std::size_t>(end - portText.data()));
    }

    const auto tuple = asio::as_tuple(asio::use_awaitable);
    const std::string_view portView = std::string_view(key).substr(host.size() + 1);

    // TLS origin: resolve, connect, TLS-handshake with SNI, then run the exchange
    // over the encrypted stream. TLS origin connections are not pooled.
    if (https) {
        asio::steady_timer deadline(executor);
        asio::ip::tcp::resolver resolver(executor);
        deadline.expires_after(limits_.connectTimeout);
        auto resolveRaced = co_await (
            resolver.async_resolve(host, portView, tuple) || deadline.async_wait(tuple));
        if (resolveRaced.index() == 1) {
            co_return failure(OriginFetchOutcome::kTimeout);
        }
        if (std::get<0>(std::get<0>(resolveRaced))) {
            co_return failure(OriginFetchOutcome::kConnectFailed);
        }
        const auto endpoints = std::move(std::get<1>(std::get<0>(resolveRaced)));

        asio::ip::tcp::socket tcpSocket(executor);
        auto connectRaced = co_await (
            asio::async_connect(tcpSocket, endpoints, tuple) || deadline.async_wait(tuple));
        if (connectRaced.index() == 1) {
            co_return failure(OriginFetchOutcome::kTimeout);
        }
        if (std::get<0>(std::get<0>(connectRaced))) {
            co_return failure(OriginFetchOutcome::kConnectFailed);
        }

        asio::ssl::stream<asio::ip::tcp::socket> tls(std::move(tcpSocket), originTlsContext_);
        const std::string hostString(host);
        SSL_set_tlsext_host_name(tls.native_handle(), hostString.c_str());

        deadline.expires_after(limits_.connectTimeout);
        auto handshakeRaced = co_await (
            tls.async_handshake(asio::ssl::stream_base::client, tuple) ||
            deadline.async_wait(tuple));
        if (handshakeRaced.index() == 1) {
            co_return failure(OriginFetchOutcome::kTimeout);
        }
        if (std::get<0>(std::get<0>(handshakeRaced))) {
            co_return failure(OriginFetchOutcome::kConnectFailed);
        }

        auto exchange = co_await runExchange(tls, *prepared, limits_);
        co_return std::move(exchange.result);
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        // Acquire a connection: reuse a fresh-enough pooled one, else connect.
        // The retry attempt (attempt > 0) always connects fresh.
        asio::ip::tcp::socket socket(executor);
        bool reused = false;
        if (auto it = attempt == 0 ? idlePool_.find(key) : idlePool_.end();
            it != idlePool_.end()) {
            auto& bucket = it->second;
            const auto now = std::chrono::steady_clock::now();
            while (!bucket.empty()) {
                auto pooled = std::move(bucket.back());
                bucket.pop_back();
                if (now - pooled.idleSince <= limits_.idleTimeout) {
                    socket = std::move(pooled.socket);
                    reused = true;
                    break;
                }
                std::error_code ignore;
                pooled.socket.close(ignore);  // idle too long
            }
        }

        if (!reused) {
            asio::ip::tcp::resolver resolver(executor);
            asio::steady_timer deadline(executor);
            deadline.expires_after(limits_.connectTimeout);
            auto resolveRaced = co_await (
                resolver.async_resolve(host, std::string_view(key).substr(host.size() + 1),
                                       tuple) ||
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
        }

        auto exchange = co_await runExchange(socket, *prepared, limits_);

        // A reused pooled connection the origin had closed: retry once fresh.
        if (exchange.transportFailed && reused && attempt == 0) {
            std::error_code ignore;
            socket.close(ignore);
            continue;
        }

        if (exchange.result.outcome == OriginFetchOutcome::kOk && exchange.reusable) {
            auto& bucket = idlePool_[key];
            if (bucket.size() < limits_.maxIdlePerHost) {
                bucket.push_back(
                    PooledConnection{std::move(socket), std::chrono::steady_clock::now()});
            } else {
                std::error_code ignore;
                socket.close(ignore);
            }
        }

        co_return std::move(exchange.result);
    }

    co_return failure(OriginFetchOutcome::kConnectFailed);
}

}  // namespace ruvia::edge
