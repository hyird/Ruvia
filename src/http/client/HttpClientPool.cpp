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
#include "HttpClientResponseParser.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia::detail {

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

    asio::ip::tcp::resolver resolver(ioContext_);
    std::array<char, 5> portBuffer;
    const auto [portEnd, portEc] = std::to_chars(
        portBuffer.data(),
        portBuffer.data() + portBuffer.size(),
        config_.port);
    if (portEc != std::errc{}) {
        throw std::logic_error("http client: invalid port");
    }
    const auto port = std::string_view(portBuffer.data(), static_cast<std::size_t>(portEnd - portBuffer.data()));
    auto [resolveEc, endpoints] = co_await asyncResult<asio::ip::tcp::resolver::results_type>(
        [this, port, &resolver](auto handler) {
            resolver.async_resolve(
                std::string_view(config_.host),
                port,
                std::move(handler));
        });
    if (resolveEc) {
        throw std::system_error(
            resolveEc,
            "http client: resolve failed");
    }

    auto [connectEc, ep] = co_await asyncResult<asio::ip::tcp::endpoint>(
        [&](auto handler) {
            asio::async_connect(conn.rawSocket, endpoints, std::move(handler));
        });
    (void)ep;
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
        const auto handshakeEc = co_await asyncError([&](auto handler) {
            conn.tlsStream->async_handshake(
                asio::ssl::stream_base::client, std::move(handler));
        });
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

    const auto index = co_await acquire();
    ConnectionGuard guard(*this, index);
    auto& conn = guard.connection();

    if (!conn.connected) {
        co_await connectOne(conn);
    }

    try {
        co_return co_await executeRequest(conn, path, options, resource);
    } catch (...) {
        guard.discard();
        throw;
    }
}

Task<FetchResponse> HttpClientPool::executeRequest(
    Connection& conn,
    std::string_view path,
    const FetchOptions& options,
    std::pmr::memory_resource* resource) {
    auto* const requestResource = resource == nullptr ? resource_ : resource;
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

    std::size_t requestHeadBytes =
        method.size() + 1 + effectivePath.size() +
        std::string_view(" HTTP/1.1\r\nHost: ").size() + config_.host.size() +
        std::string_view("\r\nConnection: keep-alive\r\n").size() +
        2;
    for (const auto& hdr : options.headers) {
        requestHeadBytes += hdr.name.size() + 2 + hdr.value.size() + 2;
    }
    if (!contentLengthValue.empty()) {
        requestHeadBytes += std::string_view("Content-Length: ").size() + contentLengthValue.size() + 2;
    }

    std::pmr::string requestBuf(requestResource);
    requestBuf.reserve(requestHeadBytes);
    requestBuf.append(method.data(), method.size());
    requestBuf.push_back(' ');
    requestBuf.append(effectivePath.data(), effectivePath.size());
    requestBuf.append(" HTTP/1.1\r\nHost: ");
    requestBuf.append(config_.host.data(), config_.host.size());
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
    const auto writeEc = co_await asyncWriteConnection(requestBuffers);
    if (writeEc) {
        conn.connected = false;
        throw std::system_error(writeEc, "http client: write failed");
    }

    // Read response headers
    std::pmr::string readBuf(requestResource);
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
        auto [readEc, n] = co_await asyncReadSomeConnection(asio::buffer(readBuf.data() + oldSize, writable));
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
    if (!responseHead.hasContentLength && responseHead.responseMayHaveBody) {
        responseHead.closeAfterResponse = true;
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
            auto [bodyEc, bodyN] = co_await asyncReadConnection(asio::buffer(
                response.body.data() + toCopy,
                responseHead.contentLength - toCopy));
            (void)bodyN;
            if (bodyEc) {
                conn.connected = false;
                throw std::system_error(bodyEc, "http client: read body failed");
            }
        }
    } else if (!responseHead.hasContentLength && readBuf.size() > responseHead.bodyOffset) {
        responseHead.closeAfterResponse = true;
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
