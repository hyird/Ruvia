#include "ruvia/web/detail/client/HttpClientRegistry.h"

#include <array>

#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"

namespace ruvia::detail {
Task<HttpClientResponse> HttpClientPool::executeHttp1(Connection& connection, const HttpClientRequest& request, const OperationTimeout& timeout, std::pmr::memory_resource* responseResource) {
    std::pmr::vector<HttpHeaderView> headers(resource_);
    auto source = HttpClientRequestAccess::view(request, headers);
    std::pmr::string cookieHeader(resource_);
    appendAutomaticHeaders(request, headers, cookieHeader);
    source.headers = std::span<const HttpHeaderView>(headers);

    connection.writeBuffer.resize(kMaxHttpHeaderBytes + 1024);
    const auto wireHost = httpClientWireHost(config_, resource_);
    const auto origin = config_.scheme == HttpScheme::kHttps
        ? HttpOriginView::https(wireHost, httpClientPort(config_))
        : HttpOriginView::http(wireHost, httpClientPort(config_));
    auto preparedResult = Http1ClientRequestWriter().prepare(origin, source, std::span<char>(connection.writeBuffer.data(), connection.writeBuffer.size()));
    const auto* prepared = preparedResult.prepared();
    if (!prepared) {
        throw HttpClientError(HttpClientError::Code::kInvalidRequest,
            preparedResult.failure() ? std::string(http1ClientRequestPrepareErrorMessage(preparedResult.failure()->error())) : "HTTP request head is too large");
    }
    co_await write(connection, prepared->head(), timeout);
    if (const auto* content = prepared->contentPlan().immediate()) {
        co_await write(connection, content->bytes(), timeout);
    }

    Http1ClientResponseParser parser(*prepared, responseResource);
    HttpClientResponse response(responseResource);
    std::array<char, 16384> input{};
    for (;;) {
        auto parseResult = parser.parse(connection.readBuffer);
        if (parseResult.failure()) {
            throw HttpClientError(HttpClientError::Code::kProtocolError, std::string(http1ClientResponseParseErrorMessage(parseResult.failure()->error())));
        }
        if (parseResult.needMore()) {
            if (connection.readBuffer.size() >= kMaxHttpHeaderBytes) throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP response head is too large");
            const auto bytes = co_await readSome(connection, input, timeout);
            if (bytes == 0) throw HttpClientError(HttpClientError::Code::kIoError, "upstream closed before the HTTP response head");
            connection.readBuffer.append(input.data(), bytes);
            continue;
        }
        auto* parsed = parseResult.parsed();
        if (!parsed) std::terminate();
        const auto consumedHead = parsed->consumedBytes();
        if (parsed->plan().informational()) {
            connection.readBuffer.erase(0, consumedHead);
            continue;
        }
        response.status_ = parsed->head().status();
        response.protocolVersion_ = parsed->head().protocolVersion();
        response.headers_.reserve(parsed->head().headers().size());
        for (const auto& header : parsed->head().headers()) response.headers_.push_back(HttpClientHeaderAccess::make(header.name(), header.value(), responseResource));
        connection.readBuffer.erase(0, consumedHead);

        const auto appendChecked = [&](std::string_view bytes) {
            if (bytes.size() > config_.maxResponseBytes - std::min(response.body_.size(), config_.maxResponseBytes)) {
                throw HttpClientError(HttpClientError::Code::kResponseTooLarge, "HTTP response exceeds configured byte limit");
            }
            response.body_.append(bytes);
        };
        const auto readMore = [&]() -> Task<void> {
            const auto bytes = co_await readSome(connection, input, timeout);
            if (bytes == 0) throw HttpClientError(HttpClientError::Code::kIoError, "upstream closed before the HTTP response completed");
            connection.readBuffer.append(input.data(), bytes);
        };
        bool closeAfter = false;

        if (const auto* known = parsed->plan().knownLength()) {
            if (known->contentLength() > config_.maxResponseBytes) {
                throw HttpClientError(HttpClientError::Code::kResponseTooLarge, "HTTP response exceeds configured byte limit");
            }
            while (connection.readBuffer.size() < known->contentLength()) co_await readMore();
            appendChecked(std::string_view(connection.readBuffer).substr(0, known->contentLength()));
            connection.readBuffer.erase(0, known->contentLength());
            closeAfter = known->persistence() == Http1ClosePolicy::kCloseAfterResponse;
        } else if (const auto* chunked = parsed->plan().chunked()) {
            if (chunked->transferCodings().count != 0) throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP transfer coding before chunked is unsupported");
            Http1ChunkedBodyDecoder decoder(ProtocolByteLimit::limited(config_.maxResponseBytes));
            for (;;) {
                auto decoded = decoder.decode(connection.readBuffer);
                if (const auto* body = decoded.bodyChunk()) appendChecked(body->bytes());
                connection.readBuffer.erase(0, decoded.consumedBytes());
                if (decoded.failure()) throw HttpClientError(HttpClientError::Code::kProtocolError, "invalid chunked HTTP response");
                if (decoded.complete()) break;
                if (decoded.needMore() || connection.readBuffer.empty()) co_await readMore();
            }
            closeAfter = chunked->persistence() == Http1ClosePolicy::kCloseAfterResponse;
        } else if (const auto* closeDelimited = parsed->plan().closeDelimited()) {
            if (closeDelimited->transferCodings().count != 0) throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP transfer coding is unsupported");
            appendChecked(connection.readBuffer);
            connection.readBuffer.clear();
            for (;;) {
                const auto bytes = co_await readSome(connection, input, timeout, true);
                if (bytes == 0) break;
                appendChecked(std::string_view(input.data(), bytes));
            }
            closeAfter = true;
        } else if (const auto* zero = parsed->plan().zeroContent()) {
            if (const auto* zeroKnown = zero->knownLength()) {
                while (connection.readBuffer.size() < zeroKnown->contentLength()) co_await readMore();
                if (zeroKnown->contentLength() != 0) throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP 205 response content is not empty");
                closeAfter = zeroKnown->persistence() == Http1ClosePolicy::kCloseAfterResponse;
            } else if (const auto* zeroChunked = zero->chunked()) {
                if (zeroChunked->transferCodings().count != 0) throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP transfer coding before chunked is unsupported");
                Http1ChunkedBodyDecoder decoder(ProtocolByteLimit::limited(config_.maxResponseBytes));
                for (;;) {
                    auto decoded = decoder.decode(connection.readBuffer);
                    if (decoded.bodyChunk() && !decoded.bodyChunk()->bytes().empty()) throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP 205 response content is not empty");
                    connection.readBuffer.erase(0, decoded.consumedBytes());
                    if (decoded.failure()) throw HttpClientError(HttpClientError::Code::kProtocolError, "invalid chunked HTTP response");
                    if (decoded.complete()) break;
                    if (decoded.needMore() || connection.readBuffer.empty()) co_await readMore();
                }
                closeAfter = zeroChunked->persistence() == Http1ClosePolicy::kCloseAfterResponse;
            } else {
                throw HttpClientError(HttpClientError::Code::kProtocolError, "close-delimited HTTP 205 response is unsupported");
            }
        } else if (const auto* without = parsed->plan().withoutContent()) {
            closeAfter = without->persistence() == Http1ClosePolicy::kCloseAfterResponse;
        } else {
            throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP tunnel and protocol upgrade responses require a dedicated API");
        }
        if (!connection.readBuffer.empty()) throw HttpClientError(HttpClientError::Code::kProtocolError, "unexpected bytes after HTTP response");
        if (closeAfter) close(connection);
        co_return response;
    }
}

}  // namespace ruvia::detail
