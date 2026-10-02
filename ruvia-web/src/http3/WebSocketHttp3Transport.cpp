#include "ruvia/web/detail/http3/WebSocketHttp3Transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <utility>

#include <asio/post.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/WebSocketClientNegotiation.h"
#include "ruvia/web/detail/client/WebSocketClientState.h"

namespace ruvia::detail {
namespace {
constexpr std::size_t kReceiveCapacity = 64 * 1024;
constexpr std::size_t kWireBlockBytes = 16384;
using StreamRead = ruvia::quic_stream_read_result;
using StreamWrite = ruvia::quic_stream_write_result;
using StreamError = ruvia::quic_operation_status;
}  // namespace

WebSocketHttp3Transport::WebSocketHttp3Transport(WebSocketClientState& owner,
    const WorkerHandle& worker, std::pmr::memory_resource* resource)
    : owner_(owner),
      resource_(resource),
      tls_(owner.config_.transport.view(), resource),
      resolver_(owner.loop_.ioContext(), resource),
      connection_(Http3PeerRole::kClient, resource,
          {.maxActiveStreams = 1, .maxPeerUnidirectionalStreams = ruvia::quic_limits{}.max_streams, .qpackMaxTableCapacity = owner.config_.qpack.maxTableCapacity, .qpackBlockedStreams = owner.config_.qpack.maxBlockedStreams}),
      drivers_(worker, {.resource = resource}),
      progress_(worker),
      peerStreams_(resource),
      received_(resource),
      outbound_(resource),
      blockedInput_(resource),
      criticalOutput_{std::pmr::string(resource), std::pmr::string(resource)} {
    peerStreams_.reserve(ruvia::quic_limits{}.max_streams);
}

void WebSocketHttp3Transport::checkFailure() const {
    if (failure_) {
        std::rethrow_exception(failure_);
    }
    owner_.throwAbort();
}

void WebSocketHttp3Transport::wake() noexcept {
    if (session_) {
        session_->notifyWork();
    }
}

Task<void> WebSocketHttp3Transport::connect() {
    drivers_.spawn(drive());
    while (!connection_.peerSettings()) {
        checkFailure();
        co_await progress_.wait();
    }
    checkFailure();
    if (!connection_.peerSettings()->enableConnectProtocol) {
        throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
            "upstream did not enable HTTP/3 Extended CONNECT");
    }
    openRequested_ = true;
    wake();
    while (!streamId_) {
        checkFailure();
        co_await progress_.wait();
    }
    checkFailure();
    std::pmr::vector<HttpHeaderView> headers(resource_);
    headers.reserve(owner_.config_.headers.size() + 1);
    for (const auto& header : owner_.config_.headers) {
        headers.emplace_back(header.name, header.value);
    }
    if (!owner_.config_.userAgent.empty()) {
        headers.emplace_back("user-agent", owner_.config_.userAgent);
    }
    std::pmr::vector<std::string_view> protocols(resource_);
    for (const auto& protocol : owner_.config_.subprotocols) {
        protocols.emplace_back(protocol);
    }
    WebSocketClientNegotiation negotiation({.headers = headers,
                                               .subprotocols = protocols,
                                               .deflate = owner_.config_.deflate},
        resource_);
    std::pmr::vector<Http3FieldSectionFieldView> fields(resource_);
    for (const auto& field : negotiation.requestFields()) {
        fields.push_back({field.name(), field.value()});
    }
    auto authority = clientUriHost(owner_.config_.host, resource_);
    ClientPortTextBuffer portBuffer{};
    authority.append(":");
    authority.append(formatClientPort(owner_.port(), portBuffer));
    Http3FieldSectionLimits limits{};
    if (const auto maximum = connection_.peerSettings()->maxFieldSectionSize) {
        limits.maxDecodedBytes = static_cast<std::size_t>(std::min<std::uint64_t>(limits.maxDecodedBytes, *maximum));
    }
    const auto head = connection_.encodeClientRequestHead(*streamId_, {.method = "CONNECT",
                                                                          .scheme = "https",
                                                                          .authority = authority,
                                                                          .path = owner_.config_.target,
                                                                          .fields = fields,
                                                                          .protocol = "websocket",
                                                                          .peerEnableConnectProtocol = true});
    if (!head) {
        throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
            "could not encode HTTP/3 WebSocket Extended CONNECT");
    }
    prepareFrame(1, head->fieldSection);
    wake();
    while (!response_) {
        checkFailure();
        if (eof_) {
            throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
                "upstream closed before HTTP/3 WebSocket response");
        }
        co_await progress_.wait();
    }
    checkFailure();
    const auto negotiated = negotiation.validateResponse(*response_, !eof_);
    if (!negotiated) {
        throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
            "invalid HTTP/3 WebSocket handshake response");
    }
    owner_.selectedSubprotocol_.assign(negotiated->selectedSubprotocol);
    owner_.negotiatedCompression_ = negotiated->compression;
    response_.reset();
    co_await waitForOutput();
}

void WebSocketHttp3Transport::stop() noexcept {
    stopped_ = true;
    resolver_.requestStop();
    drivers_.requestStop();
    if (session_) {
        session_->requestStop();
    }
    progress_.notify();
}

Task<void> WebSocketHttp3Transport::join() {
    co_await drivers_.join();
    session_.reset();
    (void)connection_.retire();
    std::pmr::string(resource_).swap(received_);
    std::pmr::string(resource_).swap(outbound_);
    std::pmr::string(resource_).swap(blockedInput_);
    for (auto& bytes : criticalOutput_) {
        std::pmr::string(resource_).swap(bytes);
    }
}

void WebSocketHttp3Transport::prepareFrame(std::uint64_t type, std::span<const char> payload) {
    if (!outbound_.empty()) {
        throw std::logic_error("overlapping HTTP/3 WebSocket output blocks");
    }
    std::array<char, 16> header{};
    const auto encoded = encodeHttp3FrameHeader(header, type, payload.size());
    if (!encoded) {
        std::terminate();
    }
    outbound_.reserve(*encoded + payload.size());
    outbound_.append(header.data(), *encoded);
    outbound_.append(payload.data(), payload.size());
    writeOffset_ = 0;
}

void WebSocketHttp3Transport::onEvent(void* context, const Http3ConnectionEvent& event) {
    auto& self = *static_cast<WebSocketHttp3Transport*>(context);
    switch (event.kind) {
        case Http3ConnectionEventKind::kFinalHead:
            if (self.response_ || !event.head) {
                throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                    "unexpected HTTP/3 WebSocket response head");
            }
            self.response_.emplace(self.resource_);
            self.response_->status = event.head->status;
            for (const auto& field : event.head->headers) {
                self.response_->headers.emplace_back(field.name, field.value, self.resource_);
            }
            break;
        case Http3ConnectionEventKind::kTunnelData:
            if (event.body.size() > kReceiveCapacity - self.received_.size()) {
                throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                    "HTTP/3 WebSocket receive buffer exceeded");
            }
            self.received_.append(event.body.data(), event.body.size());
            break;
        case Http3ConnectionEventKind::kMessageEnd:
            self.eof_ = true;
            break;
        case Http3ConnectionEventKind::kReset:
            throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                "upstream reset HTTP/3 WebSocket stream");
        case Http3ConnectionEventKind::kBody:
            if (!event.body.empty()) {
                throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
                    "upstream rejected HTTP/3 WebSocket handshake");
            }
            break;
        default:
            break;
    }
    self.progress_.notify();
}

bool WebSocketHttp3Transport::receive(std::uint64_t id, bool request) {
    auto& transport = session_->transport();
    if (request) {
        const auto health = transport.read_health(id);
        if (health.status == ruvia::quic_stream_read_status::reset) {
            throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                "upstream reset HTTP/3 WebSocket stream");
        }
        if (health.status != ruvia::quic_stream_read_status::would_block &&
            health.status != ruvia::quic_stream_read_status::fin &&
            health.status != ruvia::quic_stream_read_status::data) {
            throw WebSocketClientError(WebSocketClientError::Code::kIoError,
                "HTTP/3 WebSocket stream transport failed");
        }
        if (eof_ || received_.size() == kReceiveCapacity) {
            return false;
        }
    }
    std::array<char, kWireBlockBytes> bytes{};
    std::span<const char> input;
    bool fin = false;
    if (request && qpackBlocked_) {
        if (blockedInput_.size() > kReceiveCapacity - received_.size()) {
            return false;
        }
        input = blockedInput_;
        fin = blockedFin_;
    } else {
        const auto capacity = request ? std::min(bytes.size(), kReceiveCapacity - received_.size()) : bytes.size();
        const auto read = transport.read_stream(id,
            std::as_writable_bytes(std::span<char>(bytes.data(), capacity)));
        if (read.status == ruvia::quic_stream_read_status::would_block) {
            return false;
        }
        if (read.status != ruvia::quic_stream_read_status::data && read.status != ruvia::quic_stream_read_status::fin) {
            throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                "HTTP/3 peer stream ended unexpectedly");
        }
        fin = read.status == ruvia::quic_stream_read_status::fin;
        input = std::span<const char>(bytes.data(), read.size);
    }
    const auto result = connection_.feed(id, input, fin, false, onEvent, this);
    if (result.status == Http3ConnectionStatus::kConnectionError || result.status == Http3ConnectionStatus::kStreamError ||
        result.status == Http3ConnectionStatus::kReset || result.status == Http3ConnectionStatus::kPushPromisePending) {
        throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
            "invalid HTTP/3 WebSocket connection input");
    }
    if (request) {
        if (result.status == Http3ConnectionStatus::kQpackBlocked) {
            if (result.consumedBytes > input.size()) {
                std::terminate();
            }
            if (qpackBlocked_) {
                blockedInput_.erase(0, result.consumedBytes);
            } else {
                blockedInput_.assign(input.data() + result.consumedBytes, input.size() - result.consumedBytes);
            }
            qpackBlocked_ = true;
            blockedFin_ = fin;
            return result.consumedBytes != 0;
        }
        qpackBlocked_ = false;
        blockedFin_ = false;
        blockedInput_.clear();
    } else if (fin) {
        (void)transport.close_stream(id);
        std::erase(peerStreams_, id);
    }
    return true;
}

bool WebSocketHttp3Transport::driveOutput() {
    bool progress = false;
    auto& transport = session_->transport();
    if (openRequested_ && !streamId_) {
        const auto opened = transport.open_stream(false);
        if (opened.status == ruvia::quic_operation_status::accepted) {
            const auto registered = connection_.registerClientRequest(opened.stream_id, HttpKnownMethod::kConnect);
            if (registered.status != Http3ConnectionStatus::kNeedMoreData) {
                throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                    "could not register HTTP/3 WebSocket stream");
            }
            streamId_ = opened.stream_id;
            progress_.notify();
            progress = true;
        } else if (opened.status != ruvia::quic_operation_status::would_block && opened.status != ruvia::quic_operation_status::need_input) {
            throw WebSocketClientError(WebSocketClientError::Code::kIoError,
                "could not open HTTP/3 WebSocket stream");
        }
    }
    if (streamId_ && !outbound_.empty()) {
        const auto bytes = std::span<const char>(outbound_.data() + writeOffset_, outbound_.size() - writeOffset_);
        const auto written = transport.write_stream(*streamId_, std::as_bytes(bytes));
        if (written.status == ruvia::quic_operation_status::accepted) {
            if (written.accepted == 0 || written.accepted > bytes.size()) {
                std::terminate();
            }
            writeOffset_ += written.accepted;
            progress = true;
            if (writeOffset_ == outbound_.size()) {
                outbound_.clear();
                writeOffset_ = 0;
                progress_.notify();
            }
        } else if (written.status != ruvia::quic_operation_status::would_block) {
            throw WebSocketClientError(WebSocketClientError::Code::kIoError,
                "could not write HTTP/3 WebSocket stream");
        }
    }
    if (streamId_ && finishRequested_ && outbound_.empty() && !finPrepared_) {
        const auto result = transport.finish_stream(*streamId_);
        if (result == ruvia::quic_operation_status::accepted) {
            finPrepared_ = true;
            finished_ = true;
            progress_.notify();
            progress = true;
        } else if (result != ruvia::quic_operation_status::would_block && result != ruvia::quic_operation_status::need_input) {
            throw WebSocketClientError(WebSocketClientError::Code::kIoError,
                "could not conclude HTTP/3 WebSocket stream");
        }
    }
    for (std::size_t index = 0; index != criticalOutput_.size(); ++index) {
        auto& bytes = criticalOutput_[index];
        auto& offset = criticalOffset_[index];
        const auto pending = index == 0 ? connection_.pendingQpackEncoderOutput() : connection_.pendingQpackDecoderOutput();
        if (bytes.empty() && !pending.empty()) {
            bytes.assign(pending.data(), std::min(pending.size(), kWireBlockBytes));
            offset = 0;
            owner_.arm(criticalTimers_[index], owner_.config_.writeTimeout, WebSocketClientState::AbortReason::kTimeout);
        }
        if (bytes.empty()) {
            continue;
        }
        const auto written = session_->writeCriticalStream(index == 0 ? Http3CriticalStreamOutput::Kind::kQpackEncoder : Http3CriticalStreamOutput::Kind::kQpackDecoder,
            std::span<const char>(bytes.data() + offset, bytes.size() - offset));
        if (written.status == ruvia::quic_operation_status::accepted) {
            if (written.accepted == 0 || written.accepted > bytes.size() - offset) {
                std::terminate();
            }
            const bool consumed = index == 0 ? connection_.consumeQpackEncoderOutput(written.accepted) : connection_.consumeQpackDecoderOutput(written.accepted);
            if (!consumed) {
                std::terminate();
            }
            offset += written.accepted;
            progress = true;
            owner_.arm(criticalTimers_[index], owner_.config_.writeTimeout, WebSocketClientState::AbortReason::kTimeout);
            if (offset == bytes.size()) {
                bytes.clear();
                offset = 0;
                owner_.disarm(criticalTimers_[index]);
            }
        } else if (written.status != ruvia::quic_operation_status::would_block) {
            throw WebSocketClientError(WebSocketClientError::Code::kIoError,
                "HTTP/3 QPACK critical stream failed");
        }
    }
    return progress;
}

Task<void> WebSocketHttp3Transport::drive() {
    try {
        const auto deadline = std::chrono::steady_clock::now() + owner_.config_.connectTimeout;
        const auto resolved = co_await resolver_.resolve(owner_.config_.host, owner_.port(), deadline);
        if (stopped_) {
            co_return;
        }
        if (resolved.status != Http3QuicClientEndpointResolver::Status::kResolved || resolved.endpoints.empty()) {
            throw WebSocketClientError(WebSocketClientError::Code::kResolveFailed,
                "could not resolve HTTP/3 WebSocket peer");
        }
        // Each address receives part of the same connect deadline; successful
        // HTTP/3 admission retains that session for the connection's lifetime.
        bool established = false;
        for (std::size_t index = 0; index < resolved.endpoints.size() && !stopped_; ++index) {
            session_.emplace(owner_.loop_.ioContext(), resolved.endpoints[index], owner_.config_.host,
                tls_, connection_.localSettings());
            const auto now = std::chrono::steady_clock::now();
            const auto attemptDeadline = now + (deadline - now) / (resolved.endpoints.size() - index);
            while (!stopped_) {
                const auto pump = session_->pump();
                if (pump.status == Http3QuicClientSocketSession::PumpStatus::kFatal || pump.status == Http3QuicClientSocketSession::PumpStatus::kClosed) {
                    break;
                }
                if (pump.criticalStreamsReady) {
                    established = true;
                    break;
                }
                if (std::chrono::steady_clock::now() >= attemptDeadline) {
                    break;
                }
                const auto wakeReason = co_await session_->waitForActivity(pump, attemptDeadline);
                if (wakeReason == Http3QuicClientSocketSession::WakeReason::kFatal) {
                    break;
                }
            }
            if (established || stopped_) {
                break;
            }
            session_->close();
            session_.reset();
        }
        if (!stopped_ && !established) {
            throw WebSocketClientError(WebSocketClientError::Code::kConnectFailed,
                "could not establish HTTP/3 WebSocket transport");
        }
        unsigned activeTicks = 0;
        while (!stopped_) {
            (void)session_->consumeWorkNotification();
            const auto pump = session_->pump();
            if (pump.status == Http3QuicClientSocketSession::PumpStatus::kFatal || pump.status == Http3QuicClientSocketSession::PumpStatus::kClosed) {
                throw WebSocketClientError(WebSocketClientError::Code::kIoError,
                    "HTTP/3 WebSocket connection failed");
            }

            auto& transport = session_->transport();
            const auto accepted = transport.accept_streams();
            if (accepted.status != ruvia::quic_operation_status::accepted &&
                accepted.status != ruvia::quic_operation_status::need_input &&
                accepted.status != ruvia::quic_operation_status::would_block) {
                throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                    "could not accept HTTP/3 peer streams");
            }
            for (std::size_t index = 0; index < accepted.size; ++index) {
                if (!accepted.streams[index].readable || accepted.streams[index].writable ||
                    peerStreams_.size() == ruvia::quic_limits{}.max_streams) {
                    throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                        "invalid HTTP/3 peer stream");
                }
                peerStreams_.push_back(accepted.streams[index].stream_id);
            }
            bool progress = accepted.size != 0 || pump.received != 0 || pump.sent != 0 || pump.criticalOutputProgress;
            for (std::size_t index = 0; index < peerStreams_.size();) {
                const auto id = peerStreams_[index];
                progress |= receive(id, false);
                if (index < peerStreams_.size() && peerStreams_[index] == id) {
                    ++index;
                }
            }
            if (streamId_) {
                progress |= receive(*streamId_, true);
            }
            if (pump.criticalStreamsReady) {
                progress |= driveOutput();
            }
            if (progress) {
                progress_.notify();
            }
            if (progress && ++activeTicks < 32) {
                continue;
            }
            if (progress) {
                (void)co_await ruvia::asyncAsio([this](auto handler) {
                    asio::post(owner_.loop_.executor(), [handler = std::move(handler)]() mutable { handler(std::error_code{}); });
                });
            } else {
                const auto reason = co_await session_->waitForActivity(pump);
                if (reason == Http3QuicClientSocketSession::WakeReason::kFatal) {
                    throw WebSocketClientError(WebSocketClientError::Code::kIoError,
                        "HTTP/3 WebSocket socket wait failed");
                }
            }
            activeTicks = 0;
        }
    } catch (...) {
        if (!stopped_ && !failure_) {
            failure_ = std::current_exception();
        }
        owner_.closeOnWorker(WebSocketClientState::AbortReason::kClosing);
    }
    for (auto& timer : criticalTimers_) {
        owner_.disarm(timer);
    }
    if (session_) {
        session_->close();
    }
    progress_.notify();
}

Task<void> WebSocketHttp3Transport::waitForOutput() {
    wake();
    while (!outbound_.empty()) {
        checkFailure();
        co_await progress_.wait();
    }
    checkFailure();
}

Task<std::size_t> WebSocketHttp3Transport::read(std::span<char> output) {
    for (;;) {
        checkFailure();
        const auto available = received_.size() - readOffset_;
        if (available != 0) {
            const auto count = std::min(output.size(), available);
            std::memcpy(output.data(), received_.data() + readOffset_, count);
            readOffset_ += count;
            if (readOffset_ == received_.size()) {
                received_.clear();
                readOffset_ = 0;
            }
            wake();
            co_return count;
        }
        if (eof_) {
            co_return 0;
        }
        wake();
        co_await progress_.wait();
    }
}

Task<void> WebSocketHttp3Transport::write(std::string_view bytes) {
    while (!bytes.empty()) {
        checkFailure();
        const auto count = std::min(bytes.size(), kWireBlockBytes);
        prepareFrame(0, std::span<const char>(bytes.data(), count));
        co_await waitForOutput();
        bytes.remove_prefix(count);
    }
}

Task<void> WebSocketHttp3Transport::finish() {
    checkFailure();
    finishRequested_ = true;
    wake();
    while (!finished_) {
        checkFailure();
        co_await progress_.wait();
    }
}

}  // namespace ruvia::detail
