#include "ruvia/web/detail/client/HttpClientRegistry.h"

#include <array>

#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/coding/HttpTransferCodingDecoder.h"
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
            const bool closesExchange = parsed->plan().informational()->persistence() == Http1ClosePolicy::kCloseAfterResponse;
            connection.readBuffer.erase(0, consumedHead);
            if (closesExchange) {
                close(connection);
                throw HttpClientError(HttpClientError::Code::kProtocolError,
                    "upstream closed the HTTP exchange after an informational response");
            }
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
        std::optional<TransferCodingDecoder> transferDecoder;
        std::array<char, kBodyReadChunkBytes> transferOutput{};
        const auto configureTransferDecoder = [&](HttpTransferCodings codings) {
            if (codings.count != 0) {
                transferDecoder.emplace(codings.values[0], responseResource,
                    ProtocolByteLimit::limited(config_.maxResponseBytes));
            }
        };
        const auto throwTransferFailure = [](const TransferCodingDecodeResult& result) -> void {
            if (const auto* failure = result.protocolFailure()) {
                if (failure->protocolError().status() == http_status::kContentTooLarge) {
                    throw HttpClientError(HttpClientError::Code::kResponseTooLarge,
                        "HTTP response exceeds configured byte limit");
                }
                throw HttpClientError(HttpClientError::Code::kProtocolError,
                    "invalid HTTP response transfer coding");
            }
            if (result.decoderFailure()) {
                throw HttpClientError(HttpClientError::Code::kProtocolError,
                    "HTTP response transfer-coding decoder failed");
            }
        };
        const auto appendTransferDecoded = [&](std::string_view encodedBytes) {
            if (!transferDecoder) {
                appendChecked(encodedBytes);
                return;
            }
            for (;;) {
                const auto decoded = transferDecoder->decode(encodedBytes, transferOutput);
                encodedBytes.remove_prefix(std::min(encodedBytes.size(), decoded.consumedBytes()));
                if (const auto* output = decoded.output()) {
                    appendChecked(output->bytes());
                    continue;
                }
                throwTransferFailure(decoded);
                if (decoded.needInput() || decoded.complete()) return;
                throw HttpClientError(HttpClientError::Code::kProtocolError,
                    "invalid HTTP response transfer-coding state");
            }
        };
        const auto finishTransferDecoder = [&] {
            if (!transferDecoder) return;
            const auto finished = transferDecoder->finishInput();
            if (finished.complete()) return;
            throwTransferFailure(finished);
            throw HttpClientError(HttpClientError::Code::kProtocolError,
                "incomplete HTTP response transfer coding");
        };
        const auto retainTrailers = [&](std::string_view trailerBlock) {
            HttpChunkTrailerParser trailerParser(trailerBlock);
            for (;;) {
                const auto trailer = trailerParser.next();
                if (const auto* field = trailer.field()) {
                    response.trailers_.push_back(HttpClientHeaderAccess::make(
                        field->name(), field->value(), responseResource));
                    continue;
                }
                if (trailer.end()) return;
                throw HttpClientError(
                    HttpClientError::Code::kProtocolError,
                    "invalid chunked HTTP response trailers");
            }
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
            configureTransferDecoder(chunked->transferCodings());
            Http1ChunkedBodyDecoder decoder(transferDecoder
                    ? ProtocolByteLimit::unlimited()
                    : ProtocolByteLimit::limited(config_.maxResponseBytes));
            for (;;) {
                auto decoded = decoder.decode(connection.readBuffer);
                if (const auto* body = decoded.bodyChunk()) appendTransferDecoded(body->bytes());
                if (const auto* complete = decoded.complete()) retainTrailers(complete->trailers());
                connection.readBuffer.erase(0, decoded.consumedBytes());
                if (decoded.failure()) throw HttpClientError(HttpClientError::Code::kProtocolError, "invalid chunked HTTP response");
                if (decoded.complete()) break;
                if (decoded.needMore() || connection.readBuffer.empty()) co_await readMore();
            }
            finishTransferDecoder();
            closeAfter = chunked->persistence() == Http1ClosePolicy::kCloseAfterResponse;
        } else if (const auto* closeDelimited = parsed->plan().closeDelimited()) {
            configureTransferDecoder(closeDelimited->transferCodings());
            appendTransferDecoded(connection.readBuffer);
            connection.readBuffer.clear();
            for (;;) {
                const auto bytes = co_await readSome(connection, input, timeout, true);
                if (bytes == 0) break;
                appendTransferDecoded(std::string_view(input.data(), bytes));
            }
            finishTransferDecoder();
            closeAfter = true;
        } else if (const auto* zero = parsed->plan().zeroContent()) {
            if (const auto* zeroKnown = zero->knownLength()) {
                while (connection.readBuffer.size() < zeroKnown->contentLength()) co_await readMore();
                if (zeroKnown->contentLength() != 0) throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP 205 response content is not empty");
                closeAfter = zeroKnown->persistence() == Http1ClosePolicy::kCloseAfterResponse;
            } else if (const auto* zeroChunked = zero->chunked()) {
                configureTransferDecoder(zeroChunked->transferCodings());
                Http1ChunkedBodyDecoder decoder(transferDecoder
                        ? ProtocolByteLimit::unlimited()
                        : ProtocolByteLimit::limited(config_.maxResponseBytes));
                for (;;) {
                    auto decoded = decoder.decode(connection.readBuffer);
                    if (const auto* body = decoded.bodyChunk()) appendTransferDecoded(body->bytes());
                    if (const auto* complete = decoded.complete()) retainTrailers(complete->trailers());
                    connection.readBuffer.erase(0, decoded.consumedBytes());
                    if (decoded.failure()) throw HttpClientError(HttpClientError::Code::kProtocolError, "invalid chunked HTTP response");
                    if (decoded.complete()) break;
                    if (decoded.needMore() || connection.readBuffer.empty()) co_await readMore();
                }
                finishTransferDecoder();
                if (!response.body_.empty()) throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP 205 response content is not empty");
                closeAfter = zeroChunked->persistence() == Http1ClosePolicy::kCloseAfterResponse;
            } else if (const auto* zeroCloseDelimited = zero->closeDelimited()) {
                configureTransferDecoder(zeroCloseDelimited->transferCodings());
                appendTransferDecoded(connection.readBuffer);
                connection.readBuffer.clear();
                for (;;) {
                    const auto bytes = co_await readSome(connection, input, timeout, true);
                    if (bytes == 0) break;
                    appendTransferDecoded(std::string_view(input.data(), bytes));
                }
                finishTransferDecoder();
                if (!response.body_.empty()) throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP 205 response content is not empty");
                closeAfter = true;
            } else {
                throw HttpClientError(HttpClientError::Code::kProtocolError, "invalid HTTP 205 response framing");
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
