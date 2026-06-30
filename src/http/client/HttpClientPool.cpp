#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include "HttpClientPool.h"

#include <asio/connect.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <system_error>

#include "../../runtime/AsioAwait.h"
#include "../HeaderTokenUtils.h"
#include "HttpClientResponseParser.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool isValidHttpClientOriginTarget(std::string_view target) noexcept {
    if (target.empty()) {
        return false;
    }
    if (target == "*") {
        return true;
    }
    if (target.front() != '/') {
        return false;
    }
    for (const auto ch : target) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte == 0x7F || byte == '#' || byte == '\\') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isReservedHttpClientRequestHeader(std::string_view name) noexcept {
    return httpAsciiEqualsIgnoreCase(name, "Host") ||
        httpAsciiEqualsIgnoreCase(name, "Connection") ||
        httpAsciiEqualsIgnoreCase(name, "Content-Length") ||
        httpAsciiEqualsIgnoreCase(name, "Transfer-Encoding");
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

void validateHttpClientRequestHeader(const FetchRequestHeader& header) {
    if (!isValidHttpHeaderName(header.name)) {
        throw std::invalid_argument("http client: invalid request header name");
    }
    if (!isValidHttpHeaderValue(header.value)) {
        throw std::invalid_argument("http client: invalid request header value");
    }
    if (isReservedHttpClientRequestHeader(header.name)) {
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
    setDeadline(conn, config_.connectTimeout, Connection::DeadlineKind::kResolve);
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

    setDeadline(conn, config_.connectTimeout, Connection::DeadlineKind::kSocket);
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
        if (!conn.tlsStream) {
            using TlsStream = Connection::TlsStream;
            conn.tlsStream = makePmrObject<TlsStream>(conn.resource, conn.rawSocket, *sslContext_);
        }
        setDeadline(conn, config_.connectTimeout, Connection::DeadlineKind::kSocket);
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

    const auto index = co_await acquire();
    ConnectionGuard guard(*this, index);
    auto& conn = guard.connection();

    try {
        if (!conn.connected) {
            co_await connectOne(conn);
        }
        co_return co_await executeRequest(conn, path, options, requestResource);
    } catch (...) {
        guard.discard();
        throw;
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
    const auto requestTimeout = options.timeout.count() > 0 ? options.timeout : config_.requestTimeout;
    auto asyncWriteConnection = [&conn](auto buffers) {
        return asyncError([&conn, buffers](auto handler) mutable {
            if (conn.tlsStream) {
                asio::async_write(*conn.tlsStream, buffers, std::move(handler));
            } else {
                asio::async_write(conn.rawSocket, buffers, std::move(handler));
            }
        });
    };
    auto asyncReadSomeConnection = [&conn](asio::mutable_buffer buffer) {
        return asyncResult<std::size_t>([&conn, buffer](auto handler) mutable {
            if (conn.tlsStream) {
                conn.tlsStream->async_read_some(buffer, std::move(handler));
            } else {
                conn.rawSocket.async_read_some(buffer, std::move(handler));
            }
        });
    };
    auto asyncReadConnection = [&conn](asio::mutable_buffer buffer) {
        return asyncResult<std::size_t>([&conn, buffer](auto handler) mutable {
            if (conn.tlsStream) {
                asio::async_read(*conn.tlsStream, buffer, std::move(handler));
            } else {
                asio::async_read(conn.rawSocket, buffer, std::move(handler));
            }
        });
    };

    // Build request
    const auto method = options.method.empty() ? std::string_view("GET") : options.method;
    const auto effectivePath = path.empty() ? std::string_view("/") : path;
    validateHttpClientRequestHead(method, effectivePath);

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

    std::size_t requestHeadBytes = 0;
    addHttpClientRequestHeadBytes(requestHeadBytes, method.size());
    addHttpClientRequestHeadBytes(requestHeadBytes, 1);
    addHttpClientRequestHeadBytes(requestHeadBytes, effectivePath.size());
    addHttpClientRequestHeadBytes(requestHeadBytes, std::string_view(" HTTP/1.1\r\nHost: ").size());
    addHttpClientRequestHeadBytes(requestHeadBytes, hostHeader_.size());
    addHttpClientRequestHeadBytes(requestHeadBytes, std::string_view("\r\nConnection: keep-alive\r\n").size());
    for (const auto& hdr : options.headers) {
        validateHttpClientRequestHeader(hdr);
        addHttpClientRequestHeadBytes(requestHeadBytes, hdr.name.size());
        addHttpClientRequestHeadBytes(requestHeadBytes, 2);
        addHttpClientRequestHeadBytes(requestHeadBytes, hdr.value.size());
        addHttpClientRequestHeadBytes(requestHeadBytes, 2);
    }
    if (!contentLengthValue.empty()) {
        addHttpClientRequestHeadBytes(requestHeadBytes, std::string_view("Content-Length: ").size());
        addHttpClientRequestHeadBytes(requestHeadBytes, contentLengthValue.size());
        addHttpClientRequestHeadBytes(requestHeadBytes, 2);
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
        requestBuf.append(hdr.name.data(), hdr.name.size());
        requestBuf.append(": ");
        requestBuf.append(hdr.value.data(), hdr.value.size());
        requestBuf.append("\r\n");
    }

    if (!contentLengthValue.empty()) {
        requestBuf.append("Content-Length: ");
        requestBuf.append(contentLengthValue.data(), contentLengthValue.size());
        requestBuf.append("\r\n");
    }
    requestBuf.append("\r\n");

    // Write request
    const std::array<asio::const_buffer, 2> requestBuffers{
        asio::buffer(requestBuf.data(), requestBuf.size()),
        asio::buffer(options.body.data(), options.body.size())};
    setDeadline(conn, requestTimeout, Connection::DeadlineKind::kSocket);
    const auto writeEc = co_await asyncWriteConnection(requestBuffers);
    if (finishDeadline(conn)) {
        throw std::system_error(asio::error::timed_out, "http client: write timed out");
    }
    if (writeEc) {
        conn.connected = false;
        throw std::system_error(writeEc, "http client: write failed");
    }

    // Read response headers
    auto& readBuf = conn.responseReadBuffer;
    readBuf.clear();
    readBuf.reserve(4096);

    auto headerEnd = std::pmr::string::npos;
    while (headerEnd == std::pmr::string::npos) {
        if (readBuf.size() >= kMaxHttpHeaderBytes) {
            closeConnection(conn);
            throw std::runtime_error("http client: response headers too large");
        }

        const auto oldSize = readBuf.size();
        const auto writable = std::min<std::size_t>(4096, kMaxHttpHeaderBytes - oldSize);
        resizePmrStringForOverwrite(readBuf, oldSize + writable);
        setDeadline(conn, requestTimeout, Connection::DeadlineKind::kSocket);
        auto [readEc, n] = co_await asyncReadSomeConnection(asio::buffer(readBuf.data() + oldSize, writable));
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

    FetchResponse response(requestResource);
    auto responseHead = parseHttpClientResponseHead(
        method,
        std::string_view(readBuf.data(), headerEnd),
        response,
        requestResource);

    // Collect body
    if (responseHead.responseMayHaveBody && responseHead.hasTransferEncoding) {
        closeConnection(conn);
        throw std::runtime_error("http client: unsupported response Transfer-Encoding");
    }
    if (responseHead.responseMayHaveBody && !responseHead.hasContentLength) {
        closeConnection(conn);
        throw std::runtime_error("http client: unsupported response body framing");
    }

    if (responseHead.responseMayHaveBody &&
        responseHead.hasContentLength &&
        config_.maxResponseBodyBytes != 0 &&
        responseHead.contentLength > config_.maxResponseBodyBytes) {
        closeConnection(conn);
        throw std::runtime_error("http client: response body is too large");
    }

    if (responseHead.contentLength > 0 && responseHead.responseMayHaveBody) {
        resizePmrStringForOverwrite(response.body, responseHead.contentLength);
        const auto alreadyRead = readBuf.size() > responseHead.bodyOffset
            ? readBuf.size() - responseHead.bodyOffset
            : std::size_t{0};
        const auto toCopy = std::min(alreadyRead, responseHead.contentLength);
        if (alreadyRead > responseHead.contentLength) {
            responseHead.closeAfterResponse = true;
        }
        if (toCopy > 0) {
            std::copy_n(readBuf.data() + responseHead.bodyOffset, toCopy, response.body.data());
        }
        if (toCopy < responseHead.contentLength) {
            setDeadline(conn, requestTimeout, Connection::DeadlineKind::kSocket);
            auto [bodyEc, bodyN] = co_await asyncReadConnection(asio::buffer(
                response.body.data() + toCopy,
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

    if (responseHead.closeAfterResponse) {
        closeConnection(conn);
    }

    co_return response;
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
