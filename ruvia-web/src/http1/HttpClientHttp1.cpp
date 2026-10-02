#include <algorithm>
#include <array>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/Async.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/WorkerTimer.h"
#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseBodyDecoder.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/Http1RequestContentWriter.h"
#include "ruvia/http/HttpConnectUdp.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpRequestTarget.h"
#include "ruvia/http/HttpResponseBodyDecoding.h"
#include "ruvia/web/detail/body/HttpBodyBuffer.h"
#include "ruvia/web/detail/client/ClientTransport.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"
#include "ruvia/web/detail/client/HttpClientPool.h"
#include "ruvia/web/detail/client/HttpClientResponseDecoding.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"
#include "ruvia/web/detail/http/HttpSocketTunnelTransport.h"
#include "ruvia/web/detail/http/TlsTunnelOutput.h"

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
    auto origin =
        config_.scheme == HttpScheme::kHttps
            ? HttpOriginView::https({.host = wireHost, .port = httpClientPort(config_)})
            : HttpOriginView::http({.host = wireHost, .port = httpClientPort(config_)});
    if (request.isTunnel() && request.tunnelProtocol() == "connect-udp") {
        const auto authority = parseHttpAuthority(BorrowedText(request.tunnelAuthority()));
        if (!authority) {
            throw HttpClientError(HttpClientError::Code::kInvalidRequest, "invalid CONNECT-UDP authority");
        }
        origin = config_.scheme == HttpScheme::kHttps ? HttpOriginView::https({.host = authority->host, .port = authority->port})
                                                      : HttpOriginView::http({.host = authority->host, .port = authority->port});
    }
    if (request.isTunnel() && !request.tunnelProtocol().empty() && request.tunnelProtocol() != "connect-udp") {
        throw HttpClientError(HttpClientError::Code::kProtocolUnavailable, "Extended CONNECT requires HTTP/2 or HTTP/3");
    }
    auto preparedResult = request.isTunnel() && request.tunnelProtocol() == "connect-udp"
                              ? Http1ClientRequestWriter({.resource = responseResource}).prepareConnectUdp(origin, source.target, headers, std::span<char>(connection.writeBuffer))
                          : request.isTunnel()
                              ? Http1ClientRequestWriter({.resource = responseResource}).prepareConnect(BorrowedText(request.tunnelAuthority()), headers, std::span<char>(connection.writeBuffer))
                          : request.upload() != nullptr
                              ? Http1ClientRequestWriter({.resource = responseResource}).prepareStreaming(origin, {.method = source.method, .target = source.target, .headers = headers, .contentLength = request.upload()->config.contentLength}, std::span<char>(connection.writeBuffer), {.expectation = request.upload()->config.expectation})
                              : Http1ClientRequestWriter({.resource = responseResource}).prepare(origin, source, std::span<char>(connection.writeBuffer));
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

    if (auto* upload = request.upload()) {
        TaskScope writers(worker_, {.resource = resource_});
        std::exception_ptr writerFailure;
        writers.spawn(writeHttp1Upload(connection, *response.state_, timeout,
            Http1RequestContentWriter(*prepared->contentPlan().streaming()), parser, writerFailure));
        std::exception_ptr receiveFailure;
        try {
            co_await executeHttp1Response(connection, request, timeout, response, parser);
        } catch (...) {
            receiveFailure = std::current_exception();
        }
        upload->stop();
        if (receiveFailure || !upload->ended) {
            close(connection);
        }
        co_await writers.join();
        if (writerFailure) {
            std::rethrow_exception(writerFailure);
        }
        if (receiveFailure) {
            std::rethrow_exception(receiveFailure);
        }
        co_return;
    }
    co_await executeHttp1Response(connection, request, timeout, response, parser);
}

Task<void> HttpClientPool::executeHttp1Response(Connection& connection,
    const HttpClientRequestStorage& request, const OperationTimeout& timeout,
    HttpClientResponse& response, Http1ClientResponseParser& parser) {
    auto* responseResource = response.state_->resource;
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
            std::pmr::vector<HttpHeaderView> interimHeaders(responseResource);
            for (const auto& field : parsed->head().headers()) {
                interimHeaders.emplace_back(field.name(), field.value());
            }
            response.state_->retainInformational(parsed->head().status(), interimHeaders);
            if (request.upload() != nullptr && parsed->plan().requestContentSignal() == HttpClientRequestContentSignal::kContinue) {
                request.upload()->contentReleased = true;
                request.upload()->notifyData();
            }
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
        if (request.upload() != nullptr && !request.upload()->ended) {
            request.upload()->stop();
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
        if (request.isTunnel() && (parsed->plan().connectTunnel() != nullptr ||
                                      (request.tunnelProtocol() == "connect-udp" && parsed->plan().protocolUpgrade() != nullptr))) {
            if (request.tunnelProtocol() == "connect-udp") {
                std::pmr::vector<HttpHeaderView> fields(responseResource);
                for (const auto& field : response.state_->headers) {
                    fields.emplace_back(field.name(), field.value());
                }
                if (!validateHttpConnectUdpResponse(response.state_->protocolVersion, response.state_->status.value(), fields)) {
                    throw HttpClientError(HttpClientError::Code::kProtocolError, "invalid CONNECT-UDP response head");
                }
            }
            response.state_->tunnel->accepted = true;
            response.state_->headReady = true;
            response.state_->headSignal.notify();
            co_await executeHttp1Tunnel(connection, *response.state_, timeout);
            co_return;
        }
        if (request.tunnel() != nullptr) {
            request.tunnel()->stop();
        }
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

        std::array<char, kHttpBodyBufferBytes> output{};
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
            *response.state_, contentSemanticsPresent, config_.maxResponseBytes);
        if (timeout.expired()) {
            throw HttpClientError(HttpClientError::Code::kTimeout, "HTTP/1 response body decoding timed out");
        }
        if (persistence == Http1ClosePolicy::kCloseAfterResponse) {
            close(connection);
        }
        co_return;
    }
}

Task<void> HttpClientPool::executeHttp1Tunnel(Connection& connection, HttpClientResponseState& state, const OperationTimeout& timeout) {
    auto& output = *state.tunnel;
    std::optional<TlsTunnelOutput> tls;
    if (config_.scheme == HttpScheme::kHttps) {
        tls.emplace(*connection.stream.native_handle(), connection.stream.next_layer(), worker_, *resource_);
        tls->start();
    }
    WorkerTimerRegistration lifetimeTimer;
    if (const auto remaining = timeout.remaining()) {
        WorkerHandleAccess::scheduleTimer(worker_, lifetimeTimer, workerTimerDeadlineAfter(*remaining), [this, &connection](WorkerTimerOutcome outcome) noexcept {
            if (outcome == WorkerTimerOutcome::kExpired) {
                connection.abortReason = AbortReason::kTimeout;
                close(connection);
            }
        });
    }
    TaskScope writers(worker_, {.resource = resource_});
    std::exception_ptr writerFailure;
    const auto send = [&]() -> Task<void> {
        try {
            while (!output.ended && !output.stopped) {
                if (!output.chunkReady && !output.endRequested) {
                    co_await output.data.wait();
                    continue;
                }
                WorkerTimerRegistration writeTimer;
                const auto writeTimeout = timeout.constrainedBy(config_.writeTimeout);
                if (const auto remaining = writeTimeout.remaining()) {
                    WorkerHandleAccess::scheduleTimer(worker_, writeTimer, workerTimerDeadlineAfter(*remaining), [this, &connection](WorkerTimerOutcome outcome) noexcept {
                        if (outcome == WorkerTimerOutcome::kExpired) {
                            connection.abortReason = AbortReason::kTimeout;
                            close(connection);
                        }
                    });
                }
                const auto ending = output.endRequested && !output.chunkReady ? HttpStreamEnd::kEnd : HttpStreamEnd::kKeepOpen;
                const auto bytes = output.chunkReady ? std::string_view(output.chunk) : std::string_view{};
                std::error_code error;
                if (tls) {
                    HttpSocketTunnelTransport transport(connection.stream, &*tls);
                    error = co_await transport.writeBytes(bytes, ending);
                } else {
                    HttpSocketTunnelTransport transport(connection.stream.next_layer());
                    error = co_await transport.writeBytes(bytes, ending);
                }
                writeTimer.cancel();
                throwAbort(connection);
                if (error) {
                    throw HttpClientError(transportErrorCode(error), error.message());
                }
                bytesSent_ += bytes.size();
                if (ending == HttpStreamEnd::kEnd) {
                    output.finish();
                } else {
                    output.acknowledgeChunk();
                }
            }
        } catch (...) {
            writerFailure = std::current_exception();
            close(connection);
            output.stop();
        }
    };
    writers.spawn(send());
    std::exception_ptr receiveFailure;
    try {
        std::array<char, 4096> input{};
        for (;;) {
            while (state.pending.size() >= config_.maxResponseBytes && !state.abandoned) {
                co_await state.spaceSignal.wait();
                throwAbort(connection);
            }
            if (state.abandoned) {
                throw HttpClientError(HttpClientError::Code::kCancelled, "CONNECT tunnel abandoned");
            }
            if (!connection.readBuffer.empty()) {
                const auto count = std::min(connection.readBuffer.size(), config_.maxResponseBytes - state.pending.size());
                state.pending.append(connection.readBuffer.data(), count);
                connection.readBuffer.erase(0, count);
                state.dataSignal.notify();
                continue;
            }
            const auto count = co_await readSome(connection, std::span<char>(input).first(std::min(input.size(), config_.maxResponseBytes - state.pending.size())), timeout, true);
            if (count == 0) {
                output.receiveEnded = true;
                state.dataSignal.notify();
                break;
            }
            state.pending.append(input.data(), count);
            state.dataSignal.notify();
        }
    } catch (...) {
        receiveFailure = std::current_exception();
        close(connection);
        output.stop();
    }
    co_await writers.join();
    if (tls) {
        co_await tls->join();
    }
    lifetimeTimer.cancel();
    close(connection);
    if (writerFailure) {
        std::rethrow_exception(writerFailure);
    }
    if (receiveFailure) {
        std::rethrow_exception(receiveFailure);
    }
}

Task<void> HttpClientPool::writeUploadBytes(Connection& connection, std::string_view bytes, const OperationTimeout& timeout) {
    if (bytes.empty()) {
        co_return;
    }
    const auto writeTimeout = timeout.constrainedBy(config_.writeTimeout);
    WorkerTimerRegistration timer;
    if (const auto remaining = writeTimeout.remaining()) {
        if (remaining->count() == 0) {
            throw HttpClientError(HttpClientError::Code::kTimeout, "HTTP upload write timed out");
        }
        WorkerHandleAccess::scheduleTimer(worker_, timer, workerTimerDeadlineAfter(*remaining), [&connection](WorkerTimerOutcome outcome) noexcept {
            if (outcome == WorkerTimerOutcome::kExpired) {
                connection.abortReason = AbortReason::kTimeout;
                if (connection.activeHttp1Response != nullptr) {
                    if (auto* output = connection.activeHttp1Response->output()) {
                        output->stop();
                    }
                }
                std::error_code ignored;
                connection.stream.lowest_layer().cancel(ignored);
            }
        });
    }
    const auto completion = config_.scheme == HttpScheme::kHttps
                                ? co_await asyncAsio<std::size_t>([&connection, bytes](auto handler) { asio::async_write(connection.stream, asio::buffer(bytes), std::move(handler)); })
                                : co_await asyncAsio<std::size_t>([&connection, bytes](auto handler) { asio::async_write(connection.stream.next_layer(), asio::buffer(bytes), std::move(handler)); });
    timer.cancel();
    throwAbort(connection);
    if (completion.errorCode()) {
        throw HttpClientError(transportErrorCode(completion.errorCode()), completion.errorCode().message());
    }
    bytesSent_ += completion.result();
}

Task<void> HttpClientPool::writeHttp1Upload(Connection& connection, HttpClientResponseState& state,
    const OperationTimeout& timeout, Http1RequestContentWriter writer, Http1ClientResponseParser& parser,
    std::exception_ptr& failure) {
    auto& upload = *state.upload;
    WorkerTimerRegistration continueTimer;
    if (!upload.contentReleased) {
        WorkerHandleAccess::scheduleTimer(worker_, continueTimer, workerTimerDeadlineAfter(upload.config.continueTimeout), [&upload](WorkerTimerOutcome outcome) noexcept {
            if (outcome == WorkerTimerOutcome::kExpired && !upload.stopped) {
                upload.contentReleased = true;
                upload.notifyData();
            }
        });
    }
    try {
        for (;;) {
            if (upload.stopped) {
                writer.abort();
                co_return;
            }
            throwAbort(connection);
            if (timeout.expired()) {
                throw HttpClientError(HttpClientError::Code::kTimeout, "HTTP upload timed out");
            }
            if (!upload.contentReleased || (!upload.chunkReady && !upload.endRequested)) {
                co_await upload.data.wait();
                continue;
            }
            writer.releaseContent();
            continueTimer.cancel();
            if (upload.chunkReady) {
                const auto chunk = writer.planChunk(std::span<const char>(upload.chunk.data(), upload.chunk.size()));
                if (!chunk) {
                    throw HttpClientError(HttpClientError::Code::kInvalidRequest, "HTTP upload content length mismatch");
                }
                co_await writeUploadBytes(connection, std::string_view(chunk->prefix.data(), chunk->prefixSize), timeout);
                if (upload.stopped) {
                    writer.abort();
                    co_return;
                }
                co_await writeUploadBytes(connection, std::string_view(chunk->payload.data(), chunk->payload.size()), timeout);
                co_await writeUploadBytes(connection, chunk->suffix, timeout);
                if (!writer.commitChunk(chunk->payload.size())) {
                    std::terminate();
                }
                upload.acknowledgeChunk();
                continue;
            }
            std::pmr::vector<HttpHeaderView> trailers(state.resource);
            for (const auto& field : upload.trailers) {
                trailers.emplace_back(field.name(), field.value());
            }
            std::pmr::vector<char> scratch(kMaxHttpHeaderBytes + 1024, state.resource);
            const auto ending = writer.planFinish(scratch, trailers);
            if (!ending) {
                throw HttpClientError(HttpClientError::Code::kInvalidRequest, "HTTP upload trailer or final length rejected");
            }
            struct CompletionGuard final {
                HttpClientUploadState& upload;
                explicit CompletionGuard(HttpClientUploadState& value)
                    : upload(value) {
                    upload.completionPending = true;
                }
                ~CompletionGuard() {
                    upload.completionPending = false;
                    upload.space.notify();
                }
            } completion(upload);
            co_await writeUploadBytes(connection, *ending, timeout);
            if (!writer.commitFinish()) {
                std::terminate();
            }
            const auto completed = parser.completeRequestContent();
            if (completed == Http1ClientRequestContentCompletionStatus::kExchangeTerminal && !state.headReady) {
                std::terminate();
            }
            upload.finish();
            co_return;
        }
    } catch (...) {
        if (!state.headReady) {
            failure = std::current_exception();
        }
        upload.stop();
        close(connection);
    }
}

}  // namespace ruvia::detail
