#include <algorithm>
#include <array>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseBodyDecoder.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpResponseBodyDecoding.h"
#include "ruvia/web/detail/client/ClientTransport.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"
#include "ruvia/web/detail/client/HttpClientPool.h"
#include "ruvia/web/detail/client/HttpClientResponseDecoding.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"

namespace ruvia::detail {
Task<void> HttpClientPool::executeHttp1(Connection& connection,
    const HttpClientRequestStorage& request, const ruvia::OperationTimeout& timeout,
    HttpClientResponse& response) {
    response.state_->transport = HttpClientResponseTransport::kHttp1;
    auto* responseResource = response.state_->resource;
    std::pmr::vector<HttpHeaderView> headers(resource_);
    auto source = HttpClientRequestStorageAccess::view(request, headers);
    std::pmr::string cookieHeader(resource_);
    appendAutomaticHeaders(request, headers, cookieHeader);
    source.headers = std::span<const HttpHeaderView>(headers);

    connection.writeBuffer.resize(kMaxHttpHeaderBytes + 1024);
    const auto wireHost = clientUriHost(config_.host, resource_);
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
        response.state_->requestMethod = classifyHttpMethod(request.method());
        response.state_->responseBodyPlan =
            planHttpResponseBody(response.state_->requestMethod, response.state_->status);
        response.state_->headers.reserve(parsed->head().headers().size());
        for (const auto& header : parsed->head().headers()) {
            response.state_->headers.push_back(
                HttpHeader::copyOf(header.name(), header.value(), responseResource));
        }
        connection.readBuffer.erase(0, consumedHead);
        if (parsed->plan().connectTunnel() != nullptr ||
            parsed->plan().protocolUpgrade() != nullptr) {
            throw HttpClientError(HttpClientError::Code::kProtocolError,
                "HTTP tunnel and protocol upgrade responses require a dedicated API");
        }
        Http1ClientResponseBodyDecoder bodyDecoder(parsed->plan(), responseResource);
        const bool contentSemanticsPresent = parsed->plan().withoutContent() == nullptr;
        configureHttpClientResponseDecoding(*response.state_);
        response.state_->headReady = true;
        response.state_->headSignal.notify();

        const auto appendOutput = [&](std::string_view bytes) {
            const auto retained = response.state_->buffered.size() - response.state_->offset +
                                  response.state_->pending.size();
            if (response.state_->collectAll &&
                bytes.size() >
                    config_.maxResponseBytes - std::min(retained, config_.maxResponseBytes)) {
                throw HttpClientError(HttpClientError::Code::kResponseTooLarge,
                    "HTTP response exceeds configured byte limit");
            }
            // The consumer's last borrowed view lives in buffered, not pending.
            // Appending producer output must never relocate that view.
            response.state_->pending.append(bytes);
            response.state_->dataSignal.notify();
        };
        const auto retainTrailers = [&](std::string_view trailerBlock) {
            const auto ok = visitHttpResponseTrailers(
                trailerBlock, [&](std::string_view name, std::string_view value) {
                    response.state_->trailers.push_back(
                        HttpHeader::copyOf(name, value, responseResource));
                    return true;
                });
            if (!ok) {
                throw std::logic_error("HTTP decoder published invalid response trailers");
            }
        };
        const auto waitForBufferSpace = [&]() -> Task<void> {
            while (!response.state_->collectAll &&
                   response.state_->pending.size() >= config_.maxResponseBytes) {
                throwAbort(connection);
                if (!armDeadline(connection, timeout, DeadlineKind::kResponseBuffer)) {
                    throw HttpClientError(
                        HttpClientError::Code::kTimeout, "HTTP/1 response body decoding timed out");
                }
                try {
                    co_await response.state_->spaceSignal.wait();
                } catch (...) {
                    (void)clearDeadline(connection);
                    throw;
                }
                const bool timedOut = clearDeadline(connection) || timeout.expired();
                throwAbort(connection);
                if (timedOut) {
                    throw HttpClientError(
                        HttpClientError::Code::kTimeout, "HTTP/1 response body decoding timed out");
                }
                if (response.state_->abandoned) {
                    throw HttpClientError(
                        HttpClientError::Code::kCancelled, "HTTP response body was abandoned");
                }
            }
        };

        std::array<char, kBodyReadChunkBytes> output{};
        bool eof = false;
        Http1ClosePolicy persistence = Http1ClosePolicy::kCloseAfterResponse;
        for (;;) {
            // Backpressure applies to decoding buffered compressed input too,
            // not just to the next transport read. One step cannot overfill the
            // producer queue; readAll retains its separate total byte policy.
            co_await waitForBufferSpace();
            throwAbort(connection);
            const auto capacity = response.state_->collectAll
                                      ? output.size()
                                      : std::min(output.size(),
                                            config_.maxResponseBytes - response.state_->pending.size());
            const auto scratch = std::span<char>(output).first(capacity);
            const auto decoded = eof
                                     ? bodyDecoder.finishInput(connection.readBuffer, scratch)
                                     : bodyDecoder.decode(connection.readBuffer, scratch);
            const auto consumed = decoded.consumedBytes();
            if (const auto* body = decoded.output()) {
                appendOutput(body->bytes());
            } else if (const auto* trailers = decoded.trailers()) {
                retainTrailers(trailers->bytes());
            } else if (const auto* failure = decoded.protocolFailure()) {
                throw HttpClientError(HttpClientError::Code::kProtocolError,
                    std::string(http1ClientResponseBodyErrorMessage(failure->error())));
            } else if (decoded.decoderFailure() != nullptr) {
                throw HttpClientError(HttpClientError::Code::kProtocolError,
                    "HTTP/1 response body decoder failed");
            }
            // Output/trailers have been owned before their source is compacted.
            connection.readBuffer.erase(0, consumed);
            if (const auto* complete = decoded.complete()) {
                persistence = complete->persistence();
                break;
            }
            if (decoded.needInput() == nullptr ||
                (consumed != 0 && !connection.readBuffer.empty())) {
                continue;
            }
            if (eof) {
                throw std::logic_error("HTTP body decoder requested input after EOF");
            }
            const auto bytes = co_await readSome(connection, input, timeout, true);
            if (bytes == 0) {
                eof = true;
            } else {
                connection.readBuffer.append(input.data(), bytes);
            }
        }
        if (!connection.readBuffer.empty()) {
            throw HttpClientError(
                HttpClientError::Code::kProtocolError, "unexpected bytes after HTTP response");
        }
        if (timeout.expired()) {
            throw HttpClientError(HttpClientError::Code::kTimeout, "HTTP/1 response body decoding timed out");
        }
        decodeHttpClientResponseContentEncoding(
            *response.state_, contentSemanticsPresent, config_.maxResponseBytes, responseResource);
        if (timeout.expired()) {
            throw HttpClientError(HttpClientError::Code::kTimeout, "HTTP/1 response body decoding timed out");
        }
        if (persistence == Http1ClosePolicy::kCloseAfterResponse) {
            close(connection);
        }
        co_return;
    }
}

}  // namespace ruvia::detail
