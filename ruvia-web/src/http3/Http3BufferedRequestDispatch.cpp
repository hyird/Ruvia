#include "ruvia/web/detail/http3/Http3BufferedRequestDispatch.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/http/Http3WebSocketHandshake.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpConnectUdp.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/http/HttpTunnelSession.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http3/Http3ResponseStreamSink.h"
#include "ruvia/web/detail/router/RouteEndpoint.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/file/HttpFileOpen.h"
#include "ruvia/web/detail/server/response/HttpBufferedResponse.h"
#include "ruvia/web/detail/server/stream/HttpResponseStreamDispatch.h"
#include "ruvia/web/detail/websocket/HttpWebSocketConnection.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSession.h"
#include "ruvia/web/detail/websocket/WebSocketResponseHeaders.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::int64_t steadyNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] bool hasUnsupportedWebSocketVersion(const HttpRequest& request) noexcept {
    std::size_t versionCount = 0;
    std::string_view version;
    for (const auto& header : request.headers()) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), "sec-websocket-version")) {
            continue;
        }
        if (++versionCount != 1) {
            return false;
        }
        version = header.value();
    }
    return versionCount == 1 && version != "13";
}

class Http3TunnelTransport final {
public:
    [[nodiscard]] Task<std::optional<HttpDatagramInput>> readDatagramInput() {
        return dispatch_.readDatagramInput();
    }
    [[nodiscard]] HttpDatagramSessionConfig datagramConfig() const {
        return dispatch_.datagramConfig();
    }
    void sendDatagram(std::span<const std::byte> bytes) {
        dispatch_.sendDatagram(bytes);
    }
    explicit Http3TunnelTransport(Http3BufferedRequestDispatch& dispatch) noexcept
        : dispatch_(dispatch) {}
    [[nodiscard]] Task<HttpStreamReadResult> readMore(std::pmr::string& buffer) {
        return dispatch_.readTunnel(buffer);
    }
    [[nodiscard]] Task<std::error_code> writeBytes(std::string_view bytes, HttpStreamEnd end) {
        return dispatch_.writeTunnel(bytes, end);
    }
    void abort() noexcept {
        dispatch_.abortTunnel();
    }

private:
    Http3BufferedRequestDispatch& dispatch_;
};

class Http3WebSocketTransport final {
public:
    explicit Http3WebSocketTransport(Http3BufferedRequestDispatch& dispatch) noexcept
        : dispatch_(dispatch) {}

    [[nodiscard]] asio::any_io_executor executor() const noexcept {
        return dispatch_.executor();
    }

    [[nodiscard]] Task<HttpStreamReadResult> readMore(std::pmr::string& buffer) {
        return dispatch_.readTunnel(buffer);
    }

    [[nodiscard]] Task<std::error_code> writeBytes(std::string_view bytes,
        WebSocketTransportDisposition disposition) {
        return dispatch_.writeTunnel(bytes, disposition == WebSocketTransportDisposition::kEndTransport ? HttpStreamEnd::kEnd : HttpStreamEnd::kKeepOpen);
    }

    void abort() noexcept {
        dispatch_.abortTunnel();
    }

private:
    Http3BufferedRequestDispatch& dispatch_;
};

}  // namespace

Http3BufferedRequestDispatch::Http3BufferedRequestDispatch(
    Http3SansIoSessionEngine& session, const RouteTable& routes, WorkerMemory& worker,
    ContextServices services, const HttpServerOptions& options,
    Http3StreamMailbox& outbound, Http3StreamMessageId messageId,
    ConnectionScanner::Entry& scannerEntry, asio::any_io_executor executor,
    Http3TunnelCallbacks tunnelCallbacks, std::uint64_t responsePreludeBytes)
    : session_(session),
      routes_(routes),
      worker_(worker),
      services_(services.with_inbound_buffer_pool(*session.inbound_buffer_pool())),
      options_(options),
      outbound_(outbound),
      messageId_(messageId),
      scannerEntry_(scannerEntry),
      executor_(std::move(executor)),
      tunnelCallbacks_(tunnelCallbacks),
      activeRequestBody_(worker_.resource()),
      tunnelInputAvailable_(services_.worker()),
      tunnelOutputAvailable_(services_.worker()),
      streamFrame_(worker_.resource()),
      tunnelDataFrame_(worker_.resource()),
      interimOutput_(worker_.resource(), this, [](void* raw, const HttpInterimResponseHead& head) -> Task<void> {
          co_await static_cast<Http3BufferedRequestDispatch*>(raw)->writeInterimResponse(head);
      }),
      connectionAdvertisements_(worker_.resource(), this, [](void* raw, std::span<const std::string_view> origins) -> Task<void> {
          auto& dispatch = *static_cast<Http3BufferedRequestDispatch*>(raw);
          if (!dispatch.session_.queueOriginAdvertisement(origins)) {
              throw std::invalid_argument("HTTP/3 ORIGIN advertisement rejected");
          }
          co_return; }, nullptr),
      pushOutput_(worker_.resource(), this, [](void* raw, HttpPushRequestView request) -> Task<bool> {
          auto& dispatch = *static_cast<Http3BufferedRequestDispatch*>(raw);
          if (dispatch.tunnelCallbacks_.push == nullptr || dispatch.messageId_.pushId || dispatch.responseAborted() || dispatch.streamOutputEnded_) {
              co_return false;
          }
          co_return co_await dispatch.tunnelCallbacks_.push(dispatch.tunnelCallbacks_.context, dispatch.messageId_.streamId, request); }) {
    streamOutputActive_ = responsePreludeBytes != 0;
    publishedWireBytes_ = responsePreludeBytes;
    streamPublishedWireBytes_ = responsePreludeBytes;
}

Http3BufferedRequestDispatch::~Http3BufferedRequestDispatch() {
    if (handlerActive_) {
        std::terminate();
    }
    disarmPeerTransportFinTimeout();
    releaseDispatchStorage();
}

Task<Http3BufferedRequestDispatch::PrepareStatus> Http3BufferedRequestDispatch::prepare() & {
    if (!onWorker()) {
        co_return PrepareStatus::kWrongWorker;
    }
    if (state_ == State::kCancelled) {
        co_return PrepareStatus::kCancelled;
    }
    if (state_ != State::kCold) {
        co_return PrepareStatus::kAlreadyPrepared;
    }
    if (cancellationRequested()) {
        cancel();
        co_return PrepareStatus::kCancelled;
    }

    state_ = State::kPreparing;
    auto acquired = session_.acquireRequest(messageId_.streamId);
    if (!acquired) {
        state_ = State::kFailed;
        co_return PrepareStatus::kRequestUnavailable;
    }
    lease_.emplace(std::move(*acquired));

    try {
        requestMemory_.emplace(worker_);
        combinedWorkerAndRequestStop_ =
            combineStopTokens(services_.stopToken(), requestStopSource_.token());
        requestDeadline_.emplace(combinedWorkerAndRequestStop_);
        // Tunnel I/O waits on worker signals, so stop must wake both directions.
        requestDeadline_->token().registerCallback(tunnelStopRegistration_, [this]() noexcept {
            tunnelInputAvailable_.notify();
            tunnelOutputAvailable_.notify();
        });
        const auto* resolved = lease_->resolution().resolved();
        const auto routeDeadline = resolved != nullptr ? resolved->route().deadlineMs() : 0;
        const auto handlerDeadline = effectiveHandlerDeadline(
            options_.deadline ? std::optional{options_.deadline->handler} : std::nullopt,
            routeDeadline);
        if (handlerDeadline > std::chrono::milliseconds::zero()) {
            requestDeadline_->arm(services_.worker(), handlerDeadline);
            deadlineArmed_ = true;
        }
        const auto& request = lease_->request().request();
        const bool upstreamDeclaredEarlyData = request.header("early-data").has_value();
        requestServices_.emplace(services_.withRequestDeadline(*requestDeadline_)
                .withInterimOutput(interimOutput_)
                .withConnectionAdvertisements(connectionAdvertisements_)
                .with_early_data_info({messageId_.received_early_data,
                    upstreamDeclaredEarlyData}));
        if (tunnelCallbacks_.push != nullptr && !messageId_.pushId) {
            *requestServices_ = requestServices_->withPushOutput(pushOutput_);
        }
        if (const auto* trailers = session_.requestTrailers(messageId_.streamId)) {
            *requestServices_ = requestServices_->withRequestTrailers(*trailers);
        }
        if (const auto* priority = session_.requestPriorityUpdate(messageId_.streamId)) {
            *requestServices_ = requestServices_->withRequestPriorityUpdate(*priority);
        }
        if (session_.streamingRequest(messageId_.streamId)) {
            StreamingAccess::emplaceBodyReader(requestBodyReader_, this,
                [](void* raw) -> Task<std::optional<std::span<const std::byte>>> { co_return co_await static_cast<Http3BufferedRequestDispatch*>(raw)->readRequestBody(); });
            *requestServices_ = requestServices_->withStreamingRequestBody(*requestBodyReader_);
        }
        if (cancellationRequested()) {
            cancel();
            releaseDispatchStorage();
            co_return PrepareStatus::kCancelled;
        }
        state_ = State::kPrepared;
        co_return PrepareStatus::kPrepared;
    } catch (...) {
        fail(std::current_exception());
        co_return PrepareStatus::kFailed;
    }
}

Task<Http3BufferedRequestDispatch::RunStatus> Http3BufferedRequestDispatch::runHandler() & {
    if (!onWorker()) {
        co_return RunStatus::kWrongWorker;
    }
    if (state_ == State::kCold) {
        const auto prepared = co_await prepare();
        if (prepared == PrepareStatus::kCancelled) {
            co_return RunStatus::kCancelled;
        }
        if (prepared != PrepareStatus::kPrepared) {
            co_return RunStatus::kFailed;
        }
    } else if (state_ == State::kCancelled) {
        co_return RunStatus::kCancelled;
    } else if (state_ != State::kPrepared) {
        co_return state_ == State::kFailed ? RunStatus::kFailed : RunStatus::kAlreadyRun;
    }

    if (cancellationRequested()) {
        cancel();
        releaseDispatchStorage();
        co_return RunStatus::kCancelled;
    }

    state_ = State::kRunning;
    handlerActive_ = true;
    RunStatus result = RunStatus::kFailed;
    try {
        result = co_await runHandlerInner();
    } catch (...) {
        failure_ = std::current_exception();
        if (streamOutputActive_ && !peer_field_section_rejected_) {
            abortTunnel();
        }
    }
    // The child frame (including Router responses and preparation temporaries)
    // is gone before releasing any resource those objects may reference.
    handlerActive_ = false;
    if (cancellationRequested() || (result == RunStatus::kCancelled && !peer_field_section_rejected_)) {
        cancel();
        releaseDispatchStorage();
        co_return RunStatus::kCancelled;
    }
    if (peer_field_section_rejected_) {
        result = RunStatus::peer_field_section_limit;
    }
    if (result == RunStatus::kTunnelComplete || result == RunStatus::kOutputComplete) {
        state_ = State::kComplete;
        releaseDispatchStorage();
        co_return result;
    }
    if (result == RunStatus::peer_field_section_limit) {
        streamOutputActive_ = false;
        state_ = State::kPeerLimitRejected;
        releaseDispatchStorage();
        co_return result;
    }
    if (result != RunStatus::kResponseReady) {
        fail(failure_);
        co_return result;
    }
    state_ = State::kOutputReady;
    co_return RunStatus::kResponseReady;
}

Task<Http3BufferedRequestDispatch::RunStatus> Http3BufferedRequestDispatch::runHandlerInner() {
    const auto& request = lease_->request().request();
    const auto& resolution = lease_->resolution();
    const auto* resolved = resolution.resolved();
    const bool earlyRequestSafe =
        !messageId_.received_early_data ||
        ((request.knownMethod() == HttpKnownMethod::kGet ||
             request.knownMethod() == HttpKnownMethod::kHead) &&
            request.bodyBytes().empty() && resolved != nullptr &&
            resolved->route().endpoint().buffered() != nullptr &&
            resolved->route().endpoint().buffered()->replay_safe());
    enum class fallback_coding_policy : std::uint8_t { negotiated_then_disabled,
        immediately_disabled };
    const bool web_socket_response = earlyRequestSafe && resolved != nullptr &&
                                     resolved->route().endpoint().webSocket() != nullptr;
    const auto fallback_policy = web_socket_response
                                     ? fallback_coding_policy::immediately_disabled
                                     : fallback_coding_policy::negotiated_then_disabled;
    std::optional<HttpResponse> selectedResponse;
    if (!earlyRequestSafe) {
        selectedResponse.emplace(HttpResponse::Options{.resource = worker_.resource()});
        selectedResponse->status(http_status::kTooEarly);
        selectedResponse->header("content-length", "0");
    } else if (web_socket_response) {
        auto result = co_await runWebSocketHandler();
        if (const auto* terminal = std::get_if<RunStatus>(&result)) {
            co_return *terminal;
        }
        selectedResponse.emplace(std::get<HttpResponse>(std::move(result)));
    }
    const auto codingNegotiation = httpResponseCodingFor(request);
    auto codingPolicy = HttpResponseCodingPolicy::disabled();
    if (const auto* selection = codingNegotiation.selected()) {
        codingPolicy = HttpResponseCodingPolicy::selected(*selection);
    } else {
        codingPolicy = HttpResponseCodingPolicy::noAcceptableCoding();
    }

    if (selectedResponse.has_value()) {
        // Early rejection and uncommitted WebSocket responses bypass ordinary routing.
    } else if (resolved != nullptr && resolved->route().endpoint().tunnel() != nullptr) {
        selectedResponse = co_await runTunnelHandler();
        if (!selectedResponse.has_value()) {
            co_return cancellationRequested() || tunnelAborted_ ? RunStatus::kCancelled : RunStatus::kTunnelComplete;
        }
    } else if (resolved != nullptr && resolved->route().endpoint().responseStream() != nullptr) {
        if (codingPolicy.selection() == nullptr) {
            selectedResponse.emplace(co_await routes_.handleError(request, *requestMemory_,
                HttpErrorInfo({.status = http_status::kNotAcceptable, .code = "not_acceptable", .message = "no acceptable response content coding"}), *requestServices_));
        } else {
            Http3ResponseStreamSink sink(*this, services_.worker(), request.knownMethod(), resolved->route().endpoint().responseStream()->kind(),
                worker_.resource(), *codingPolicy.selection(), options_.compression.has_value() ? HttpResponseCodingAvailability::kIdentityAndCompression : HttpResponseCodingAvailability::kIdentityOnly);
            auto result = co_await dispatchResponseStreamWith(sink, routes_, request, *resolved, *requestMemory_, *requestServices_, [this]() noexcept { return responseAborted(); });
            if (peer_field_section_rejected_) {
                co_return RunStatus::peer_field_section_limit;
            }
            if (result.peerAbortedBeforeCommit() != nullptr) {
                co_return RunStatus::kCancelled;
            }
            if (const auto status = result.committedStatus()) {
                if (const auto* failed = result.failedAfterCommit()) {
                    options_.connectionFailure.invoke(services_.resolveConnInfo(request).client().address(), failed->exception());
                    abortTunnel();
                    co_return RunStatus::kCancelled;
                }
                co_return RunStatus::kOutputComplete;
            }
            if (auto* route = result.routeResponse()) {
                selectedResponse.emplace(std::move(*route).takeResponse());
            } else if (auto* recovered = result.recoveredFailure()) {
                selectedResponse.emplace(std::move(*recovered).takeResponse());
            } else {
                throw std::logic_error("HTTP/3 response stream dispatch has no terminal outcome");
            }
        }
    } else {
        selectedResponse.emplace(co_await routes_.dispatchBufferedResponse(request, resolution,
            *requestMemory_, options_.documentRoot.binding(), *requestServices_,
            services_.precompressedStaticFiles() ? StaticFileSelectionMode::kPrecompressed
                                                 : StaticFileSelectionMode::kIdentityOnly));
    }
    if (peer_field_section_rejected_) {
        co_return RunStatus::peer_field_section_limit;
    }
    auto response = std::move(*selectedResponse);
    // WebSocket rejection does not consume the tunnel body or enter the file /
    // interim response drivers; only its uncommitted buffered preparation joins here.
    if (!web_socket_response && session_.streamingRequest(messageId_.streamId)) {
        co_await drainRequestBody();
    }
    if (!web_socket_response && cancellationRequested()) {
        co_return RunStatus::kCancelled;
    }
    response_.emplace(std::move(response));

    auto preparation = co_await prepareBufferedHttpResponseAsync(
        request, codingPolicy, *response_, options_, services_.worker());
    if (cancellationRequested()) {
        co_return RunStatus::kCancelled;
    }
    if (const auto error = httpBufferedResponsePreparationError(
            codingPolicy, request, *response_, preparation.compressionResult())) {
        response_.reset();
        auto errorResponse = co_await routes_.handleError(
            request, *requestMemory_, *error, *requestServices_);
        if (cancellationRequested()) {
            co_return RunStatus::kCancelled;
        }
        response_.emplace(std::move(errorResponse));
        if (fallback_policy == fallback_coding_policy::immediately_disabled) {
            codingPolicy = HttpResponseCodingPolicy::disabled();
        }
        preparation = co_await prepareBufferedHttpResponseAsync(
            request, codingPolicy, *response_, options_, services_.worker());
        // Ordinary recovery checks every preparation suspension. WebSocket
        // recovery retains its immediate-disabled terminal preparation boundary.
        if (fallback_policy == fallback_coding_policy::negotiated_then_disabled && cancellationRequested()) {
            co_return RunStatus::kCancelled;
        }
        if (fallback_policy == fallback_coding_policy::negotiated_then_disabled &&
            httpBufferedResponsePreparationError(
                codingPolicy, request, *response_, preparation.compressionResult())
                .has_value()) {
            codingPolicy = HttpResponseCodingPolicy::disabled();
            preparation = co_await prepareBufferedHttpResponseAsync(
                request, codingPolicy, *response_, options_, services_.worker());
            if (cancellationRequested()) {
                co_return RunStatus::kCancelled;
            }
        }
    }

    if (!web_socket_response && response_->fileBody().has_value() && preparation.writePlan().sendBody() && preparation.writePlan().contentLength() != 0) {
        std::exception_ptr fileFailure;
        try {
            co_return co_await writeFileResponse(preparation.writePlan());
        } catch (...) {
            fileFailure = std::current_exception();
        }
        if (peer_field_section_rejected_) {
            co_return RunStatus::peer_field_section_limit;
        }
        if (interimOutput_.finalCommitted()) {
            options_.connectionFailure.invoke(services_.resolveConnInfo(request).client().address(), fileFailure);
            abortTunnel();
            co_return RunStatus::kCancelled;
        }
        response_.reset();
        response_.emplace(co_await routes_.handleException(request, *requestMemory_, fileFailure, *requestServices_));
        preparation = co_await prepareBufferedHttpResponseAsync(request, HttpResponseCodingPolicy::disabled(), *response_, options_, services_.worker());
    }
    if (!web_socket_response && streamOutputActive_) {
        co_return co_await writeBufferedAfterInterim(preparation.writePlan());
    }
    auto encodedHead = session_.encodeResponseHead(messageId_.streamId, *response_, preparation.writePlan());
    if (!encodedHead) {
        co_return encodedHead.error().kind == Http3ResponseHeadError::peer_field_section_limit
            ? RunStatus::peer_field_section_limit
            : RunStatus::kFailed;
    }
    auto output = Http3BufferedResponseOutput::create(
        *response_, preparation.writePlan(), std::move(*encodedHead), worker_, outbound_, messageId_);
    if (!output) {
        co_return output.error() == Http3BufferedResponseOutputError::kFileBodyUnsupported
            ? RunStatus::kFilePayloadUnsupported
            : RunStatus::kFailed;
    }
    output_.emplace(std::move(*output));
    co_return RunStatus::kResponseReady;
}

Task<std::optional<std::span<const std::byte>>> Http3BufferedRequestDispatch::readRequestBody() {
    std::pmr::string(activeRequestBody_.get_allocator()).swap(activeRequestBody_);
    for (;;) {
        if (responseAborted()) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        const auto failure = session_.streamingBodyFailure(messageId_.streamId);
        if (failure != Http3SansIoSessionEngine::Rejection::kNone) {
            throw HttpError({.status = failure == Http3SansIoSessionEngine::Rejection::kBodyTooLarge ? http_status::kContentTooLarge : http_status::kServiceUnavailable,
                .code = "request_body_unavailable",
                .message = "HTTP/3 request body limit exceeded"});
        }
        activeRequestBody_.resize(16 * 1024);
        const auto read = session_.readTunnelData(messageId_.streamId, std::span<char>(activeRequestBody_.data(), activeRequestBody_.size()));
        activeRequestBody_.resize(read.bytes);
        if (read.reset) {
            throw std::system_error(std::make_error_code(std::errc::connection_reset));
        }
        if (read.bytes != 0) {
            if (tunnelCallbacks_.inputConsumed != nullptr) {
                tunnelCallbacks_.inputConsumed(tunnelCallbacks_.context);
            }
            co_return std::as_bytes(std::span<const char>(activeRequestBody_.data(), activeRequestBody_.size()));
        }
        if (read.ended) {
            co_return std::nullopt;
        }
        co_await tunnelInputAvailable_.wait();
    }
}

Task<void> Http3BufferedRequestDispatch::drainRequestBody() {
    for (;;) {
        if (responseAborted()) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        std::array<char, 4096> bytes{};
        const auto read = session_.readTunnelData(messageId_.streamId, bytes);
        if (read.reset) {
            throw std::system_error(std::make_error_code(std::errc::connection_reset));
        }
        if (read.bytes != 0) {
            if (tunnelCallbacks_.inputConsumed != nullptr) {
                tunnelCallbacks_.inputConsumed(tunnelCallbacks_.context);
            }
            continue;
        }
        if (read.ended) {
            co_return;
        }
        co_await tunnelInputAvailable_.wait();
    }
}

Task<Http3BufferedRequestDispatch::RunStatus> Http3BufferedRequestDispatch::writeFileResponse(HttpBufferedResponseWritePlan plan) {
    auto* pool = services_.blockingPool();
    if (pool == nullptr) {
        pool = options_.blockingPool;
    }
    if (pool == nullptr) {
        throw HttpError({.status = http_status::kServiceUnavailable, .code = "file_io_unavailable", .message = "file output requires the server blocking pool"});
    }
    auto encoded = session_.encodeResponseHead(messageId_.streamId, *response_, plan);
    if (!encoded) {
        if (encoded.error().kind == Http3ResponseHeadError::peer_field_section_limit) {
            reject_peer_field_section();
        }
        throw std::invalid_argument("invalid HTTP/3 file response head");
    }
    const auto file = *response_->fileBody();
    auto input = co_await runBlocking(*pool, services_.worker(), requestDeadline_->token(),
        [path = file.toPath(), size = file.size(), identity = file.identity()]() mutable {
            auto opened = openResponseFileInput(HttpResponseFileView(path.c_str(), size, 0, size, identity));
            if (!opened) {
                throw std::runtime_error("HTTP/3 response file could not be opened or changed identity");
            }
            return opened;
        });
    // SETTINGS can arrive while the file open is offloaded. Recheck the final
    // decoded size before committing any response bytes.
    const auto peerLimit = session_.peerMaxFieldSectionSize();
    if (peerLimit && encoded->field_section.decodedFieldSectionSize() > *peerLimit) {
        reject_peer_field_section();
    }
    Http3DataWritePlan data(encoded->bodyPlan, plan.contentLength());
    commitFinalResponse();
    co_await publishResponseFrame(static_cast<std::uint64_t>(Http3FrameType::kHeaders), encoded->field_section.fieldSection);
    struct ReadResult final {
        ResponseFileInput input;
        std::array<char, 16 * 1024> bytes{};
        std::size_t count{};
    };
    for (std::size_t index = 0; index < response_->body_segment_count(); ++index) {
        const auto segment = response_->body_segment(index);
        if (!segment.file_) {
            std::size_t offset = 0;
            while (offset < segment.bytes_.size()) {
                const auto count = std::min<std::size_t>(16 * 1024, segment.bytes_.size() - offset);
                const auto bytes = segment.bytes_.substr(offset, count);
                const auto chunk = data.planChunk(std::span<const char>(bytes.data(), bytes.size()), false);
                if (!chunk) {
                    throw std::length_error("invalid HTTP/3 multipart content length");
                }
                co_await publishResponseFrame(static_cast<std::uint64_t>(Http3FrameType::kData), chunk->payload);
                if (!data.commitPayload(bytes.size(), false)) {
                    std::terminate();
                }
                offset += count;
            }
            continue;
        }
        input = co_await runBlocking(*pool, services_.worker(), requestDeadline_->token(),
            [source = std::move(input), offset = segment.file_->offset()]() mutable {
                source.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                if (!source) {
                    throw std::runtime_error("HTTP/3 response file range seek failed");
                }
                return std::move(source);
            });
        std::uint64_t remaining = segment.file_->length();
        while (remaining != 0) {
            auto read = co_await runBlocking(*pool, services_.worker(), requestDeadline_->token(),
                [source = std::move(input), remaining]() mutable {
                    ReadResult result{std::move(source)};
                    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(result.bytes.size(), remaining));
                    result.input.read(result.bytes.data(), static_cast<std::streamsize>(count));
                    if (!result.input || result.input.gcount() <= 0) {
                        throw std::runtime_error("HTTP/3 response file ended before its declared length");
                    }
                    result.count = static_cast<std::size_t>(result.input.gcount());
                    return result;
                });
            input = std::move(read.input);
            const auto chunk = data.planChunk(std::span<const char>(read.bytes.data(), read.count), false);
            if (!chunk) {
                throw std::length_error("invalid HTTP/3 file content length");
            }
            co_await publishResponseFrame(static_cast<std::uint64_t>(Http3FrameType::kData), chunk->payload);
            if (!data.commitPayload(read.count, false)) {
                std::terminate();
            }
            remaining -= read.count;
        }
    }
    const auto matches = co_await runBlocking(*pool, services_.worker(), requestDeadline_->token(),
        [source = std::move(input), identity = file.identity(), size = file.size()]() mutable { return source.matchesSnapshot(identity, size); });
    if (!matches) {
        throw std::runtime_error("HTTP/3 response file changed while it was being sent");
    }
    if (!data.planChunk({}, true)) {
        std::terminate();
    }
    co_await finishResponse();
    if (!data.commitPayload(0, true)) {
        std::terminate();
    }
    co_return RunStatus::kOutputComplete;
}

Task<std::optional<HttpResponse>> Http3BufferedRequestDispatch::runTunnelHandler() {
    const auto& request = lease_->request().request();
    const auto& resolved = *lease_->resolution().resolved();
    const auto& endpoint = *resolved.route().endpoint().tunnel();
    const bool udp = endpoint.protocol() == "connect-udp";
    if (udp && !validateHttpConnectUdpRequest(request)) {
        co_return co_await routes_.handleError(request, *requestMemory_,
            HttpErrorInfo({.status = http_status::kBadRequest, .message = "invalid CONNECT-UDP request head"}), *requestServices_);
    }
    std::optional<HttpTunnelSession<Http3TunnelTransport>> tunnelSession;
    auto establishAndRun = [&](Context& context) -> Task<void> {
        auto response = ContextAccess::streamingHead(context);
        if (udp) {
            auto negotiated = prepareHttpConnectUdpResponse(std::move(response), HttpProtocolVersion::kHttp3);
            if (!negotiated) {
                throw std::invalid_argument("invalid CONNECT-UDP response metadata");
            }
            response = std::move(*negotiated);
        }
        auto head = session_.encodeConnectResponseHead(messageId_.streamId, response);
        if (!head) {
            if (head.error().kind == Http3ResponseHeadError::peer_field_section_limit) {
                reject_peer_field_section();
            }
            throw std::invalid_argument("HTTP/3 CONNECT response head rejected");
        }
        if (!responseFieldSectionAllowed(head->field_section.decodedFieldSectionSize())) {
            reject_peer_field_section();
        }
        std::pmr::vector<char> framed(head->field_section.fieldSection.size() + kHttp3FrameHeaderMaxBytes, worker_.resource());
        const auto frameSize = encodeHttp3Frame(framed, static_cast<std::uint64_t>(Http3FrameType::kHeaders), head->field_section.fieldSection);
        if (!frameSize) {
            throw std::length_error("HTTP/3 CONNECT response framing failed");
        }
        framed.resize(*frameSize);
        if (tunnelCallbacks_.attachScanner == nullptr ||
            !tunnelCallbacks_.attachScanner(tunnelCallbacks_.context, messageId_.streamId, scannerEntry_)) {
            throw std::runtime_error("HTTP/3 tunnel scanner attachment failed");
        }
        ContextAccess::markTunnelHandshakeStarted(context);
        if (const auto error = co_await publishTunnelHandshake(framed); error) {
            throw std::system_error(error, "HTTP/3 CONNECT response publication");
        }
        tunnelSession.emplace(Http3TunnelTransport(*this), services_.worker(), *context.pool());
        co_await invokeTunnelHandler(*tunnelSession, scannerEntry_, endpoint.handler(), context);
    };
    const auto terminal = makeCallableRef<void, Context&>(establishAndRun);
    std::optional<HttpResponse> response;
    std::exception_ptr failure;
    try {
        response = co_await routes_.dispatchTunnel(request, resolved, *requestMemory_, terminal, *requestServices_);
    } catch (...) {
        failure = std::current_exception();
    }
    if (tunnelSession.has_value()) {
        co_await finishTunnelSession(*tunnelSession, failure, options_.connectionFailure, services_.connInfo().remote().address(), scannerEntry_, endpoint.config().peerTransportFinTimeout);
        if (!cancellationRequested() && !tunnelAborted_) {
            (void)co_await waitTunnelReceiveEnd();
        }
        co_return std::nullopt;
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    co_return std::move(response);
}

Task<Http3BufferedRequestDispatch::web_socket_dispatch_result>
Http3BufferedRequestDispatch::runWebSocketHandler() {
    const auto& request = lease_->request().request();
    const auto& resolution = lease_->resolution();
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr || resolved->route().endpoint().webSocket() == nullptr) {
        co_return RunStatus::kFailed;
    }
    const auto& endpoint = *resolved->route().endpoint().webSocket();
    peerTransportFinTimeout_ = endpoint.lifecycle().peerTransportFinTimeout;
    const auto protocol = lease_->request().extendedConnectProtocol();
    const auto validation = validateHttp3WebSocketHandshake(
        request, protocol, !session_.tunnelReceiveEnded(messageId_.streamId));

    if (!validation) {
        const auto& failure = validation.error();
        const auto unsupported = failure.kind() ==
                                     Http3WebSocketHandshakeFailure::Kind::kUnsupportedVersion ||
                                 (request.knownMethod() == HttpKnownMethod::kConnect &&
                                     httpAsciiEqualsIgnoreCase(protocol, "websocket") &&
                                     hasUnsupportedWebSocketVersion(request));
        const auto protocolError = failure.protocolError();
        auto response = co_await routes_.handleError(request, *requestMemory_,
            HttpErrorInfo({.status = protocolError.status(),
                .code = unsupported ? "websocket_version_unsupported"
                                    : "invalid_websocket_handshake",
                .message = protocolError.what()}),
            *requestServices_);
        if (unsupported) {
            response.removeHeader("Sec-WebSocket-Version");
            response.header("Sec-WebSocket-Version", "13");
        } else {
            failure.applyRequiredResponseHeaders(response);
        }
        co_return std::move(response);
    }

    using Connection = WebSocketConnection<Http3WebSocketTransport>;
    std::optional<Connection> webSocketConnection;
    auto upgradeAndRun = [&](Context& context) -> Task<void> {
        const auto responseHeaders = webSocketResponseHeaders(context);
        auto handshake = makeHttp3WebSocketHandshake(request, protocol, true,
            {.supportedSubprotocols = endpoint.subprotocols(),
                .responseHeaders = responseHeaders,
                .resource = requestMemory_->resource(),
                .deflate = endpoint.deflate()});
        if (!handshake) {
            throw std::runtime_error("HTTP/3 WebSocket handshake construction failed");
        }
        if (tunnelCallbacks_.attachScanner == nullptr ||
            !tunnelCallbacks_.attachScanner(
                tunnelCallbacks_.context, messageId_.streamId, scannerEntry_)) {
            throw std::runtime_error("HTTP/3 WebSocket scanner attachment failed");
        }
        ContextAccess::markWebSocketHandshakeStarted(context);
        if (const auto error = co_await publishTunnelHandshake(handshake->headersFrame()); error) {
            throw std::system_error(error, "failed to publish HTTP/3 WebSocket handshake");
        }
        webSocketConnection.emplace(Http3WebSocketTransport{*this}, services_.worker(),
            scannerEntry_, endpoint.lifecycle(),
            ProtocolByteLimit::limited(options_.maxWebSocketMessageBytes),
            session_.inbound_buffer_pool(), std::string_view{}, handshake->compression(),
            endpoint.deflate().compressionLevel);
        co_await invokeWebSocketHandler(*webSocketConnection, scannerEntry_,
            endpoint.handler(), context);
    };
    const auto terminal = makeCallableRef<void, Context&>(upgradeAndRun);
    std::optional<HttpResponse> buffered;
    std::exception_ptr exception;
    try {
        buffered = co_await routes_.dispatchWebSocket(
            request, *resolved, *requestMemory_, terminal, *requestServices_);
    } catch (...) {
        exception = std::current_exception();
    }

    if (webSocketConnection.has_value()) {
        co_await finishWebSocketSession(*webSocketConnection, exception,
            options_.connectionFailure, services_.connInfo().remote().address());
        if (cancellationRequested()) {
            co_return RunStatus::kCancelled;
        }
        const auto receiveEnded = co_await waitTunnelReceiveEnd();
        co_return receiveEnded ? RunStatus::kTunnelComplete : RunStatus::kCancelled;
    }
    if (cancellationRequested()) {
        co_return RunStatus::kCancelled;
    }
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
    if (buffered.has_value()) {
        co_return std::move(*buffered);
    }
    co_return RunStatus::kFailed;
}

Http3BufferedRequestDispatch::PublicationDemand
Http3BufferedRequestDispatch::publicationDemand() const noexcept {
    if (!onWorker()) {
        return PublicationDemand::kWrongWorker;
    }
    if (streamOutputActive_) {
        if (state_ == State::kCancelled || cancellationRequested()) {
            return PublicationDemand::kLocalCancelled;
        }
        if (streamOutputEnded_) {
            return PublicationDemand::kLocalComplete;
        }
        if (outbound_.stopped()) {
            return PublicationDemand::kLocalMailboxStopped;
        }
        if (streamFrameOffset_ < streamFrame_.size() || tunnelDataPending_) {
            return PublicationDemand::kData;
        }
        if (tunnelEstablishedPending_ || tunnelFinPending_) {
            return PublicationDemand::kControl;
        }
        return PublicationDemand::kNotReady;
    }
    switch (state_) {
        case State::kComplete:
            return PublicationDemand::kLocalComplete;
        case State::kCancelled:
            return PublicationDemand::kLocalCancelled;
        case State::kPeerLimitRejected:
            return PublicationDemand::kLocalPeerLimitRejected;
        case State::kFailed:
            return PublicationDemand::kLocalFailed;
        case State::kCold:
        case State::kPreparing:
        case State::kPrepared:
        case State::kRunning:
            return PublicationDemand::kNotReady;
        case State::kOutputReady:
        case State::kPublishing:
            break;
    }
    if (cancellationRequested()) {
        return PublicationDemand::kLocalCancelled;
    }
    if (outbound_.stopped()) {
        return PublicationDemand::kLocalMailboxStopped;
    }
    if (!output_) {
        return PublicationDemand::kLocalFailed;
    }
    // The peer limit is advisory: gate only before the first handoff. Once a
    // HEADERS prefix is published it cannot be withdrawn or rewritten.
    if (publishedWireBytes_ == 0 && exceedsPeerFieldSectionLimit()) {
        return PublicationDemand::kLocalPeerLimitRejected;
    }
    switch (output_->nextStep()) {
        case Http3BufferedResponseOutput::NextStep::bytes:
            return PublicationDemand::kData;
        case Http3BufferedResponseOutput::NextStep::fin:
            return PublicationDemand::kControl;
        case Http3BufferedResponseOutput::NextStep::complete:
        case Http3BufferedResponseOutput::NextStep::failed:
            return PublicationDemand::kLocalFailed;
    }
    return PublicationDemand::kLocalFailed;
}

Http3BufferedRequestDispatch::PublishResult Http3BufferedRequestDispatch::publishStep() & noexcept {
    const auto demand = publicationDemand();
    if (streamOutputActive_ && (demand == PublicationDemand::kData ||
                                   demand == PublicationDemand::kControl)) {
        return publishStreamStep(demand);
    }
    switch (demand) {
        case PublicationDemand::kWrongWorker:
            return {PublishStatus::kWrongWorker};
        case PublicationDemand::kNotReady:
            return {PublishStatus::kNotReady};
        case PublicationDemand::kLocalComplete:
            return {PublishStatus::kComplete};
        case PublicationDemand::kLocalCancelled:
            cancel();
            return {PublishStatus::kCancelled};
        case PublicationDemand::kLocalMailboxStopped:
            fail();
            return {PublishStatus::kFailed};
        case PublicationDemand::kLocalPeerLimitRejected:
            if (state_ == State::kPeerLimitRejected) {
                return {PublishStatus::kPeerLimitRejected};
            }
            state_ = State::kPublishing;
            (void)outbound_.drainReturns();
            state_ = State::kPeerLimitRejected;
            releaseDispatchStorage();
            return {PublishStatus::kPeerLimitRejected};
        case PublicationDemand::kLocalFailed:
            if (state_ != State::kFailed) {
                fail();
            }
            return {PublishStatus::kFailed};
        case PublicationDemand::kData:
        case PublicationDemand::kControl:
            break;
    }

    state_ = State::kPublishing;
    const auto publication = output_->publishStep();
    publishedWireBytes_ += publication.bytesAccepted;
    switch (publication.status) {
        case Http3BufferedResponseOutput::Status::kBytes:
            return {PublishStatus::kBytesPublished, publication.bytesAccepted,
                publication.notifyPeer};
        case Http3BufferedResponseOutput::Status::kFin:
            state_ = State::kComplete;
            releaseDispatchStorage();
            return {PublishStatus::kFinPublished, 0, publication.notifyPeer};
        case Http3BufferedResponseOutput::Status::kBackpressured:
            return {PublishStatus::kBackpressured, 0, publication.notifyPeer,
                publication.blockReason == Http3BufferedResponseOutput::BlockReason::kData
                    ? PublishBlockReason::kData
                : publication.blockReason == Http3BufferedResponseOutput::BlockReason::kControl
                    ? PublishBlockReason::kControl
                    : PublishBlockReason::kNone};
        case Http3BufferedResponseOutput::Status::kComplete:
            state_ = State::kComplete;
            releaseDispatchStorage();
            return {PublishStatus::kComplete, 0, publication.notifyPeer};
        case Http3BufferedResponseOutput::Status::kFailed:
            fail();
            return {PublishStatus::kFailed, publication.bytesAccepted,
                publication.notifyPeer};
    }
    fail();
    return {PublishStatus::kFailed, publication.bytesAccepted, publication.notifyPeer};
}

Http3BufferedRequestDispatch::PublishResult
Http3BufferedRequestDispatch::publishStreamStep(PublicationDemand demand) noexcept {
    if (!onWorker()) {
        return {PublishStatus::kWrongWorker};
    }
    if (demand == PublicationDemand::kLocalCancelled) {
        cancel();
        return {PublishStatus::kCancelled};
    }
    if (demand == PublicationDemand::kLocalMailboxStopped) {
        cancel();
        return {PublishStatus::kCancelled};
    }
    if (demand == PublicationDemand::kNotReady) {
        return {PublishStatus::kNotReady};
    }

    bool notifyPeer = false;
    if (demand == PublicationDemand::kData) {
        std::span<const std::byte> bytes;
        if (streamFrameOffset_ < streamFrame_.size()) {
            const auto count = (std::min)(Http3StreamMailbox::kMaxBlockBytes,
                streamFrame_.size() - streamFrameOffset_);
            bytes = std::as_bytes(std::span<const char>(
                streamFrame_.data() + streamFrameOffset_, count));
        } else if (tunnelDataPending_) {
            bytes = std::as_bytes(std::span<const char>(
                tunnelDataFrame_.data(), tunnelDataFrame_.size()));
        } else {
            return {PublishStatus::kNotReady};
        }
        if (bytes.size() > kHttp3VarIntMax - streamPublishedWireBytes_) {
            return {PublishStatus::kFailed};
        }
        const auto result = outbound_.trySend(messageId_, bytes);
        if (result == Http3StreamMailbox::SendResult::kFull || result == Http3StreamMailbox::SendResult::kNoBlock) {
            return {PublishStatus::kBackpressured, 0, false, PublishBlockReason::kData};
        }
        if (result != Http3StreamMailbox::SendResult::kSent &&
            result != Http3StreamMailbox::SendResult::kSentNotifyPeer) {
            return {PublishStatus::kFailed};
        }
        notifyPeer = result == Http3StreamMailbox::SendResult::kSentNotifyPeer;
        streamPublishedWireBytes_ += bytes.size();
        publishedWireBytes_ += bytes.size();
        if (streamFrameOffset_ < streamFrame_.size()) {
            streamFrameOffset_ += bytes.size();
            if (streamFrameOffset_ == streamFrame_.size()) {
                tunnelOutputAvailable_.notify();
            }
        } else {
            tunnelDataPending_ = false;
            tunnelDataFrame_.clear();
            tunnelOutputAvailable_.notify();
        }
        return {PublishStatus::kBytesPublished, bytes.size(), notifyPeer};
    }

    const bool establishingTunnel = tunnelEstablishedPending_;
    Http3StreamControl event{
        .kind = establishingTunnel ? Http3StreamControl::Kind::kTunnelEstablished
                                   : Http3StreamControl::Kind::kStreamFin,
        .id = messageId_,
        .value = streamPublishedWireBytes_};
    const auto result = outbound_.trySendControl(event);
    if (result == Http3StreamMailbox::ControlResult::kFull) {
        return {PublishStatus::kBackpressured, 0, false, PublishBlockReason::kControl};
    }
    if (result != Http3StreamMailbox::ControlResult::kSent &&
        result != Http3StreamMailbox::ControlResult::kSentNotifyPeer) {
        return {PublishStatus::kFailed};
    }
    notifyPeer = result == Http3StreamMailbox::ControlResult::kSentNotifyPeer;
    if (establishingTunnel) {
        tunnelEstablishedPending_ = false;
        tunnelEstablishedPublished_ = true;
        tunnelOutputAvailable_.notify();
        return {PublishStatus::kControlPublished, 0, notifyPeer};
    }
    tunnelFinPending_ = false;
    streamOutputEnded_ = true;
    if (tunnelEstablishedPublished_) {
        armPeerTransportFinTimeout();
    }
    tunnelOutputAvailable_.notify();
    return {PublishStatus::kFinPublished, 0, notifyPeer};
}

bool Http3BufferedRequestDispatch::registerPublicationDeadlineCallback(
    MoveOnlyFunction<void()> callback) & {
    if (!onWorker()) {
        throw std::logic_error("HTTP/3 publication deadline registration must run on its worker");
    }
    if (state_ != State::kOutputReady || !callback) {
        throw std::logic_error("HTTP/3 publication deadline registration requires ready output");
    }
    if (!deadlineArmed_) {
        return false;
    }
    if (publicationDeadlineCallbackRegistered_ || !requestDeadline_) {
        throw std::logic_error("HTTP/3 publication deadline callback is already registered");
    }
    publicationDeadlineCallbackRegistered_ = true;
    requestDeadline_->token().registerCallback(
        publicationDeadlineRegistration_, std::move(callback));
    return true;
}

Http3BufferedRequestDispatch::CancellationReason
Http3BufferedRequestDispatch::cancellationReason() const noexcept {
    latchCancellationReason();
    return cancellationReason_;
}

void Http3BufferedRequestDispatch::cancel() & noexcept {
    if (!onWorker()) {
        std::terminate();
    }
    if (state_ == State::kComplete || state_ == State::kFailed ||
        state_ == State::kCancelled || state_ == State::kPeerLimitRejected) {
        return;
    }
    // Set the terminal state first: stop callbacks may resume a suspended handler.
    disarmPeerTransportFinTimeout();
    cancellationRequested_ = true;
    latchCancellationReason();
    state_ = State::kCancelled;
    tunnelAborted_ = true;
    requestStopSource_.requestStop();
    tunnelInputAvailable_.notify();
    tunnelOutputAvailable_.notify();
    if (!handlerActive_) {
        releaseDispatchStorage();
    }
}

Task<std::error_code> Http3BufferedRequestDispatch::publishTunnelHandshake(
    std::span<const char> headersFrame) {
    if (!onWorker() || headersFrame.empty() || interimOutput_.finalCommitted() ||
        streamFrameOffset_ < streamFrame_.size() || tunnelCallbacks_.outputReady == nullptr) {
        co_return std::make_error_code(std::errc::invalid_argument);
    }
    try {
        streamFrame_.assign(headersFrame.data(), headersFrame.size());
    } catch (...) {
        co_return std::make_error_code(std::errc::not_enough_memory);
    }
    commitFinalResponse();
    streamOutputActive_ = true;
    streamFrameOffset_ = 0;
    notifyTunnelOutput();
    while (streamFrameOffset_ < streamFrame_.size()) {
        if (cancellationRequested() || tunnelAborted_) {
            co_return std::make_error_code(std::errc::operation_canceled);
        }
        co_await tunnelOutputAvailable_.wait();
    }
    if (cancellationRequested() || tunnelAborted_) {
        co_return std::make_error_code(std::errc::operation_canceled);
    }
    tunnelEstablishedPending_ = true;
    notifyTunnelOutput();
    while (!tunnelEstablishedPublished_) {
        if (cancellationRequested() || tunnelAborted_) {
            co_return std::make_error_code(std::errc::operation_canceled);
        }
        co_await tunnelOutputAvailable_.wait();
    }
    co_return std::error_code{};
}

Task<void> Http3BufferedRequestDispatch::writeInterimResponse(const HttpInterimResponseHead& head) {
    auto encoded = session_.encodeInterimResponseHead(messageId_.streamId, head);
    if (!encoded) {
        if (encoded.error().kind == Http3ResponseHeadError::peer_field_section_limit) {
            reject_peer_field_section();
        }
        throw std::invalid_argument("invalid HTTP/3 interim response head");
    }
    if (!responseFieldSectionAllowed(encoded->field_section.decodedFieldSectionSize())) {
        reject_peer_field_section();
    }
    co_await publishResponseFrame(static_cast<std::uint64_t>(Http3FrameType::kHeaders), encoded->field_section.fieldSection);
}

Task<Http3BufferedRequestDispatch::RunStatus> Http3BufferedRequestDispatch::writeBufferedAfterInterim(HttpBufferedResponseWritePlan plan) {
    auto encoded = session_.encodeResponseHead(messageId_.streamId, *response_, plan);
    if (!encoded) {
        if (encoded.error().kind == Http3ResponseHeadError::peer_field_section_limit) {
            reject_peer_field_section();
        }
        throw std::invalid_argument("invalid HTTP/3 buffered response head");
    }
    if (!responseFieldSectionAllowed(encoded->field_section.decodedFieldSectionSize())) {
        reject_peer_field_section();
    }
    commitFinalResponse();
    co_await publishResponseFrame(static_cast<std::uint64_t>(Http3FrameType::kHeaders), encoded->field_section.fieldSection);
    if (plan.sendBody()) {
        auto body = response_->bodyBytes();
        while (!body.empty()) {
            const auto count = std::min(body.size(), std::size_t{16 * 1024});
            co_await publishResponseFrame(static_cast<std::uint64_t>(Http3FrameType::kData), std::span<const char>(body.data(), count));
            body.remove_prefix(count);
        }
    }
    co_await finishResponse();
    co_return RunStatus::kOutputComplete;
}

Task<void> Http3BufferedRequestDispatch::publishResponseFrame(std::uint64_t type, std::span<const char> payload) {
    if (responseAborted()) {
        throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    }
    if (!onWorker() || streamOutputEnded_ || tunnelCallbacks_.outputReady == nullptr || streamFrameOffset_ < streamFrame_.size()) {
        throw std::logic_error("HTTP/3 response publication is unavailable or already active");
    }
    std::array<char, kHttp3FrameHeaderMaxBytes> header{};
    const auto encoded = encodeHttp3FrameHeader(header, type, payload.size());
    if (!encoded) {
        throw std::length_error("HTTP/3 response frame exceeds its wire limit");
    }
    streamFrame_.assign(header.data(), *encoded);
    streamFrame_.append(payload.data(), payload.size());
    co_await awaitResponsePublication();
}

Task<void> Http3BufferedRequestDispatch::publishResponseBytes(std::span<const char> bytes) {
    if (responseAborted()) {
        throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    }
    if (!onWorker() || streamOutputEnded_ || tunnelCallbacks_.outputReady == nullptr || streamFrameOffset_ < streamFrame_.size()) {
        throw std::logic_error("HTTP/3 response publication is unavailable or already active");
    }
    streamFrame_.assign(bytes.data(), bytes.size());
    co_await awaitResponsePublication();
}

Task<void> Http3BufferedRequestDispatch::awaitResponsePublication() {
    streamFrameOffset_ = 0;
    streamOutputActive_ = true;
    notifyTunnelOutput();
    while (streamFrameOffset_ < streamFrame_.size()) {
        if (responseAborted()) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        co_await tunnelOutputAvailable_.wait();
    }
    if (responseAborted()) {
        throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    }
    std::pmr::string(streamFrame_.get_allocator()).swap(streamFrame_);
    streamFrameOffset_ = 0;
}

void Http3BufferedRequestDispatch::reject_peer_field_section() {
    // The router can recover an uncommitted writer exception into an application
    // response. Preserve this transport refusal independently of that recovery;
    // the connection owner resets only this stream after joining its handler.
    peer_field_section_rejected_ = true;
    throw std::length_error("HTTP/3 response exceeds peer field section limit");
}

bool Http3BufferedRequestDispatch::responseFieldSectionAllowed(std::size_t decodedSize) const noexcept {
    const auto limit = session_.peerMaxFieldSectionSize();
    return !limit || decodedSize <= *limit;
}

Task<void> Http3BufferedRequestDispatch::finishResponse() {
    if (streamOutputEnded_) {
        co_return;
    }
    if (!streamOutputActive_ || tunnelFinPending_ || responseAborted()) {
        throw std::logic_error("HTTP/3 response cannot be concluded in this state");
    }
    tunnelFinPending_ = true;
    notifyTunnelOutput();
    while (!streamOutputEnded_) {
        if (responseAborted()) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        co_await tunnelOutputAvailable_.wait();
    }
}

void Http3BufferedRequestDispatch::notifyTunnelInput() noexcept {
    if (!onWorker()) {
        std::terminate();
    }
    tunnelInputAvailable_.notify();
}

Task<std::optional<HttpDatagramInput>> Http3BufferedRequestDispatch::readDatagramInput() {
    if (!onWorker()) {
        throw std::logic_error("HTTP Datagram read requires its worker");
    }
    for (;;) {
        if (cancellationRequested() || tunnelAborted_) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        if (auto native = session_.takeDatagram(messageId_.streamId)) {
            co_return HttpDatagramInput{std::move(*native), true};
        }
        std::array<char, 4096> bytes{};
        const auto result = session_.readTunnelData(messageId_.streamId, bytes);
        if (result.reset || result.ended) {
            disarmPeerTransportFinTimeout();
        }
        if (result.overflow) {
            abortTunnel();
            throw std::system_error(std::make_error_code(std::errc::no_buffer_space));
        }
        if (result.reset) {
            throw std::system_error(std::make_error_code(std::errc::connection_reset));
        }
        if (result.bytes != 0) {
            if (tunnelCallbacks_.inputConsumed) {
                tunnelCallbacks_.inputConsumed(tunnelCallbacks_.context);
            }
            co_return HttpDatagramInput{std::pmr::string(bytes.data(), result.bytes, worker_.resource()), false};
        }
        if (result.ended) {
            co_return std::nullopt;
        }
        co_await tunnelInputAvailable_.wait();
    }
}
void Http3BufferedRequestDispatch::sendDatagram(std::span<const std::byte> bytes) {
    if (!onWorker() || !streamOutputActive_ || streamOutputEnded_ || cancellationRequested() || tunnelAborted_ || !tunnelCallbacks_.sendDatagram) {
        throw std::runtime_error("HTTP Datagram sending direction is closed");
    }
    tunnelCallbacks_.sendDatagram(tunnelCallbacks_.context, messageId_.streamId, bytes);
}

Task<HttpStreamReadResult> Http3BufferedRequestDispatch::readTunnel(
    std::pmr::string& buffer) {
    if (!onWorker()) {
        co_return HttpStreamReadResult::makeFailure(
            std::make_error_code(std::errc::operation_not_permitted));
    }
    for (;;) {
        if (cancellationRequested() || tunnelAborted_) {
            co_return HttpStreamReadResult::makeFailure(
                std::make_error_code(std::errc::operation_canceled));
        }
        std::array<char, 4096> bytes{};
        const auto result = session_.readTunnelData(messageId_.streamId, bytes);
        if (result.reset || result.ended) {
            disarmPeerTransportFinTimeout();
        }
        if (result.overflow) {
            abortTunnel();
            co_return HttpStreamReadResult::makeFailure(
                std::make_error_code(std::errc::no_buffer_space));
        }
        if (result.reset) {
            co_return HttpStreamReadResult::makeFailure(
                std::make_error_code(std::errc::connection_reset));
        }
        if (result.bytes != 0) {
            buffer.append(bytes.data(), result.bytes);
            if (tunnelCallbacks_.inputConsumed != nullptr) {
                tunnelCallbacks_.inputConsumed(tunnelCallbacks_.context);
            }
            co_return HttpStreamReadResult::makeData();
        }
        if (result.ended) {
            co_return HttpStreamReadResult::makeEnd();
        }
        co_await tunnelInputAvailable_.wait();
    }
}

Task<std::error_code> Http3BufferedRequestDispatch::writeTunnel(
    std::string_view bytes, HttpStreamEnd disposition) {
    if (!onWorker() || !streamOutputActive_ || streamOutputEnded_) {
        co_return std::make_error_code(std::errc::operation_not_permitted);
    }
    constexpr auto maxFrameHeader = kHttp3FrameHeaderMaxBytes;
    constexpr auto maxPayload = Http3StreamMailbox::kMaxBlockBytes - maxFrameHeader;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (cancellationRequested() || tunnelAborted_) {
            co_return std::make_error_code(std::errc::operation_canceled);
        }
        if (tunnelDataPending_ || tunnelFinPending_) {
            co_return std::make_error_code(std::errc::operation_not_permitted);
        }
        const auto count = (std::min)(maxPayload, bytes.size() - offset);
        std::array<char, maxFrameHeader> header{};
        const auto encoded = encodeHttp3FrameHeader(header,
            static_cast<std::uint64_t>(Http3FrameType::kData), count);
        if (!encoded) {
            abortTunnel();
            co_return std::make_error_code(std::errc::message_size);
        }
        try {
            tunnelDataFrame_.assign(header.data(), *encoded);
            tunnelDataFrame_.append(bytes.data() + offset, count);
        } catch (...) {
            abortTunnel();
            co_return std::make_error_code(std::errc::not_enough_memory);
        }
        tunnelDataPending_ = true;
        notifyTunnelOutput();
        while (tunnelDataPending_) {
            if (cancellationRequested() || tunnelAborted_) {
                co_return std::make_error_code(std::errc::operation_canceled);
            }
            co_await tunnelOutputAvailable_.wait();
        }
        offset += count;
    }
    if (disposition == HttpStreamEnd::kEnd) {
        tunnelFinPending_ = true;
        notifyTunnelOutput();
        while (!streamOutputEnded_) {
            if (cancellationRequested() || tunnelAborted_) {
                co_return std::make_error_code(std::errc::operation_canceled);
            }
            co_await tunnelOutputAvailable_.wait();
        }
    }
    co_return std::error_code{};
}

Task<bool> Http3BufferedRequestDispatch::waitTunnelReceiveEnd() {
    if (!onWorker()) {
        co_return false;
    }
    for (;;) {
        if (cancellationRequested() || tunnelAborted_) {
            co_return false;
        }
        const auto result = session_.readTunnelData(messageId_.streamId, {});
        if (result.reset || result.overflow) {
            disarmPeerTransportFinTimeout();
            co_return false;
        }
        if (result.ended) {
            disarmPeerTransportFinTimeout();
            co_return true;
        }
        co_await tunnelInputAvailable_.wait();
    }
}

void Http3BufferedRequestDispatch::peerTransportFinTimeoutTick(
    void* target, std::int64_t nowMs) noexcept {
    auto& dispatch = *static_cast<Http3BufferedRequestDispatch*>(target);
    if (!dispatch.onWorker()) {
        std::terminate();
    }
    const auto peerState = dispatch.session_.readTunnelData(dispatch.messageId_.streamId, {});
    if (peerState.ended || peerState.reset || peerState.overflow) {
        dispatch.disarmPeerTransportFinTimeout();
        return;
    }
    if (nowMs >= dispatch.peerTransportFinDeadlineMs_) {
        dispatch.abortTunnel();
    }
}

void Http3BufferedRequestDispatch::armPeerTransportFinTimeout() noexcept {
    if (!onWorker() || peerFinTimeoutArmed_ ||
        peerTransportFinTimeout_ <= std::chrono::milliseconds::zero()) {
        return;
    }
    const auto peerState = session_.readTunnelData(messageId_.streamId, {});
    if (peerState.ended || peerState.reset || peerState.overflow) {
        return;
    }
    const auto now = steadyNowMs();
    const auto timeout = peerTransportFinTimeout_.count();
    peerTransportFinDeadlineMs_ = timeout > (std::numeric_limits<std::int64_t>::max)() - now
                                      ? (std::numeric_limits<std::int64_t>::max)()
                                      : now + timeout;
    scannerEntry_.registerPeriodicCheck(
        peerTransportFinCheck_, this, &Http3BufferedRequestDispatch::peerTransportFinTimeoutTick);
    peerFinTimeoutArmed_ = true;
}

void Http3BufferedRequestDispatch::disarmPeerTransportFinTimeout() noexcept {
    peerTransportFinCheck_.reset();
    peerFinTimeoutArmed_ = false;
    peerTransportFinDeadlineMs_ = 0;
}

void Http3BufferedRequestDispatch::abortTunnel() noexcept {
    if (!onWorker()) {
        std::terminate();
    }
    if (tunnelAborted_) {
        return;
    }
    disarmPeerTransportFinTimeout();
    tunnelAborted_ = true;
    tunnelInputAvailable_.notify();
    tunnelOutputAvailable_.notify();
    if (tunnelCallbacks_.abort != nullptr) {
        tunnelCallbacks_.abort(tunnelCallbacks_.context, messageId_.streamId);
    }
}

void Http3BufferedRequestDispatch::notifyTunnelOutput() noexcept {
    if (!onWorker()) {
        std::terminate();
    }
    if (tunnelCallbacks_.outputReady != nullptr) {
        tunnelCallbacks_.outputReady(tunnelCallbacks_.context, messageId_.streamId);
    }
}

bool Http3BufferedRequestDispatch::handlerActive() const noexcept {
    return handlerActive_;
}

bool Http3BufferedRequestDispatch::responseReady() const noexcept {
    return state_ == State::kOutputReady || state_ == State::kPublishing;
}

bool Http3BufferedRequestDispatch::complete() const noexcept {
    return state_ == State::kComplete;
}

std::uint64_t Http3BufferedRequestDispatch::publishedWireBytes() const noexcept {
    return publishedWireBytes_;
}

std::exception_ptr Http3BufferedRequestDispatch::failure() const noexcept {
    return failure_;
}

bool Http3BufferedRequestDispatch::onWorker() const noexcept {
    return services_.worker().isCurrent();
}

bool Http3BufferedRequestDispatch::cancellationRequested() const noexcept {
    latchCancellationReason();
    return cancellationReason_ != CancellationReason::kNone ||
           (requestDeadline_ && requestDeadline_->token().stopRequested());
}

void Http3BufferedRequestDispatch::latchCancellationReason() const noexcept {
    if (cancellationReason_ != CancellationReason::kNone) {
        return;
    }
    if (services_.stopToken().stopRequested()) {
        cancellationReason_ = CancellationReason::kWorkerStop;
    } else if (cancellationRequested_ || requestStopSource_.stopRequested()) {
        cancellationReason_ = CancellationReason::kExplicit;
    } else if (requestDeadline_ && requestDeadline_->token().stopRequested()) {
        cancellationReason_ = requestDeadline_->exceeded()
                                  ? CancellationReason::kDeadline
                                  : CancellationReason::kWorkerStop;
    }
}

bool Http3BufferedRequestDispatch::exceedsPeerFieldSectionLimit() const noexcept {
    const auto limit = session_.peerMaxFieldSectionSize();
    return limit.has_value() && output_ &&
           std::cmp_greater(output_->decodedFieldSectionSize(), *limit);
}

void Http3BufferedRequestDispatch::fail(std::exception_ptr failure) noexcept {
    disarmPeerTransportFinTimeout();
    if (handlerActive_) {
        std::terminate();
    }
    if (failure != nullptr) {
        failure_ = std::move(failure);
    }
    state_ = State::kFailed;
    releaseDispatchStorage();
}

void Http3BufferedRequestDispatch::releaseDispatchStorage() noexcept {
    if (handlerActive_) {
        std::terminate();
    }
    publicationDeadlineRegistration_.reset();
    tunnelStopRegistration_.reset();
    latchCancellationReason();
    requestServices_.reset();
    requestBodyReader_.reset();
    std::pmr::string(activeRequestBody_.get_allocator()).swap(activeRequestBody_);
    requestDeadline_.reset();
    combinedWorkerAndRequestStop_ = StopToken{};
    output_.reset();
    response_.reset();
    requestMemory_.reset();

    const bool hadLease = lease_.has_value();
    if (hadLease && (tunnelAborted_ || peer_field_section_rejected_)) {
        // A local abort can finish before peer FIN reaches the input mailbox.
        // Retire the protocol request while its lease still pins the head; the
        // lease then frees this stream without waiting for more peer input.
        (void)session_.cancelRequest(messageId_.streamId);
    }
    lease_.reset();
    if (hadLease) {
        // Response FIN completes only the send direction. The input owner keeps
        // an open receive direction alive until validated FIN or cancellation.
        (void)session_.release(messageId_.streamId);
    }
}

}  // namespace ruvia::detail
