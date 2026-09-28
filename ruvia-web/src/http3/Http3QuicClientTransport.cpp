#include "ruvia/web/detail/http3/Http3QuicClientTransport.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/time.h>
#endif

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "ruvia/http/Http3Connection.h"

namespace ruvia::detail {
namespace {

// SSL_get_error() requires the error queue to be empty before its SSL call.
class ErrorQueueScope final {
public:
    ErrorQueueScope() noexcept {
        ERR_clear_error();
    }
    void discard() noexcept {
        ERR_clear_error();
    }
    ~ErrorQueueScope() {
        ERR_clear_error();
    }
};

}  // namespace

Http3QuicClientTransport::Http3QuicClientTransport(Http3QuicClientTlsContext& tls,
    Http3QuicDatagramBridge& bridge, const Http3QuicDatagramAddress& peer,
    std::string_view host)
    : ownerThread_(std::this_thread::get_id()) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    (void)tls;
    (void)bridge;
    (void)peer;
    (void)host;
    throw std::runtime_error("QUIC client transport requires OpenSSL 3.6 or newer");
#else
    connection_ = SSL_new(tls.nativeHandle());
    if (connection_ == nullptr) {
        throw std::runtime_error("failed to create OpenSSL QUIC client connection");
    }
    try {
        tls.prepare(connection_, host);
        std::unique_ptr<BIO_ADDR, decltype(&BIO_ADDR_free)> destination(BIO_ADDR_new(), BIO_ADDR_free);
        if (!destination || !makeHttp3QuicBioAddress(peer, destination.get()) ||
            SSL_set1_initial_peer_addr(connection_, destination.get()) != 1) {
            throw std::invalid_argument("QUIC client requires a concrete UDP peer address");
        }
        if (SSL_set_default_stream_mode(connection_, SSL_DEFAULT_STREAM_MODE_NONE) != 1 ||
            SSL_set_incoming_stream_policy(connection_, SSL_INCOMING_STREAM_POLICY_ACCEPT, 0) != 1) {
            throw std::runtime_error("failed to configure QUIC client stream policy");
        }
        BIO* const bio = bridge.releaseSslBio();
        if (bio == nullptr) {
            throw std::runtime_error("QUIC datagram bridge has no SSL-side BIO");
        }
        // SSL_set_bio cannot fail and takes ownership of the bridge BIO.
        SSL_set_bio(connection_, bio, bio);
        if (SSL_set_blocking_mode(connection_, 0) != 1) {
            throw std::runtime_error("failed to configure non-blocking QUIC client");
        }
    } catch (...) {
        SSL_free(connection_);
        connection_ = nullptr;
        throw;
    }
#endif
}

Http3QuicClientTransport::~Http3QuicClientTransport() {
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
    close();
}

void Http3QuicClientTransport::close() {
    requireOwnerThread();
    streams_.reset();
    SSL_free(connection_);
    connection_ = nullptr;
    streamResource_.release();
    state_ = State::kTransportClosed;
}

void Http3QuicClientTransport::requireOwnerThread() const {
    if (std::this_thread::get_id() != ownerThread_) {
        throw std::logic_error("QUIC client transport used outside its owner thread");
    }
}

Http3QuicClientTransport::State Http3QuicClientTransport::startConnect() {
    requireOwnerThread();
    if (state_ == State::kNotStarted) {
        state_ = State::kConnecting;
        (void)advanceHandshake();
    }
    return state_;
}

Http3QuicClientTransport::State Http3QuicClientTransport::handleEvents() {
    requireOwnerThread();
    if (state_ != State::kConnecting && state_ != State::kH3Ready) {
        return state_;
    }
    ErrorQueueScope errors;
    const int result = SSL_handle_events(connection_);
    if (result != 1) {
        const int error = SSL_get_error(connection_, result);
        errors.discard();
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE &&
            error != SSL_ERROR_WANT_ACCEPT) {
            state_ = state_ == State::kH3Ready ? State::kTransportClosed : State::kTlsFailure;
            return state_;
        }
    } else {
        errors.discard();
    }
    return state_ == State::kConnecting ? advanceHandshake() : (state_ = inspectConnection());
}

Http3QuicClientTransport::State Http3QuicClientTransport::advanceHandshake() {
    ErrorQueueScope errors;
    const int result = SSL_connect(connection_);
    if (result == 1) {
        errors.discard();
        return state_ = inspectConnection();
    }
    const int error = SSL_get_error(connection_, result);
    errors.discard();
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ||
        error == SSL_ERROR_WANT_ACCEPT) {
        return state_ = State::kConnecting;
    }
    return state_ = State::kTlsFailure;
}

Http3QuicClientTransport::State Http3QuicClientTransport::inspectConnection() {
    SSL_CONN_CLOSE_INFO close{};
    ErrorQueueScope errors;
    const int closed = SSL_get_conn_close_info(connection_, &close, sizeof(close));
    errors.discard();
    if (closed == 1) {
        return State::kTransportClosed;
    }
    if (SSL_is_init_finished(connection_) == 1) {
        const unsigned char* alpn = nullptr;
        unsigned int alpnSize = 0;
        SSL_get0_alpn_selected(connection_, &alpn, &alpnSize);
        if (alpn == nullptr || alpnSize != 2 || alpn[0] != 'h' || alpn[1] != '3') {
            return State::kAlpnMismatch;
        }
        if (!streams_) {
            streams_.emplace(connection_, &streamResource_, kMaxStreamsPerConnection);
        }
        return State::kH3Ready;
    }
    return State::kConnecting;
}

std::optional<Http3QuicClientTransport::Duration>
Http3QuicClientTransport::eventTimeout() const {
    requireOwnerThread();
    if (state_ != State::kConnecting && state_ != State::kH3Ready) {
        return std::nullopt;
    }
    timeval timeout{};
    int infinite = 0;
    ErrorQueueScope errors;
    if (SSL_get_event_timeout(connection_, &timeout, &infinite) != 1) {
        errors.discard();
        throw std::runtime_error("failed to read QUIC client event timeout");
    }
    errors.discard();
    if (infinite != 0) {
        return std::nullopt;
    }
    if (timeout.tv_sec < 0 || timeout.tv_usec < 0) {
        throw std::runtime_error("QUIC client returned a negative event timeout");
    }
    using Rep = Duration::rep;
    constexpr auto maxSeconds = std::numeric_limits<Rep>::max() /
                                std::chrono::duration_cast<Duration>(std::chrono::seconds(1)).count();
    if (static_cast<unsigned long long>(timeout.tv_sec) >
        static_cast<unsigned long long>(maxSeconds)) {
        return Duration::max();
    }
    const auto seconds = std::chrono::duration_cast<Duration>(std::chrono::seconds(timeout.tv_sec));
    const auto micros = std::chrono::duration_cast<Duration>(std::chrono::microseconds(timeout.tv_usec));
    if (micros > Duration::zero() && seconds > Duration::max() - micros) {
        return Duration::max();
    }
    return std::max(Duration::zero(), seconds + micros);
}

Http3QuicClientTransport::State Http3QuicClientTransport::connectionInfo() {
    requireOwnerThread();
    if (state_ == State::kConnecting || state_ == State::kH3Ready) {
        return inspectConnection() == State::kTransportClosed ? State::kTransportClosed : state_;
    }
    return state_;
}

Http3QuicClientTransport::Error Http3QuicClientTransport::streamOperationError() {
    requireOwnerThread();
    if ((state_ == State::kConnecting || state_ == State::kH3Ready) &&
        inspectConnection() == State::kTransportClosed) {
        close();
    }
    if (state_ == State::kH3Ready && streams_) {
        return Error::kNone;
    }
    if (state_ == State::kConnecting || state_ == State::kNotStarted) {
        return Error::kHandshakePending;
    }
    if (state_ == State::kAlpnMismatch) {
        return Error::kAlpnMismatch;
    }
    return state_ == State::kTransportClosed ? Error::kClosed : Error::kNoConnection;
}

Http3QuicClientTransport::StreamWrite Http3QuicClientTransport::streamWriteError() {
    const Error error = streamOperationError();
    switch (error) {
        case Error::kHandshakePending:
            return {.status = StreamWrite::Status::kWouldBlock};
        case Error::kAlpnMismatch:
            return {.status = StreamWrite::Status::kFatal};
        case Error::kClosed:
            return {.status = StreamWrite::Status::kClosed};
        default:
            return {.status = StreamWrite::Status::kNoConnection};
    }
}

Http3QuicClientTransport::StreamRead Http3QuicClientTransport::streamReadError() {
    const Error error = streamOperationError();
    switch (error) {
        case Error::kHandshakePending:
            return {.status = StreamRead::Status::kWouldBlock};
        case Error::kAlpnMismatch:
            return {.status = StreamRead::Status::kFatal};
        case Error::kClosed:
            return {.status = StreamRead::Status::kClosed};
        default:
            return {.status = StreamRead::Status::kNoConnection};
    }
}

Http3QuicClientTransport::OpenStream
Http3QuicClientTransport::openLocalBidirectionalStream() {
    const Error error = streamOperationError();
    if (error != Error::kNone) {
        return {.error = error};
    }
    if (openedRequestsEver_ >= kMaxLifetimeRequests) {
        return {.error = Error::kConnectionRequestLimit};
    }
    if (!canOpenRequestStream(streams_->size(), openedRequestsEver_)) {
        return {.error = Error::kStreamLimitRetry};
    }
    try {
        auto created = streams_->createBidirectional();
        if (created.error == Error::kNone) {
            ++openedRequestsEver_;
        }
        return created;
    } catch (...) {
        close();
        return {.error = Error::kFatal};
    }
}

Http3QuicClientTransport::OpenStream
Http3QuicClientTransport::openLocalUnidirectionalStream() {
    const Error error = streamOperationError();
    if (error != Error::kNone) {
        return {.error = error};
    }
    try {
        return streams_->createUnidirectional();
    } catch (...) {
        close();
        return {.error = Error::kFatal};
    }
}

Http3QuicClientTransport::AcceptedStreams
Http3QuicClientTransport::acceptPeerStreams() {
    const Error error = streamOperationError();
    if (error != Error::kNone) {
        AcceptedStreams result;
        result.error = error;
        return result;
    }
    AcceptedStreams result = streams_->accept();
    if (result.error == Error::kInvalidStreamId || result.error == Error::kFatal) {
        const Error failure = result.error;
        close();
        result = {};
        result.error = failure;
    }
    return result;
}

Http3QuicClientTransport::StreamRead Http3QuicClientTransport::readStream(
    StreamId id, std::span<char> output) {
    const Error error = streamOperationError();
    if (error != Error::kNone) {
        return streamReadError();
    }
    return streams_->read(id, output);
}

Http3QuicClientTransport::StreamWrite Http3QuicClientTransport::writeStream(
    StreamId id, std::span<const char> input) {
    const Error error = streamOperationError();
    if (error != Error::kNone) {
        return streamWriteError();
    }
    return streams_->write(id, input);
}

Http3QuicClientTransport::Error Http3QuicClientTransport::finishStream(StreamId id) {
    const Error error = streamOperationError();
    return error == Error::kNone ? streams_->finish(id) : error;
}

Http3QuicClientTransport::Error Http3QuicClientTransport::streamWriteHealth(StreamId id) {
    const Error error = streamOperationError();
    return error == Error::kNone ? streams_->writeHealth(id) : error;
}

Http3QuicClientTransport::Error Http3QuicClientTransport::resetStream(
    StreamId id, std::uint64_t errorCode) {
    const Error error = streamOperationError();
    return error == Error::kNone ? streams_->reset(id, errorCode) : error;
}

Http3QuicClientTransport::StreamTermination
Http3QuicClientTransport::terminateRequestStream(StreamId id) {
    const Error error = streamOperationError();
    if (error != Error::kNone) {
        return {.send = error, .close = error};
    }
    return streams_->terminateBidirectional(
        id, static_cast<std::uint64_t>(Http3ConnectionErrorCode::kRequestCancelled));
}

Http3QuicClientTransport::Error Http3QuicClientTransport::closeStream(StreamId id) {
    const Error error = streamOperationError();
    return error == Error::kNone ? streams_->close(id) : error;
}

}  // namespace ruvia::detail
