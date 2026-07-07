
#include "HttpClientPool.h"

#include <asio/connect.hpp>
#include <asio/ip/address.hpp>
#include <asio/read.hpp>
#include <asio/ssl/error.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <string>
#include <system_error>

#include "runtime/AsioAwait.h"
#include "../HeaderTokenUtils.h"
#include "HttpClientAccess.h"
#include "HttpClientContentEncoding.h"
#include "HttpClientRedirect.h"
#include "HttpClientResponseLimits.h"
#include "HttpClientResponseParser.h"
#include "HttpClientTlsVerification.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool isReservedHttpClientRequestHeader(std::string_view name) noexcept {
    return asciiEqualsIgnoreCase(name, "Host") ||
        asciiEqualsIgnoreCase(name, "Connection") ||
        asciiEqualsIgnoreCase(name, "Keep-Alive") ||
        asciiEqualsIgnoreCase(name, "Proxy-Connection") ||
        asciiEqualsIgnoreCase(name, "TE") ||
        asciiEqualsIgnoreCase(name, "Trailer") ||
        asciiEqualsIgnoreCase(name, "Content-Length") ||
        asciiEqualsIgnoreCase(name, "Transfer-Encoding") ||
        asciiEqualsIgnoreCase(name, "Upgrade");
}

void validateHttpClientRequestHead(
    std::string_view method,
    std::string_view target) {
    if (!isValidHttpHeaderName(method)) {
        throw std::invalid_argument("http client: invalid request method");
    }
    if (!isValidHttpClientOriginTarget(target)) {
        throw std::invalid_argument("http client: invalid request target");
    }
}

void validateHttpClientRequestHeader(const HttpHeaderView& header) {
    if (!isValidHttpHeaderName(header.name())) {
        throw std::invalid_argument("http client: invalid request header name");
    }
    if (!isValidHttpHeaderValue(header.value())) {
        throw std::invalid_argument("http client: invalid request header value");
    }
    if (isReservedHttpClientRequestHeader(header.name())) {
        throw std::invalid_argument("http client: request header is managed by the client");
    }
}

void addHttpClientRequestHeadBytes(std::size_t& total, std::size_t bytes) {
    if (bytes > kMaxHttpHeaderBytes || total > kMaxHttpHeaderBytes - bytes) {
        throw std::invalid_argument("http client: request headers too large");
    }
    total += bytes;
}

}  // namespace

// Connect

Task<void> HttpClientPool::connectOne(Connection& conn) {
    if (conn.rawSocket.is_open()) {
        std::error_code ignored;
        if (conn.tlsStream) {
            conn.tlsStream->lowest_layer().close(ignored);
        } else {
            conn.rawSocket.close(ignored);
        }
        conn.connected = false;
    }
    // Create a fresh socket for each reconnect attempt
    conn.rawSocket = asio::ip::tcp::socket(ioContext_);

    std::array<char, 5> portBuffer;
    const auto [portEnd, portEc] = std::to_chars(
        portBuffer.data(),
        portBuffer.data() + portBuffer.size(),
        config_.port);
    if (portEc != std::errc{}) {
        throw std::logic_error("http client: invalid port");
    }
    const auto port = std::string_view(portBuffer.data(), static_cast<std::size_t>(portEnd - portBuffer.data()));
    setDeadline(conn, config_.proxyConnectTimeout, Connection::DeadlineKind::kResolve);
    auto [resolveEc, endpoints] = co_await asyncResult<asio::ip::tcp::resolver::results_type>(
        [this, port, &conn](auto handler) {
            conn.resolver.async_resolve(
                std::string_view(config_.host),
                port,
                std::move(handler));
        });
    if (finishDeadline(conn)) {
        throw std::system_error(asio::error::timed_out, "http client: resolve timed out");
    }
    if (resolveEc) {
        throw std::system_error(
            resolveEc,
            "http client: resolve failed");
    }

    setDeadline(conn, config_.proxyConnectTimeout, Connection::DeadlineKind::kSocket);
    auto [connectEc, ep] = co_await asyncResult<asio::ip::tcp::endpoint>(
        [&](auto handler) {
            asio::async_connect(conn.rawSocket, endpoints, std::move(handler));
        });
    (void)ep;
    if (finishDeadline(conn)) {
        throw std::system_error(asio::error::timed_out, "http client: connect timed out");
    }
    if (connectEc) {
        throw std::system_error(connectEc, "http client: connect failed");
    }

    {
        std::error_code ignored;
        conn.rawSocket.set_option(asio::ip::tcp::no_delay(true), ignored);
    }

    if (config_.tls) {
        // Always start from fresh SSL state: a stream that already completed a handshake
        // cannot be re-handshaked, so a reused connection needs a new one. The stream binds
        // to conn.rawSocket by reference (stable address), which now holds the fresh socket.
        conn.tlsStream.reset();
        using TlsStream = Connection::TlsStream;
        conn.tlsStream = makePmrObject<TlsStream>(conn.resource, conn.rawSocket, *sslContext_);
        // RFC 6066 SNI + RFC 6125 host-name verification (verify_peer only checks the
        // chain), shared with the HTTP/2 path via one owner.
        applyClientTlsIdentity(
            *conn.tlsStream,
            config_.tlsOptions.sniHost.empty() ? config_.host : config_.tlsOptions.sniHost,
            config_.tlsOptions.insecureSkipVerify,
            conn.resource);
        setDeadline(conn, config_.proxyConnectTimeout, Connection::DeadlineKind::kSocket);
        const auto handshakeEc = co_await asyncError([&](auto handler) {
            conn.tlsStream->async_handshake(
                asio::ssl::stream_base::client, std::move(handler));
        });
        if (finishDeadline(conn)) {
            throw std::system_error(asio::error::timed_out, "http client: TLS handshake timed out");
        }
        if (handshakeEc) {
            throw std::system_error(handshakeEc, "http client: TLS handshake failed");
        }
    }

    conn.connected = true;
}

// Request

Task<FetchResponse> HttpClientPool::fetch(
    std::string_view path,
    const FetchOptions& options,
    std::pmr::memory_resource* resource) {
    if (closing_) {
        throw std::runtime_error("http client pool is closed");
    }
    auto* const requestResource = resource == nullptr ? resource_ : resource;

    std::string_view currentPath = path;
    FetchOptions currentOptions = options;
    // Stable storage for a resolved redirect target; currentPath points into it across hops.
    std::pmr::string redirectTarget(requestResource);
    std::uint32_t hopsRemaining = options.maxRedirects;

    for (;;) {
        const auto index = co_await acquire();
        FetchResponse response = FetchResponseAccess::make(requestResource);
        {
            ConnectionGuard guard(*this, index);
            auto& conn = guard.connection();
            try {
                if (!conn.connected) {
                    co_await connectOne(conn);
                }
                response = co_await executeRequest(conn, currentPath, currentOptions, requestResource);
            } catch (...) {
                guard.discard();
                throw;
            }
        }  // release the connection back to the pool before following a redirect

        if (hopsRemaining == 0 || !isHttpClientRedirectStatus(response.status())) {
            co_return response;
        }
        if (!canReplayHttpClientRedirectRequest(currentOptions, response.status())) {
            co_return response;
        }
        const auto location = findUniqueHttpClientResponseHeader(response, "Location");
        if (location.empty() ||
            !resolveHttpClientSameOriginRedirect(config_, location, redirectTarget)) {
            // No Location, or a cross-origin/unparseable one: hand the 3xx back to the caller.
            co_return response;
        }
        currentPath = redirectTarget;
        applyHttpClientRedirectMethod(currentOptions, response.status());
        --hopsRemaining;
    }
}

Task<void> HttpClientPool::readChunkedResponseBody(
    Connection& conn,
    FetchResponse& response,
    std::size_t bodyOffset,
    std::chrono::milliseconds readTimeout) {
    // Bound a single chunk-size / trailer line and the whole trailer section so a hostile
    // peer cannot force unbounded buffering.
    constexpr std::size_t kMaxChunkLineBytes = 1024;
    constexpr std::size_t kMaxTrailerBytes = kMaxHttpHeaderBytes;

    auto& buf = conn.responseReadBuffer;
    std::size_t pos = bodyOffset;

    auto readSome = [&conn](asio::mutable_buffer buffer) {
        return asyncResult<std::size_t>([&conn, buffer](auto handler) mutable {
            if (conn.tlsStream) {
                conn.tlsStream->async_read_some(buffer, std::move(handler));
            } else {
                conn.rawSocket.async_read_some(buffer, std::move(handler));
            }
        });
    };
    auto readExact = [&conn](asio::mutable_buffer buffer) {
        return asyncResult<std::size_t>([&conn, buffer](auto handler) mutable {
            if (conn.tlsStream) {
                asio::async_read(*conn.tlsStream, buffer, std::move(handler));
            } else {
                asio::async_read(conn.rawSocket, buffer, std::move(handler));
            }
        });
    };

    // Append more bytes to buf, compacting the already-parsed prefix so the buffered window
    // stays small. Throws on timeout, error, or premature EOF.
    auto fill = [&]() -> Task<void> {
        if (pos > 0) {
            buf.erase(0, pos);
            pos = 0;
        }
        constexpr std::size_t kReadChunk = 4096;
        const auto oldSize = buf.size();
        resizePmrStringForOverwrite(buf, oldSize + kReadChunk);
        setDeadline(conn, readTimeout, Connection::DeadlineKind::kSocket);
        auto [ec, n] = co_await readSome(asio::buffer(buf.data() + oldSize, kReadChunk));
        if (finishDeadline(conn)) {
            closeConnection(conn);
            throw std::system_error(asio::error::timed_out, "http client: read chunk timed out");
        }
        if (ec) {
            conn.connected = false;
            throw std::system_error(ec, "http client: read chunk failed");
        }
        if (n == 0) {
            closeConnection(conn);
            throw std::runtime_error("http client: truncated chunked response");
        }
        buf.resize(oldSize + n);
    };

    // Return the next CRLF-terminated line (without the CRLF) and advance pos past it.
    auto readLine = [&](std::size_t maxLen) -> Task<std::string_view> {
        for (;;) {
            const auto nl = buf.find("\r\n", pos);
            if (nl != std::pmr::string::npos) {
                const auto start = pos;
                pos = nl + 2;
                co_return std::string_view(buf.data() + start, nl - start);
            }
            if (buf.size() - pos > maxLen) {
                closeConnection(conn);
                throw std::runtime_error("http client: chunked line too long");
            }
            co_await fill();
        }
    };

    std::size_t totalBody = 0;
    const auto maxBody = config_.maxResponseBodyBytes;

    for (;;) {
        const auto sizeLine = co_await readLine(kMaxChunkLineBytes);
        auto sizeToken = sizeLine;
        if (const auto semi = sizeToken.find(';'); semi != std::string_view::npos) {
            sizeToken = sizeToken.substr(0, semi);  // drop chunk extensions
        }
        sizeToken = httpTrimOws(sizeToken);
        std::size_t chunkSize = 0;
        const auto [ptr, ec] = std::from_chars(
            sizeToken.data(), sizeToken.data() + sizeToken.size(), chunkSize, 16);
        if (sizeToken.empty() || ec != std::errc{} || ptr != sizeToken.data() + sizeToken.size()) {
            closeConnection(conn);
            throw std::runtime_error("http client: malformed chunk size");
        }

        if (chunkSize == 0) {
            // Final chunk: drain the optional trailer section (lines up to an empty line).
            std::size_t trailerBytes = 0;
            for (;;) {
                const auto trailer = co_await readLine(kMaxChunkLineBytes);
                if (trailer.empty()) {
                    break;
                }
                trailerBytes += trailer.size() + 2;
                if (trailerBytes > kMaxTrailerBytes) {
                    closeConnection(conn);
                    throw std::runtime_error("http client: chunked trailers too large");
                }
            }
            // Any bytes past the terminating CRLF are an unexpected extra/pipelined
            // response; the connection can no longer be safely reused.
            if (pos != buf.size()) {
                closeConnection(conn);
            }
            break;
        }

        if (maxBody != 0 && chunkSize > maxBody - totalBody) {
            closeConnection(conn);
            throw std::runtime_error("http client: response body is too large");
        }
        totalBody += chunkSize;

        auto& body = FetchResponseAccess::body(response);
        const auto writeAt = body.size();
        resizePmrStringForOverwrite(body, writeAt + chunkSize);
        const auto buffered = buf.size() - pos;
        const auto copied = std::min(buffered, chunkSize);
        if (copied > 0) {
            std::copy_n(buf.data() + pos, copied, body.data() + writeAt);
            pos += copied;
        }
        if (copied < chunkSize) {
            setDeadline(conn, readTimeout, Connection::DeadlineKind::kSocket);
            auto [dataEc, dataN] = co_await readExact(asio::buffer(
                body.data() + writeAt + copied, chunkSize - copied));
            (void)dataN;
            if (finishDeadline(conn)) {
                closeConnection(conn);
                throw std::system_error(asio::error::timed_out, "http client: read chunk data timed out");
            }
            if (dataEc) {
                conn.connected = false;
                throw std::system_error(dataEc, "http client: read chunk data failed");
            }
        }

        // Each chunk's data is followed by its own CRLF.
        const auto terminator = co_await readLine(kMaxChunkLineBytes);
        if (!terminator.empty()) {
            closeConnection(conn);
            throw std::runtime_error("http client: malformed chunk terminator");
        }
    }
}

Task<void> HttpClientPool::readCloseDelimitedResponseBody(
    Connection& conn,
    FetchResponse& response,
    std::size_t bodyOffset,
    std::chrono::milliseconds readTimeout) {
    // RFC 7230 §3.3.3 rule 7: with neither Transfer-Encoding nor Content-Length, the body
    // runs until the peer closes the connection. The caller marks the connection unreusable.
    const auto maxBody = config_.maxResponseBodyBytes;
    auto& buf = conn.responseReadBuffer;

    auto readSome = [&conn](asio::mutable_buffer buffer) {
        return asyncResult<std::size_t>([&conn, buffer](auto handler) mutable {
            if (conn.tlsStream) {
                conn.tlsStream->async_read_some(buffer, std::move(handler));
            } else {
                conn.rawSocket.async_read_some(buffer, std::move(handler));
            }
        });
    };

    // Seed the body with the bytes already buffered past the header block.
    auto& body = FetchResponseAccess::body(response);
    if (buf.size() > bodyOffset) {
        const auto available = buf.size() - bodyOffset;
        if (maxBody != 0 && available > maxBody) {
            closeConnection(conn);
            throw std::runtime_error("http client: response body is too large");
        }
        body.assign(buf.data() + bodyOffset, available);
    }

    constexpr std::size_t kReadChunk = 8192;
    for (;;) {
        const auto oldSize = body.size();
        resizePmrStringForOverwrite(body, oldSize + kReadChunk);
        setDeadline(conn, readTimeout, Connection::DeadlineKind::kSocket);
        auto [ec, n] = co_await readSome(asio::buffer(body.data() + oldSize, kReadChunk));
        if (finishDeadline(conn)) {
            body.resize(oldSize);
            closeConnection(conn);
            throw std::system_error(asio::error::timed_out, "http client: read body timed out");
        }
        // A peer close (TCP EOF, or a TLS shutdown/truncation) is the normal end of a
        // close-delimited body -- keep any bytes delivered alongside it and stop.
        if (ec == asio::error::eof || ec == asio::ssl::error::stream_truncated) {
            body.resize(oldSize + n);
            if (maxBody != 0 && body.size() > maxBody) {
                closeConnection(conn);
                throw std::runtime_error("http client: response body is too large");
            }
            break;
        }
        if (ec) {
            body.resize(oldSize);
            conn.connected = false;
            throw std::system_error(ec, "http client: read body failed");
        }
        body.resize(oldSize + n);
        if (maxBody != 0 && body.size() > maxBody) {
            closeConnection(conn);
            throw std::runtime_error("http client: response body is too large");
        }
    }
}

Task<std::error_code> HttpClientPool::connWrite(
    Connection& conn, std::array<asio::const_buffer, 2> buffers) {
    co_return co_await asyncError([&conn, buffers](auto handler) mutable {
        if (conn.tlsStream) {
            asio::async_write(*conn.tlsStream, buffers, std::move(handler));
        } else {
            asio::async_write(conn.rawSocket, buffers, std::move(handler));
        }
    });
}

Task<std::pair<std::error_code, std::size_t>> HttpClientPool::connReadSome(
    Connection& conn, asio::mutable_buffer buffer) {
    co_return co_await asyncResult<std::size_t>([&conn, buffer](auto handler) mutable {
        if (conn.tlsStream) {
            conn.tlsStream->async_read_some(buffer, std::move(handler));
        } else {
            conn.rawSocket.async_read_some(buffer, std::move(handler));
        }
    });
}

Task<std::pair<std::error_code, std::size_t>> HttpClientPool::connRead(
    Connection& conn, asio::mutable_buffer buffer) {
    co_return co_await asyncResult<std::size_t>([&conn, buffer](auto handler) mutable {
        if (conn.tlsStream) {
            asio::async_read(*conn.tlsStream, buffer, std::move(handler));
        } else {
            asio::async_read(conn.rawSocket, buffer, std::move(handler));
        }
    });
}

Task<void> HttpClientPool::writeChunkedRequestBody(
    Connection& conn, const RequestBodyStream& bodyStream, std::chrono::milliseconds sendTimeout) {
    for (;;) {
        const std::string_view chunk = co_await bodyStream.nextChunk();
        if (chunk.empty()) {
            break;  // end of body
        }
        std::array<char, 18> sizeLine;  // up to 16 hex digits + CRLF
        auto [ptr, ec] = std::to_chars(sizeLine.data(), sizeLine.data() + 16, chunk.size(), 16);
        if (ec != std::errc{}) {
            closeConnection(conn);
            throw std::logic_error("http client: failed to format request chunk size");
        }
        *ptr++ = '\r';
        *ptr++ = '\n';
        static constexpr char kCrlf[] = {'\r', '\n'};
        const std::array<asio::const_buffer, 3> buffers{
            asio::buffer(sizeLine.data(), static_cast<std::size_t>(ptr - sizeLine.data())),
            asio::buffer(chunk.data(), chunk.size()),
            asio::buffer(kCrlf, sizeof(kCrlf))};
        setDeadline(conn, sendTimeout, Connection::DeadlineKind::kSocket);
        const auto writeEc = co_await asyncError([&conn, buffers](auto handler) mutable {
            if (conn.tlsStream) {
                asio::async_write(*conn.tlsStream, buffers, std::move(handler));
            } else {
                asio::async_write(conn.rawSocket, buffers, std::move(handler));
            }
        });
        if (finishDeadline(conn)) {
            closeConnection(conn);
            throw std::system_error(asio::error::timed_out, "http client: request body write timed out");
        }
        if (writeEc) {
            conn.connected = false;
            throw std::system_error(writeEc, "http client: request body write failed");
        }
    }
    static constexpr char kTerminator[] = {'0', '\r', '\n', '\r', '\n'};
    const std::array<asio::const_buffer, 2> terminator{
        asio::buffer(kTerminator, sizeof(kTerminator)), asio::buffer(kTerminator, 0)};
    setDeadline(conn, sendTimeout, Connection::DeadlineKind::kSocket);
    const auto writeEc = co_await connWrite(conn, terminator);
    if (finishDeadline(conn)) {
        closeConnection(conn);
        throw std::system_error(asio::error::timed_out, "http client: request terminator timed out");
    }
    if (writeEc) {
        conn.connected = false;
        throw std::system_error(writeEc, "http client: request terminator write failed");
    }
}

Task<HttpClientResponseHead> HttpClientPool::writeRequestAndReadHead(
    Connection& conn,
    std::string_view path,
    const FetchOptions& options,
    std::chrono::milliseconds readTimeout,
    std::chrono::milliseconds sendTimeout,
    FetchResponse& response,
    std::pmr::memory_resource* requestResource) {
    // Build request
    const auto method = options.method.empty() ? std::string_view("GET") : options.method;
    const auto effectivePath = path.empty() ? std::string_view("/") : path;
    validateHttpClientRequestHead(method, effectivePath);

    const bool streamingBody = static_cast<bool>(options.bodyStream);
    if (streamingBody && !options.body.empty()) {
        throw std::invalid_argument("http client: set either body or bodyStream, not both");
    }
    const bool hasBody = streamingBody || !options.body.empty();
    // Expect: 100-continue is only meaningful with a body; if the caller already put an Expect
    // header in, don't duplicate it (we still run the wait dance for their header).
    bool userSetExpect = false;
    for (const auto& hdr : options.headers) {
        if (asciiEqualsIgnoreCase(hdr.name(), "Expect")) {
            userSetExpect = true;
            break;
        }
    }
    const bool wantContinue = options.expectContinue && hasBody;
    const bool addExpectHeader = wantContinue && !userSetExpect;
    // nginx-style inactivity: each write resets sendTimeout (proxy_send_timeout), each read resets
    // readTimeout (proxy_read_timeout). The timer is relative and re-armed per I/O.
    auto armRead = [&]() {
        setDeadline(conn, readTimeout, Connection::DeadlineKind::kSocket);
    };
    auto armWrite = [&]() {
        setDeadline(conn, sendTimeout, Connection::DeadlineKind::kSocket);
    };

    std::array<char, 24> lenBuf;
    std::string_view contentLengthValue;
    if (!options.body.empty()) {
        const auto [ptr, ec] = std::to_chars(
            lenBuf.data(), lenBuf.data() + lenBuf.size(), options.body.size());
        if (ec != std::errc{}) {
            throw std::logic_error("failed to format request content length");
        }
        contentLengthValue = std::string_view(lenBuf.data(), static_cast<std::size_t>(ptr - lenBuf.data()));
    }
    constexpr std::string_view kChunkedHeader = "Transfer-Encoding: chunked\r\n";

    std::size_t requestHeadBytes = 0;
    addHttpClientRequestHeadBytes(requestHeadBytes, method.size());
    addHttpClientRequestHeadBytes(requestHeadBytes, 1);
    addHttpClientRequestHeadBytes(requestHeadBytes, effectivePath.size());
    addHttpClientRequestHeadBytes(requestHeadBytes, std::string_view(" HTTP/1.1\r\nHost: ").size());
    addHttpClientRequestHeadBytes(requestHeadBytes, hostHeader_.size());
    addHttpClientRequestHeadBytes(requestHeadBytes, std::string_view("\r\nConnection: keep-alive\r\n").size());
    for (const auto& hdr : options.headers) {
        validateHttpClientRequestHeader(hdr);
        addHttpClientRequestHeadBytes(requestHeadBytes, hdr.name().size());
        addHttpClientRequestHeadBytes(requestHeadBytes, 2);
        addHttpClientRequestHeadBytes(requestHeadBytes, hdr.value().size());
        addHttpClientRequestHeadBytes(requestHeadBytes, 2);
    }
    if (!contentLengthValue.empty()) {
        addHttpClientRequestHeadBytes(requestHeadBytes, std::string_view("Content-Length: ").size());
        addHttpClientRequestHeadBytes(requestHeadBytes, contentLengthValue.size());
        addHttpClientRequestHeadBytes(requestHeadBytes, 2);
    }
    if (streamingBody) {
        addHttpClientRequestHeadBytes(requestHeadBytes, kChunkedHeader.size());
    }
    constexpr std::string_view kExpectHeader = "Expect: 100-continue\r\n";
    if (addExpectHeader) {
        addHttpClientRequestHeadBytes(requestHeadBytes, kExpectHeader.size());
    }
    addHttpClientRequestHeadBytes(requestHeadBytes, 2);

    auto& requestBuf = conn.requestBuffer;
    requestBuf.clear();
    requestBuf.reserve(requestHeadBytes);
    requestBuf.append(method.data(), method.size());
    requestBuf.push_back(' ');
    requestBuf.append(effectivePath.data(), effectivePath.size());
    requestBuf.append(" HTTP/1.1\r\nHost: ");
    requestBuf.append(hostHeader_.data(), hostHeader_.size());
    requestBuf.append("\r\nConnection: keep-alive\r\n");

    for (const auto& hdr : options.headers) {
        const auto name = hdr.name();
        const auto value = hdr.value();
        requestBuf.append(name.data(), name.size());
        requestBuf.append(": ");
        requestBuf.append(value.data(), value.size());
        requestBuf.append("\r\n");
    }

    if (!contentLengthValue.empty()) {
        requestBuf.append("Content-Length: ");
        requestBuf.append(contentLengthValue.data(), contentLengthValue.size());
        requestBuf.append("\r\n");
    }
    if (streamingBody) {
        requestBuf.append(kChunkedHeader.data(), kChunkedHeader.size());
    }
    if (addExpectHeader) {
        requestBuf.append(kExpectHeader.data(), kExpectHeader.size());
    }
    requestBuf.append("\r\n");

    // Write the request head. Without Expect: 100-continue, the buffered body rides along in the
    // same write; with it, only the head goes now and the body is held until the server is ready.
    const std::array<asio::const_buffer, 2> requestBuffers{
        asio::const_buffer(requestBuf.data(), requestBuf.size()),
        wantContinue ? asio::const_buffer(options.body.data(), 0)
                     : asio::const_buffer(options.body.data(), options.body.size())};
    armWrite();
    const auto writeEc = co_await connWrite(conn, requestBuffers);
    if (finishDeadline(conn)) {
        throw std::system_error(asio::error::timed_out, "http client: write timed out");
    }
    if (writeEc) {
        conn.connected = false;
        throw std::system_error(writeEc, "http client: write failed");
    }

    // Sends the request body (streamed as chunks, or the buffered bytes). Not used for the
    // buffered non-Expect path above, which already wrote the body inline with the head.
    auto sendRequestBody = [&]() -> Task<void> {
        if (streamingBody) {
            co_await writeChunkedRequestBody(conn, options.bodyStream, sendTimeout);
        } else if (!options.body.empty()) {
            armWrite();
            const std::array<asio::const_buffer, 2> bodyBuffers{
                asio::buffer(options.body.data(), options.body.size()),
                asio::buffer(options.body.data(), 0)};
            const auto ec = co_await connWrite(conn, bodyBuffers);
            if (finishDeadline(conn)) {
                closeConnection(conn);
                throw std::system_error(asio::error::timed_out, "http client: body write timed out");
            }
            if (ec) {
                conn.connected = false;
                throw std::system_error(ec, "http client: body write failed");
            }
        }
        co_return;
    };

    // A streamed body without Expect goes now (a buffered non-Expect body rode with the head).
    if (!wantContinue && streamingBody) {
        co_await sendRequestBody();
    }

    // Read response headers
    auto& readBuf = conn.responseReadBuffer;
    readBuf.clear();
    readBuf.reserve(4096);

    std::size_t interimResponses = 0;

    // Expect: 100-continue -- wait (bounded) for the server's interim 100 before sending the
    // body. A final status (>= 200) arriving first means the server rejected up front: return it
    // without sending the body. If the server stays silent, send the body anyway once the bounded
    // window elapses so a server that ignores the expectation cannot deadlock us (RFC 7231 §5.1.1).
    if (wantContinue) {
        // Bound the wait for a 100 by a short window (capped at the read timeout) so a server that
        // ignores the expectation cannot deadlock us -- with per-read inactivity, a silent server
        // simply times out this read and we send the body.
        constexpr auto kExpectContinueTimeout = std::chrono::milliseconds(1000);
        const auto continueTimeout = readTimeout.count() > 0
            ? std::min<std::chrono::milliseconds>(readTimeout, kExpectContinueTimeout)
            : kExpectContinueTimeout;
        bool sendNow = false;
        for (;;) {
            auto headerEnd = readBuf.find("\r\n\r\n");
            while (headerEnd == std::pmr::string::npos) {
                if (readBuf.size() >= kMaxHttpHeaderBytes) {
                    closeConnection(conn);
                    throw std::runtime_error("http client: response headers too large");
                }
                const auto oldSize = readBuf.size();
                const auto writable = std::min<std::size_t>(4096, kMaxHttpHeaderBytes - oldSize);
                resizePmrStringForOverwrite(readBuf, oldSize + writable);
                setDeadline(conn, continueTimeout, Connection::DeadlineKind::kSocket);
                auto [readEc, n] = co_await connReadSome(conn, asio::buffer(readBuf.data() + oldSize, writable));
                if (finishDeadline(conn)) {
                    readBuf.resize(oldSize);  // drop the unfilled tail; keep any partial header
                    sendNow = true;
                    break;
                }
                if (readEc) {
                    conn.connected = false;
                    throw std::system_error(readEc, "http client: read failed");
                }
                if (n == 0) {
                    closeConnection(conn);
                    throw std::runtime_error("http client: empty response read");
                }
                readBuf.resize(oldSize + n);
                const auto searchOffset = oldSize > 3 ? oldSize - 3 : 0;
                headerEnd = readBuf.find("\r\n\r\n", searchOffset);
            }
            if (sendNow) {
                break;
            }
            auto head = parseHttpClientResponseHead(
                method, std::string_view(readBuf.data(), headerEnd), response, requestResource);
            const auto status = response.status();
            if (status >= 100 && status < 200 && status != 101) {
                // 100 means "send the body"; any other interim (e.g. 103) we skip and keep waiting.
                const bool isContinue = status == 100;
                if (!isContinue && ++interimResponses > kMaxHttpClientInterimResponses) {
                    closeConnection(conn);
                    throw std::runtime_error("http client: too many interim responses");
                }
                readBuf.erase(0, headerEnd + 4);
                FetchResponseAccess::setStatus(response, 0);
                FetchResponseAccess::headers(response).clear();
                FetchResponseAccess::body(response).clear();
                if (isContinue) {
                    sendNow = true;
                    break;
                }
                continue;
            }
            // A final response (>= 200) or 101 arrived before the body: the server answered up
            // front. Don't send the body; discard the connection (we advertised a body/length we
            // won't transmit, so it isn't safe to reuse) and return this head to the caller.
            head.closeAfterResponse = true;
            co_return head;
        }
        co_await sendRequestBody();
    }

    for (;;) {
        auto headerEnd = readBuf.find("\r\n\r\n");
        while (headerEnd == std::pmr::string::npos) {
            if (readBuf.size() >= kMaxHttpHeaderBytes) {
                closeConnection(conn);
                throw std::runtime_error("http client: response headers too large");
            }

            const auto oldSize = readBuf.size();
            const auto writable = std::min<std::size_t>(4096, kMaxHttpHeaderBytes - oldSize);
            resizePmrStringForOverwrite(readBuf, oldSize + writable);
            armRead();
            auto [readEc, n] = co_await connReadSome(conn, asio::buffer(readBuf.data() + oldSize, writable));
            if (finishDeadline(conn)) {
                closeConnection(conn);
                throw std::system_error(asio::error::timed_out, "http client: read timed out");
            }
            if (readEc) {
                conn.connected = false;
                throw std::system_error(readEc, "http client: read failed");
            }
            if (n == 0) {
                closeConnection(conn);
                throw std::runtime_error("http client: empty response read");
            }
            readBuf.resize(oldSize + n);
            const auto searchOffset = oldSize > 3 ? oldSize - 3 : 0;
            headerEnd = readBuf.find("\r\n\r\n", searchOffset);
        }

        auto head = parseHttpClientResponseHead(
            method,
            std::string_view(readBuf.data(), headerEnd),
            response,
            requestResource);
        const auto status = response.status();
        if (status >= 100 && status < 200 && status != 101) {
            if (++interimResponses > kMaxHttpClientInterimResponses) {
                closeConnection(conn);
                throw std::runtime_error("http client: too many interim responses");
            }
            readBuf.erase(0, headerEnd + 4);
            FetchResponseAccess::setStatus(response, 0);
            FetchResponseAccess::headers(response).clear();
            FetchResponseAccess::body(response).clear();
            continue;
        }
        co_return head;
    }
}

Task<FetchResponse> HttpClientPool::executeRequest(
    Connection& conn,
    std::string_view path,
    const FetchOptions& options,
    std::pmr::memory_resource* requestResource) {
    if (options.timeout.count() < 0) {
        throw std::invalid_argument("http client request timeout must not be negative");
    }
    // nginx-style inactivity timeouts (proxy_read_timeout / proxy_send_timeout), each reset per
    // I/O; FetchOptions::timeout overrides both for this request.
    const auto readTimeout = options.timeout.count() > 0 ? options.timeout : config_.proxyReadTimeout;
    const auto sendTimeout = options.timeout.count() > 0 ? options.timeout : config_.proxySendTimeout;

    FetchResponse response = FetchResponseAccess::make(requestResource);
    auto responseHead = co_await writeRequestAndReadHead(
        conn, path, options, readTimeout, sendTimeout, response, requestResource);
    auto& readBuf = conn.responseReadBuffer;

    // Collect body per RFC 7230 §3.3.3: chunked, then Content-Length, else close-delimited.
    if (responseHead.responseMayHaveBody) {
        if (responseHead.hasTransferEncoding) {
            if (!responseHead.isChunked) {
                closeConnection(conn);
                throw std::runtime_error("http client: unsupported response Transfer-Encoding");
            }
            co_await readChunkedResponseBody(conn, response, responseHead.bodyOffset, readTimeout);
        } else if (responseHead.hasContentLength) {
            if (config_.maxResponseBodyBytes != 0 &&
                responseHead.contentLength > config_.maxResponseBodyBytes) {
                closeConnection(conn);
                throw std::runtime_error("http client: response body is too large");
            }
            if (responseHead.contentLength > 0) {
                auto& body = FetchResponseAccess::body(response);
                resizePmrStringForOverwrite(body, responseHead.contentLength);
                const auto alreadyRead = readBuf.size() > responseHead.bodyOffset
                    ? readBuf.size() - responseHead.bodyOffset
                    : std::size_t{0};
                const auto toCopy = std::min(alreadyRead, responseHead.contentLength);
                if (alreadyRead > responseHead.contentLength) {
                    responseHead.closeAfterResponse = true;
                }
                if (toCopy > 0) {
                    std::copy_n(readBuf.data() + responseHead.bodyOffset, toCopy, body.data());
                }
                if (toCopy < responseHead.contentLength) {
                    setDeadline(conn, readTimeout, Connection::DeadlineKind::kSocket);
                    auto [bodyEc, bodyN] = co_await connRead(conn, asio::buffer(
                        body.data() + toCopy,
                        responseHead.contentLength - toCopy));
                    (void)bodyN;
                    if (finishDeadline(conn)) {
                        closeConnection(conn);
                        throw std::system_error(asio::error::timed_out, "http client: read body timed out");
                    }
                    if (bodyEc) {
                        conn.connected = false;
                        throw std::system_error(bodyEc, "http client: read body failed");
                    }
                }
            } else if (readBuf.size() > responseHead.bodyOffset) {
                responseHead.closeAfterResponse = true;
            }
        } else {
            co_await readCloseDelimitedResponseBody(
                conn, response, responseHead.bodyOffset, readTimeout);
            // A close-delimited body consumes the connection; it cannot be reused.
            responseHead.closeAfterResponse = true;
        }
    } else if (readBuf.size() > responseHead.bodyOffset) {
        responseHead.closeAfterResponse = true;
    }

    // Transparently decode a supported Content-Encoding once the full body is in hand.
    decodeHttpClientResponseContentEncoding(
        response,
        config_.maxResponseBodyBytes != 0 ? config_.maxResponseBodyBytes : kMaxDecodedRequestBodyBytes);

    if (responseHead.closeAfterResponse) {
        closeConnection(conn);
    }

    co_return response;
}

}  // namespace ruvia::detail
