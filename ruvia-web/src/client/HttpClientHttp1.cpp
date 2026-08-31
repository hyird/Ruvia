#include "ruvia/web/detail/client/HttpClientRegistry.h"

#include <array>
#include <exception>

#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/coding/HttpTransferCodingDecoder.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"
#include "client/HttpClientResponseState.h"

namespace ruvia::detail {
Task<void> HttpClientPool::executeHttp1(Connection& connection,
    const HttpClientRequestStorage& request, const OperationTimeout& timeout,
    HttpClientResponse& response) {
    auto* responseResource = response.state_->resource;
    std::pmr::vector<HttpHeaderView> headers(resource_);
    auto source = HttpClientRequestStorageAccess::view(request, headers);
    std::pmr::string cookieHeader(resource_);
    appendAutomaticHeaders(request, headers, cookieHeader);
    source.headers = std::span<const HttpHeaderView>(headers);

    connection.writeBuffer.resize(kMaxHttpHeaderBytes + 1024);
    const auto wireHost = httpClientWireHost(config_, resource_);
    const auto origin =
        config_.scheme == HttpScheme::kHttps
            ? HttpOriginView::https({.host = wireHost, .port = httpClientPort(config_)})
            : HttpOriginView::http({.host = wireHost, .port = httpClientPort(config_)});
    auto preparedResult =
        Http1ClientRequestWriter({.resource = responseResource})
            .prepare(origin, source,
                std::span<char>(connection.writeBuffer.data(), connection.writeBuffer.size()));
    const auto* prepared = preparedResult.prepared();
    if (!prepared) {
        throw HttpClientError(HttpClientError::Code::kInvalidRequest,
            preparedResult.failure() ? std::string(http1ClientRequestPrepareErrorMessage(
                                           preparedResult.failure()->error()))
                                     : "HTTP request head is too large");
    }
    Http1ClientResponseParser parser(prepared->exchangeState(), {.resource = responseResource});
    co_await write(connection, prepared->head(), timeout);
    if (const auto* content = prepared->contentPlan().immediate()) {
        co_await write(connection, content->bytes(), timeout);
        if (!content->bytes().empty() &&
            parser.completeRequestContent() !=
                Http1ClientRequestContentCompletionStatus::kCompleted) {
            std::terminate();
        }
    }

    std::array<char, 16384> input{};
    for (;;) {
        auto parseResult = parser.parse(connection.readBuffer);
        if (parseResult.failure()) {
            throw HttpClientError(HttpClientError::Code::kProtocolError,
                std::string(http1ClientResponseParseErrorMessage(parseResult.failure()->error())));
        }
        if (parseResult.needMore()) {
            if (connection.readBuffer.size() >= kMaxHttpHeaderBytes) {
                throw HttpClientError(
                    HttpClientError::Code::kProtocolError, "HTTP response head is too large");
            }
            const auto bytes = co_await readSome(connection, input, timeout);
            if (bytes == 0) {
                throw HttpClientError(HttpClientError::Code::kIoError,
                    "upstream closed before the HTTP response head");
            }
            connection.readBuffer.append(input.data(), bytes);
            continue;
        }
        auto* parsed = parseResult.parsed();
        if (!parsed) {
            std::terminate();
        }
        const auto consumedHead = parsed->consumedBytes();
        if (parsed->plan().informational()) {
            const bool closesExchange = parsed->plan().informational()->persistence() ==
                                        Http1ClosePolicy::kCloseAfterResponse;
            connection.readBuffer.erase(0, consumedHead);
            if (closesExchange) {
                close(connection);
                throw HttpClientError(HttpClientError::Code::kProtocolError,
                    "upstream closed the HTTP exchange after an informational response");
            }
            continue;
        }
        response.state_->status = parsed->head().status();
        response.state_->protocolVersion = parsed->head().protocolVersion();
        response.state_->headers.reserve(parsed->head().headers().size());
        for (const auto& header : parsed->head().headers()) {
            response.state_->headers.push_back(HttpClientResponseHeaderAccess::make(
                header.name(), header.value(), responseResource));
        }
        connection.readBuffer.erase(0, consumedHead);
        if (parsed->plan().connectTunnel() != nullptr ||
            parsed->plan().protocolUpgrade() != nullptr) {
            throw HttpClientError(HttpClientError::Code::kProtocolError,
                "HTTP tunnel and protocol upgrade responses require a dedicated API");
        }
        response.state_->headReady = true;
        response.state_->headSignal.notify();

        const auto appendChecked = [&](std::string_view bytes) {
            const auto retained = response.state_->buffered.size() - response.state_->offset +
                                  response.state_->pending.size();
            if (response.state_->collectAll &&
                bytes.size() >
                    config_.maxResponseBytes - std::min(retained, config_.maxResponseBytes)) {
                throw HttpClientError(HttpClientError::Code::kResponseTooLarge,
                    "HTTP response exceeds configured byte limit");
            }
            response.state_->pending.append(bytes);
            response.state_->dataSignal.notify();
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
                throw HttpClientError(
                    HttpClientError::Code::kProtocolError, "invalid HTTP response transfer coding");
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
                if (decoded.needInput() || decoded.complete()) {
                    return;
                }
                throw HttpClientError(HttpClientError::Code::kProtocolError,
                    "invalid HTTP response transfer-coding state");
            }
        };
        const auto finishTransferDecoder = [&] {
            if (!transferDecoder) {
                return;
            }
            const auto finished = transferDecoder->finishInput();
            if (finished.complete()) {
                return;
            }
            throwTransferFailure(finished);
            throw HttpClientError(
                HttpClientError::Code::kProtocolError, "incomplete HTTP response transfer coding");
        };
        const auto retainTrailers = [&](std::string_view trailerBlock) {
            const auto ok = visitHttpResponseTrailerFields(
                trailerBlock, [&](std::string_view name, std::string_view value) {
                    response.state_->trailers.push_back(HttpClientResponseHeaderAccess::make(
                        name, value, responseResource));
                    return true;
                });
            if (!ok) {
                throw HttpClientError(HttpClientError::Code::kProtocolError,
                    "invalid chunked HTTP response trailers");
            }
        };
        const auto waitForBufferSpace = [&]() -> Task<void> {
            while (!response.state_->collectAll &&
                   response.state_->pending.size() >= config_.maxResponseBytes) {
                co_await response.state_->spaceSignal.wait();
                if (response.state_->abandoned) {
                    throw HttpClientError(
                        HttpClientError::Code::kCancelled, "HTTP response body was abandoned");
                }
            }
        };
        const auto readMore = [&]() -> Task<void> {
            co_await waitForBufferSpace();
            const auto bytes = co_await readSome(connection, input, timeout);
            if (bytes == 0) {
                throw HttpClientError(HttpClientError::Code::kIoError,
                    "upstream closed before the HTTP response completed");
            }
            connection.readBuffer.append(input.data(), bytes);
        };
        bool closeAfter = false;
        bool contentSemanticsPresent = false;

        auto chunkedPlan = parsed->plan().chunked();
        auto closeDelimitedPlan = parsed->plan().closeDelimited();
        bool framingHandled = false;
        bool requireEmptyContent = false;
        if (const auto* known = parsed->plan().knownLength()) {
            contentSemanticsPresent = true;
            auto remaining = known->contentLength();
            while (remaining != 0) {
                if (connection.readBuffer.empty()) {
                    co_await readMore();
                }
                const auto count = std::min(remaining, connection.readBuffer.size());
                appendChecked(std::string_view(connection.readBuffer).substr(0, count));
                connection.readBuffer.erase(0, count);
                remaining -= count;
            }
            closeAfter = known->persistence() == Http1ClosePolicy::kCloseAfterResponse;
            framingHandled = true;
        } else if (const auto* zero = parsed->plan().zeroContent()) {
            contentSemanticsPresent = true;
            requireEmptyContent = true;
            if (const auto* zeroKnown = zero->knownLength()) {
                while (connection.readBuffer.size() < zeroKnown->contentLength()) {
                    co_await readMore();
                }
                if (zeroKnown->contentLength() != 0) {
                    throw HttpClientError(HttpClientError::Code::kProtocolError,
                        "HTTP 205 response content is not empty");
                }
                closeAfter = zeroKnown->persistence() == Http1ClosePolicy::kCloseAfterResponse;
                framingHandled = true;
            } else if (zero->chunked()) {
                chunkedPlan = zero->chunked();
            } else if (zero->closeDelimited()) {
                closeDelimitedPlan = zero->closeDelimited();
            } else {
                throw HttpClientError(
                    HttpClientError::Code::kProtocolError, "invalid HTTP 205 response framing");
            }
        }

        if (!framingHandled && chunkedPlan != nullptr) {
            contentSemanticsPresent = true;
            configureTransferDecoder(chunkedPlan->transferCodings());
            Http1ChunkedBodyDecoder decoder(
                transferDecoder ? ProtocolByteLimit::unlimited()
                                : ProtocolByteLimit::limited(config_.maxResponseBytes),
                Http1ChunkTrailerRole::kResponse);
            for (;;) {
                auto decoded = decoder.decode(connection.readBuffer);
                if (const auto* body = decoded.bodyChunk()) {
                    appendTransferDecoded(body->bytes());
                }
                if (const auto* complete = decoded.complete()) {
                    retainTrailers(complete->trailers());
                }
                connection.readBuffer.erase(0, decoded.consumedBytes());
                if (decoded.failure()) {
                    throw HttpClientError(
                        HttpClientError::Code::kProtocolError, "invalid chunked HTTP response");
                }
                if (decoded.complete()) {
                    break;
                }
                if (decoded.needMore() || connection.readBuffer.empty()) {
                    co_await readMore();
                }
            }
            finishTransferDecoder();
            closeAfter = chunkedPlan->persistence() == Http1ClosePolicy::kCloseAfterResponse;
            framingHandled = true;
        } else if (!framingHandled && closeDelimitedPlan != nullptr) {
            contentSemanticsPresent = true;
            configureTransferDecoder(closeDelimitedPlan->transferCodings());
            appendTransferDecoded(connection.readBuffer);
            connection.readBuffer.clear();
            for (;;) {
                co_await waitForBufferSpace();
                const auto bytes = co_await readSome(connection, input, timeout, true);
                if (bytes == 0) {
                    break;
                }
                appendTransferDecoded(std::string_view(input.data(), bytes));
            }
            finishTransferDecoder();
            closeAfter = true;
            framingHandled = true;
        }

        if (!framingHandled) {
            if (const auto* without = parsed->plan().withoutContent()) {
                closeAfter = without->persistence() == Http1ClosePolicy::kCloseAfterResponse;
            } else {
                throw HttpClientError(HttpClientError::Code::kProtocolError,
                    "HTTP tunnel and protocol upgrade responses require a dedicated API");
            }
        }
        if (requireEmptyContent &&
            (!response.state_->buffered.empty() || !response.state_->pending.empty())) {
            throw HttpClientError(
                HttpClientError::Code::kProtocolError, "HTTP 205 response content is not empty");
        }
        if (!connection.readBuffer.empty()) {
            throw HttpClientError(
                HttpClientError::Code::kProtocolError, "unexpected bytes after HTTP response");
        }
        decodeResponseContentEncoding(
            response, contentSemanticsPresent, config_.maxResponseBytes, responseResource);
        if (closeAfter) {
            close(connection);
        }
        co_return;
    }
}

}  // namespace ruvia::detail
