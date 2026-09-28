#include "ruvia/web/detail/http3/Http3QuicServerTransport.h"

#include <algorithm>
#include <array>
#include <bit>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>

#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/http/Http3VarInt.h"

namespace ruvia::detail {
namespace {

Http3QuicServerTransport::Duration checkedHandshakeTimeout(std::chrono::milliseconds value) {
    using Duration = Http3QuicServerTransport::Duration;
    if (value <= std::chrono::milliseconds::zero() ||
        value > std::chrono::duration_cast<std::chrono::milliseconds>(Duration::max())) {
        throw std::invalid_argument("QUIC handshake timeout must be positive and representable");
    }
    return std::chrono::duration_cast<Duration>(value);
}

std::size_t checkedLifetimePeerStreamLimit(std::size_t value) {
    if (value == 0) {
        throw std::invalid_argument("QUIC lifetime peer stream limit must be positive");
    }
    return value;
}

std::uint64_t checkedIdleTimeout(
    std::optional<std::chrono::milliseconds> value) {
    constexpr std::uint64_t kMaximumQuicTimeout = (std::uint64_t{1} << 62) - 1;
    if (!value) {
        return 0;
    }
    if (*value <= std::chrono::milliseconds::zero() ||
        static_cast<std::uint64_t>(value->count()) > kMaximumQuicTimeout) {
        throw std::invalid_argument("QUIC idle timeout must be positive and representable");
    }
    return static_cast<std::uint64_t>(value->count());
}

class SslOwner final {
public:
    explicit SslOwner(SSL* value = nullptr) noexcept
        : value_(value) {}
    ~SslOwner() {
        SSL_free(value_);
    }
    SslOwner(const SslOwner&) = delete;
    SslOwner& operator=(const SslOwner&) = delete;
    [[nodiscard]] SSL* get() const noexcept {
        return value_;
    }
    [[nodiscard]] SSL* release() noexcept {
        SSL* result = value_;
        value_ = nullptr;
        return result;
    }

private:
    SSL* value_;
};

// SSL_get_error() requires an empty thread-local error queue before its SSL
// operation. QUIC objects are confined to this owner thread; no previous error
// may be preserved across an operation boundary.
std::optional<asio::ip::udp::endpoint> peerEndpoint(
    const Http3QuicDatagramAddress& peer) {
    if (peer.port == 0) {
        return std::nullopt;
    }
    if (peer.family == Http3QuicDatagramAddress::Family::kIPv4) {
        asio::ip::address_v4::bytes_type address{};
        std::copy_n(peer.address.begin(), address.size(), address.begin());
        const asio::ip::address_v4 value(address);
        return value.is_unspecified() ? std::nullopt
                                      : std::optional(asio::ip::udp::endpoint(value, peer.port));
    }
    asio::ip::address_v6::bytes_type address{};
    std::copy_n(peer.address.begin(), address.size(), address.begin());
    const asio::ip::address_v6 value(address, peer.scopeId);
    return value.is_unspecified() ? std::nullopt
                                  : std::optional(asio::ip::udp::endpoint(value, peer.port));
}

bool concretePeerEndpoint(const asio::ip::udp::endpoint& endpoint) noexcept {
    return endpoint.port() != 0 && !endpoint.address().is_unspecified();
}

std::optional<asio::ip::udp::endpoint> peerEndpoint(SSL* connection) {
    using AddressOwner = std::unique_ptr<BIO_ADDR, decltype(&BIO_ADDR_free)>;
    AddressOwner peer(BIO_ADDR_new(), BIO_ADDR_free);
    BIO* const readBio = SSL_get_rbio(connection);
    if (!peer || readBio == nullptr || BIO_dgram_get_peer(readBio, peer.get()) <= 0) {
        return std::nullopt;
    }
    const int family = BIO_ADDR_family(peer.get());
    const std::size_t expected = family == AF_INET ? 4 : family == AF_INET6 ? 16
                                                                            : 0;
    if (expected == 0) {
        return std::nullopt;
    }
    std::array<unsigned char, 16> bytes{};
    std::size_t size = bytes.size();
    if (BIO_ADDR_rawaddress(peer.get(), bytes.data(), &size) != 1 || size != expected) {
        return std::nullopt;
    }
    const auto networkPort = static_cast<std::uint16_t>(BIO_ADDR_rawport(peer.get()));
    const auto hostPort = std::endian::native == std::endian::little
                              ? std::byteswap(networkPort)
                              : networkPort;
    asio::ip::udp::endpoint endpoint;
    if (family == AF_INET) {
        asio::ip::address_v4::bytes_type address{};
        std::copy_n(bytes.begin(), address.size(), address.begin());
        endpoint = {asio::ip::address_v4(address), hostPort};
    } else {
        asio::ip::address_v6::bytes_type address{};
        std::copy_n(bytes.begin(), address.size(), address.begin());
        endpoint = {asio::ip::address_v6(address), hostPort};
    }
    return concretePeerEndpoint(endpoint) ? std::optional(endpoint) : std::nullopt;
}

void loadClientCertificateSubject(SSL* ssl, std::pmr::string& output) {
    output.clear();
    const auto certificate =
        std::unique_ptr<X509, decltype(&X509_free)>(SSL_get1_peer_certificate(ssl), X509_free);
    if (!certificate) {
        return;
    }
    X509_NAME* const subject = X509_get_subject_name(certificate.get());
    const auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), BIO_free);
    if (subject == nullptr || !bio ||
        X509_NAME_print_ex(bio.get(), subject, 0, XN_FLAG_RFC2253) < 0) {
        return;
    }
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio.get(), &data);
    if (data != nullptr && size > 0) {
        output.assign(data, static_cast<std::size_t>(size));
    }
}

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

Http3QuicServerTransport::Http3QuicServerTransport(
    Http3QuicTlsContext& tls, Http3QuicDatagramBridge& bridge, Http3QuicServerTransportConfig config)
    : ownerThread_(std::this_thread::get_id()),
      bridge_(bridge),
      maxActive_(config.maxActiveConnections),
      maxLifetimePeerStreams_(checkedLifetimePeerStreamLimit(config.maxLifetimePeerStreams)),
      handshakeTimeout_(checkedHandshakeTimeout(config.handshakeTimeout)),
      idleTimeoutMilliseconds_(checkedIdleTimeout(config.idleTimeout)),
      pendingInitialPeerPaths_(&connectionResource_),
      connections_(&connectionResource_) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    (void)tls;
    (void)bridge;
    throw std::runtime_error("QUIC server transport requires OpenSSL 3.6 or newer");
#else
    SslOwner domain(SSL_new_domain(tls.defaultContext(), SSL_DOMAIN_FLAG_SINGLE_THREAD));
    if (domain.get() == nullptr) {
        throw std::runtime_error("failed to create OpenSSL QUIC domain");
    }
    SslOwner listener(SSL_new_listener_from(domain.get(), 0));
    if (listener.get() == nullptr) {
        throw std::runtime_error("failed to create OpenSSL QUIC listener");
    }
#ifdef SSL_VALUE_QUIC_MAX_PENDING_CONNS
    // The public queue cap was backported to OpenSSL 3.6.4. Install it before
    // attaching input, without a pending callback: rejecting from that callback
    // can deadlock inside OpenSSL. Zero would disable the limit entirely.
    static_assert(kMaxPendingConnections > 0);
    std::uint64_t pendingLimit{};
    if (SSL_set_generic_value_uint(listener.get(), SSL_VALUE_QUIC_MAX_PENDING_CONNS,
            kMaxPendingConnections) != 1 ||
        SSL_get_generic_value_uint(listener.get(), SSL_VALUE_QUIC_MAX_PENDING_CONNS,
            &pendingLimit) != 1 ||
        pendingLimit != kMaxPendingConnections) {
        throw std::runtime_error("failed to bound OpenSSL QUIC pending connection queue");
    }
#else
    // Without this cap, a listener with no worker admission credits could grow
    // its pending connection population without a bound. Fail closed.
    throw std::runtime_error("QUIC server requires an OpenSSL pending connection limit");
#endif
    BIO* const bio = bridge.releaseSslBio();
    if (bio == nullptr) {
        throw std::runtime_error("QUIC datagram bridge has no SSL-side BIO");
    }
    // SSL_set_bio takes ownership, including when later listener setup fails.
    SSL_set_bio(listener.get(), bio, bio);
    if (SSL_set_blocking_mode(listener.get(), 0) != 1) {
        throw std::runtime_error("failed to configure non-blocking QUIC listener");
    }
    if (SSL_listen(listener.get()) != 1) {
        throw std::runtime_error("failed to start OpenSSL QUIC listener");
    }
    domain_ = domain.release();
    listener_ = listener.release();
#endif
}

Http3QuicServerTransport::~Http3QuicServerTransport() {
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
    // Accepted connections depend on the domain; the listener owns the bridge BIO.
    for (auto& [id, connection] : connections_) {
        (void)id;
        connection.streams.close();
        SSL_free(connection.ssl);
    }
    connections_.clear();
    SSL_free(listener_);
    SSL_free(domain_);
}

void Http3QuicServerTransport::requireOwnerThread() const {
    if (std::this_thread::get_id() != ownerThread_) {
        throw std::logic_error("QUIC server transport used outside its owner thread");
    }
}

Http3QuicServerTransport::EventResult Http3QuicServerTransport::handleEvents() {
    requireOwnerThread();
    (void)retireExpiredHandshakes(Clock::now());
    ErrorQueueScope errors;
    const int result = SSL_handle_events(domain_);
    EventResult eventResult = EventResult::kHandled;
    if (result == 1) {
        errors.discard();
    } else {
        const int sslError = SSL_get_error(domain_, result);
        errors.discard();
        if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE ||
            sslError == SSL_ERROR_WANT_ACCEPT) {
            eventResult = EventResult::kNonFatal;
        } else {
            return EventResult::kFatal;
        }
    }
    const auto pendingCount = SSL_get_accept_connection_queue_len(listener_);
    if (pendingCount < pendingInitialPeerPaths_.size()) {
        const auto removed = pendingInitialPeerPaths_.size() - pendingCount;
        for (std::size_t i = 0; i < removed; ++i) {
            pendingInitialPeerPaths_.pop_front();
        }
    } else if (pendingCount > pendingInitialPeerPaths_.size() &&
               bridge_.injectionGeneration() != lastMappedInjectionGeneration_) {
        const auto added = pendingCount - pendingInitialPeerPaths_.size();
        for (std::size_t i = 0; i < added; ++i) {
            pendingInitialPeerPaths_.push_back(bridge_.lastInjectedPeer());
        }
        lastMappedInjectionGeneration_ = bridge_.injectionGeneration();
    }
    for (auto& [id, connection] : connections_) {
        (void)id;
        if (!connection.closeErrorCode ||
            connection.closeStatus != ConnectionCloseStatus::kPending) {
            continue;
        }
        const auto close = driveConnectionClose(connection);
        if (close.status == ConnectionCloseStatus::kFailure && !connection.gracefulClose) {
            return EventResult::kFatal;
        }
    }
    return eventResult;
}

Http3QuicServerTransport::AcceptedBatch Http3QuicServerTransport::acceptConnections(
    std::size_t availableConnectionCredits) {
    requireOwnerThread();
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    (void)availableConnectionCredits;
    throw std::runtime_error("QUIC server transport requires OpenSSL 3.6 or newer");
#else
    AcceptedBatch accepted;
    const std::size_t activeConnections = connections_.size();
    if (availableConnectionCredits == 0 || activeConnections >= maxActive_) {
        return accepted;
    }
    const std::size_t acceptLimit = std::min({availableConnectionCredits,
        kAcceptBatchLimit, maxActive_ - activeConnections});
    try {
        for (std::size_t attempt = 0; attempt < acceptLimit; ++attempt) {
            if (SSL_get_accept_connection_queue_len(listener_) == 0) {
                break;
            }
            ErrorQueueScope errors;
            SslOwner connection(SSL_accept_connection(listener_, SSL_ACCEPT_CONNECTION_NO_BLOCK));
            if (connection.get() == nullptr) {
                const int sslError = SSL_get_error(listener_, 0);
                errors.discard();
                if (sslError == SSL_ERROR_WANT_ACCEPT || sslError == SSL_ERROR_WANT_READ ||
                    sslError == SSL_ERROR_WANT_WRITE) {
                    break;
                }
                throw std::runtime_error("failed to accept OpenSSL QUIC connection");
            }
            errors.discard();
            if (nextConnectionId_ == 0) {
                throw std::overflow_error("QUIC connection generation exhausted");
            }
            std::uint64_t requestedIdleTimeout{};
            const bool idleAlreadyConfigured =
                SSL_get_feature_request_uint(connection.get(), SSL_VALUE_QUIC_IDLE_TIMEOUT,
                    &requestedIdleTimeout) == 1 &&
                requestedIdleTimeout == idleTimeoutMilliseconds_;
            if ((!idleAlreadyConfigured &&
                    SSL_set_feature_request_uint(connection.get(),
                        SSL_VALUE_QUIC_IDLE_TIMEOUT, idleTimeoutMilliseconds_) != 1) ||
                SSL_set_incoming_stream_policy(connection.get(),
                    SSL_INCOMING_STREAM_POLICY_ACCEPT, 0) != 1) {
                // OpenSSL may complete a queued child's transport-parameter
                // flight before application admission is available. Such a
                // child can no longer honor this listener's idle-timeout
                // contract, so reject it instead of admitting it with a
                // silently different value. Production accepts new children
                // after every domain drive; this path applies to overflow that
                // remained in OpenSSL's bounded pending queue.
                errors.discard();
                if (!pendingInitialPeerPaths_.empty()) {
                    pendingInitialPeerPaths_.pop_front();
                }
                continue;
            }
            errors.discard();
            // Preserve both address sources until handshake completion: the Initial
            // datagram's observed source is the fallback, while a concrete accepted-child
            // BIO endpoint takes precedence when OpenSSL exposes one.
            const auto childPeer = peerEndpoint(connection.get());
            std::optional<asio::ip::udp::endpoint> initialPeer;
            if (!pendingInitialPeerPaths_.empty()) {
                initialPeer = peerEndpoint(pendingInitialPeerPaths_.front());
                pendingInitialPeerPaths_.pop_front();
            }
            const ConnectionId id = nextConnectionId_++;
            const auto now = Clock::now();
            const auto remaining = Clock::time_point::max() - now;
            const auto deadline = now + std::min(handshakeTimeout_, remaining);
            connections_.try_emplace(id, connection.get(), &connectionResource_, deadline,
                initialPeer ? initialPeer->address().to_string() : std::string_view{},
                initialPeer ? initialPeer->port() : 0,
                childPeer ? childPeer->address().to_string() : std::string_view{},
                childPeer ? childPeer->port() : 0, maxLifetimePeerStreams_);
            (void)connection.release();
            accepted.ids[accepted.size++] = id;
        }
    } catch (...) {
        for (std::size_t index = 0; index < accepted.size; ++index) {
            (void)retireConnectionLocally(accepted.ids[index]);
        }
        throw;
    }
    return accepted;
#endif
}

std::optional<Http3QuicServerTransport::ConnectionInfo>
Http3QuicServerTransport::connectionInfo(ConnectionId id) const {
    requireOwnerThread();
    const auto found = connections_.find(id);
    if (found == connections_.end()) {
        return std::nullopt;
    }
    auto& connection = found->second;
    ConnectionInfo info{
        .id = id,
        .handshakeComplete = SSL_is_init_finished(connection.ssl) == 1,
    };
    if (info.handshakeComplete) {
        // Completing the QUIC/TLS handshake confirms that this connection owns
        // the Initial-observed path. It is not a claim about a migrated current
        // path or the peer's identity. Use a concrete accepted-child BIO endpoint
        // when available; otherwise retain the source observed on its Initial.
        if (const auto childPeer = peerEndpoint(connection.ssl)) {
            connection.acceptedChildPeerAddress = childPeer->address().to_string();
            connection.acceptedChildPeerPort = childPeer->port();
        }
        if (connection.acceptedChildPeerPort != 0) {
            info.remoteAddress = connection.acceptedChildPeerAddress;
            info.remotePort = connection.acceptedChildPeerPort;
        } else {
            info.remoteAddress = connection.initialObservedPeerAddress;
            info.remotePort = connection.initialObservedPeerPort;
        }
        if (!connection.certificateSubjectLoaded) {
            loadClientCertificateSubject(connection.ssl, connection.clientCertificateSubject);
            connection.certificateSubjectLoaded = true;
        }
        info.clientCertificateSubject = connection.clientCertificateSubject;
        if (SSL_get_feature_negotiated_uint(connection.ssl,
                SSL_VALUE_QUIC_IDLE_TIMEOUT,
                &info.negotiatedIdleTimeoutMilliseconds) != 1) {
            info.negotiatedIdleTimeoutMilliseconds = 0;
        }
        const unsigned char* alpn = nullptr;
        unsigned int alpnSize = 0;
        SSL_get0_alpn_selected(connection.ssl, &alpn, &alpnSize);
        info.h3Negotiated = alpnSize == 2 && alpn != nullptr && alpn[0] == 'h' && alpn[1] == '3';
    }
    SSL_CONN_CLOSE_INFO close{};
    ErrorQueueScope errors;
    if (SSL_get_conn_close_info(connection.ssl, &close, sizeof(close)) == 1) {
        info.terminated = true;
        info.closeErrorCode = close.error_code;
        info.closeFlags = close.flags;
    }
    return info;
}

Http3QuicServerTransport::AcceptedStreams
Http3QuicServerTransport::acceptStreams(ConnectionId id) {
    requireOwnerThread();
    const auto connection = connections_.find(id);
    if (connection == connections_.end()) {
        AcceptedStreams result;
        result.error = Error::kNoConnection;
        return result;
    }
    if (connection->second.closeErrorCode) {
        AcceptedStreams result;
        result.error = Error::kClosed;
        return result;
    }
    SSL* const parent = connection->second.ssl;
    if (SSL_is_init_finished(parent) != 1) {
        AcceptedStreams result;
        result.error = Error::kHandshakePending;
        return result;
    }
    const unsigned char* alpn = nullptr;
    unsigned int alpnSize = 0;
    SSL_get0_alpn_selected(parent, &alpn, &alpnSize);
    if (alpn == nullptr || alpnSize != 2 || alpn[0] != 'h' || alpn[1] != '3') {
        (void)retireConnectionLocally(id);
        AcceptedStreams result;
        result.error = Error::kAlpnMismatch;
        return result;
    }
    AcceptedStreams accepted = connection->second.streams.accept();
    if (accepted.error == Error::kInvalidStreamId || accepted.error == Error::kFatal) {
        const Error failure = accepted.error;
        (void)retireConnectionLocally(id);
        accepted = {};
        accepted.error = failure;
    }
    return accepted;
}

Http3QuicServerTransport::OpenStream
Http3QuicServerTransport::openLocalUnidirectionalStream(ConnectionId id) {
    requireOwnerThread();
    const auto connection = connections_.find(id);
    if (connection == connections_.end()) {
        return OpenStream{.error = Error::kNoConnection};
    }
    if (connection->second.closeErrorCode) {
        return OpenStream{.error = Error::kClosed};
    }
    SSL* const parent = connection->second.ssl;
    if (SSL_is_init_finished(parent) != 1) {
        return OpenStream{.error = Error::kHandshakePending};
    }
    const unsigned char* alpn = nullptr;
    unsigned int alpnSize = 0;
    SSL_get0_alpn_selected(parent, &alpn, &alpnSize);
    if (alpn == nullptr || alpnSize != 2 || alpn[0] != 'h' || alpn[1] != '3') {
        return OpenStream{.error = Error::kAlpnMismatch};
    }
    try {
        return connection->second.streams.createUnidirectional();
    } catch (...) {
        // The stream ID cannot be rolled back after SSL_new_stream() succeeds.
        (void)retireConnectionLocally(id);
        return OpenStream{.error = Error::kFatal};
    }
}

Http3QuicServerTransport::StreamWrite Http3QuicServerTransport::writeStream(
    ConnectionId id, StreamId streamId, std::span<const char> input) {
    requireOwnerThread();
    const auto connection = connections_.find(id);
    if (connection == connections_.end()) {
        return StreamWrite{.status = StreamWrite::Status::kNoConnection};
    }
    if (connection->second.closeErrorCode) {
        return StreamWrite{.status = StreamWrite::Status::kClosed};
    }
    return connection->second.streams.write(streamId, input);
}

Http3QuicServerTransport::Error Http3QuicServerTransport::finishStream(
    ConnectionId id, StreamId streamId) {
    requireOwnerThread();
    const auto connection = connections_.find(id);
    if (connection == connections_.end()) {
        return Error::kNoConnection;
    }
    if (connection->second.closeErrorCode) {
        return Error::kClosed;
    }
    return connection->second.streams.finish(streamId);
}

Http3QuicServerTransport::Error Http3QuicServerTransport::resetStream(
    ConnectionId id, StreamId streamId, std::uint64_t errorCode) {
    requireOwnerThread();
    const auto connection = connections_.find(id);
    if (connection == connections_.end()) {
        return Error::kNoConnection;
    }
    if (connection->second.closeErrorCode) {
        return Error::kClosed;
    }
    return connection->second.streams.reset(streamId, errorCode);
}

Http3QuicServerTransport::StreamTermination
Http3QuicServerTransport::terminateBidirectionalStream(
    ConnectionId id, StreamId streamId, std::uint64_t sendErrorCode) {
    requireOwnerThread();
    const auto connection = connections_.find(id);
    if (connection == connections_.end()) {
        return {.send = Error::kNoConnection, .close = Error::kNoConnection};
    }
    if (connection->second.closeErrorCode) {
        return {.send = Error::kClosed, .close = Error::kClosed};
    }
    return connection->second.streams.terminateBidirectional(streamId, sendErrorCode);
}

Http3QuicServerTransport::StreamRead Http3QuicServerTransport::readStream(
    ConnectionId id, StreamId streamId, std::span<char> output) {
    requireOwnerThread();
    const auto connection = connections_.find(id);
    if (connection == connections_.end()) {
        return StreamRead{.status = StreamRead::Status::kNoConnection};
    }
    if (connection->second.closeErrorCode) {
        return StreamRead{.status = StreamRead::Status::kClosed};
    }
    return connection->second.streams.read(streamId, output);
}

Http3QuicServerTransport::Error
Http3QuicServerTransport::retireCompletedBidirectionalStream(
    ConnectionId id, StreamId streamId) {
    requireOwnerThread();
    const auto connection = connections_.find(id);
    if (connection == connections_.end()) {
        return Error::kNoConnection;
    }
    if (connection->second.closeErrorCode) {
        return Error::kClosed;
    }
    return connection->second.streams.retireCompletedBidirectional(streamId);
}

Http3QuicServerTransport::Error Http3QuicServerTransport::closeStream(
    ConnectionId id, StreamId streamId) {
    requireOwnerThread();
    const auto connection = connections_.find(id);
    if (connection == connections_.end()) {
        return Error::kNoConnection;
    }
    if (connection->second.closeErrorCode) {
        return Error::kClosed;
    }
    return connection->second.streams.close(streamId);
}

Http3QuicServerTransport::ConnectionCloseResult
Http3QuicServerTransport::requestConnectionClose(
    ConnectionId id, Http3ConnectionErrorCode errorCode) {
    requireOwnerThread();
    return requestConnectionClose(id, errorCode, false);
}

Http3QuicServerTransport::ConnectionCloseResult
Http3QuicServerTransport::requestGracefulConnectionClose(ConnectionId id) {
    requireOwnerThread();
    return requestConnectionClose(id, Http3ConnectionErrorCode::kNoError, true);
}

Http3QuicServerTransport::ConnectionCloseResult
Http3QuicServerTransport::requestConnectionClose(
    ConnectionId id, Http3ConnectionErrorCode errorCode, bool graceful) {
    const auto found = connections_.find(id);
    if (found == connections_.end()) {
        return {.status = ConnectionCloseStatus::kNoConnection};
    }
    auto& connection = found->second;
    const bool streamsRetired = connection.streams.size() == 0;
    if (connection.closeErrorCode) {
        if (*connection.closeErrorCode != errorCode || connection.gracefulClose != graceful) {
            return {.status = ConnectionCloseStatus::kConflict,
                .allStreamsRetired = streamsRetired};
        }
        if (connection.closeStatus != ConnectionCloseStatus::kPending) {
            return {.status = connection.closeStatus,
                .allStreamsRetired = streamsRetired};
        }
        return driveConnectionClose(connection);
    }
    if (static_cast<std::uint64_t>(errorCode) > kHttp3VarIntMax ||
        (graceful && errorCode != Http3ConnectionErrorCode::kNoError)) {
        return {.status = ConnectionCloseStatus::kFailure,
            .allStreamsRetired = streamsRetired};
    }

    connection.closeErrorCode = errorCode;
    connection.gracefulClose = graceful;
    if (!graceful) {
        connection.streams.close();
    }
    return driveConnectionClose(connection);
}

Http3QuicServerTransport::ConnectionCloseResult
Http3QuicServerTransport::driveConnectionClose(Connection& connection) {
    const bool graceful = connection.gracefulClose;
    if (!connection.closeErrorCode || (!graceful && connection.streams.size() != 0)) {
        connection.closeStatus = ConnectionCloseStatus::kFailure;
        return {.status = connection.closeStatus,
            .allStreamsRetired = connection.streams.size() == 0};
    }
    if (connection.closeStatus != ConnectionCloseStatus::kPending) {
        return {.status = connection.closeStatus,
            .allStreamsRetired = connection.streams.size() == 0};
    }
    SSL_SHUTDOWN_EX_ARGS args{};
    args.quic_error_code = static_cast<std::uint64_t>(*connection.closeErrorCode);
    std::uint64_t flags = SSL_SHUTDOWN_FLAG_NO_BLOCK;
    if (!graceful) {
        flags |= SSL_SHUTDOWN_FLAG_RAPID | SSL_SHUTDOWN_FLAG_NO_STREAM_FLUSH;
    }
    ErrorQueueScope errors;
    const int result = SSL_shutdown_ex(connection.ssl, flags, &args, sizeof(args));
    const auto pending = [&connection]() {
        return ConnectionCloseResult{
            .status = ConnectionCloseStatus::kPending,
            .allStreamsRetired = connection.streams.size() == 0};
    };
    if (result == 0) {
        errors.discard();
        return pending();
    }
    if (result == 1) {
        errors.discard();
        if (graceful) {
            // OpenSSL may flush live stream state during graceful shutdown, so
            // retire its SSL wrappers only after SSL_shutdown_ex completes.
            connection.streams.close();
        }
        connection.closeStatus = ConnectionCloseStatus::kCompleted;
        return {.status = connection.closeStatus, .allStreamsRetired = true};
    }

    const int sslError = SSL_get_error(connection.ssl, result);
    errors.discard();
    if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE ||
        sslError == SSL_ERROR_WANT_ACCEPT) {
        return pending();
    }
    connection.closeStatus = ConnectionCloseStatus::kFailure;
    return {.status = connection.closeStatus,
        .allStreamsRetired = connection.streams.size() == 0};
}

Http3QuicServerTransport::Error
Http3QuicServerTransport::retireConnectionLocally(ConnectionId id) {
    requireOwnerThread();
    const auto found = connections_.find(id);
    if (found == connections_.end()) {
        return Error::kNoConnection;
    }
    found->second.streams.close();
    SSL_free(found->second.ssl);
    connections_.erase(found);
    return Error::kNone;
}

std::size_t Http3QuicServerTransport::retireExpiredHandshakes(Clock::time_point now) {
    requireOwnerThread();
    std::size_t retired{};
    for (auto it = connections_.begin(); it != connections_.end();) {
        auto& connection = it->second;
        if (SSL_is_init_finished(connection.ssl) == 1 || now < connection.handshakeDeadline) {
            ++it;
            continue;
        }
        connection.streams.close();
        SSL_free(connection.ssl);
        it = connections_.erase(it);
        ++retired;
    }
    return retired;
}

std::optional<Http3QuicServerTransport::Duration>
Http3QuicServerTransport::eventTimeout() {
    requireOwnerThread();
    timeval timeout{};
    int infinite = 0;
    if (SSL_get_event_timeout(domain_, &timeout, &infinite) != 1) {
        throw std::runtime_error("failed to read QUIC domain event timeout");
    }
    const auto now = Clock::now();
    std::optional<Duration> handshakeRemaining;
    for (const auto& [id, connection] : connections_) {
        (void)id;
        if (SSL_is_init_finished(connection.ssl) == 1) {
            continue;
        }
        const auto remaining = connection.handshakeDeadline <= now
                                   ? Duration::zero()
                                   : connection.handshakeDeadline - now;
        if (!handshakeRemaining || remaining < *handshakeRemaining) {
            handshakeRemaining = remaining;
        }
    }
    if (infinite != 0) {
        return handshakeRemaining;
    }
    if (timeout.tv_sec < 0 || timeout.tv_usec < 0) {
        throw std::runtime_error("QUIC domain returned a negative event timeout");
    }
    using Rep = Duration::rep;
    constexpr auto maxSeconds = std::numeric_limits<Rep>::max() /
                                std::chrono::duration_cast<Duration>(std::chrono::seconds(1)).count();
    if (static_cast<unsigned long long>(timeout.tv_sec) >
        static_cast<unsigned long long>(maxSeconds)) {
        return handshakeRemaining.value_or(Duration::max());
    }
    const auto seconds = std::chrono::duration_cast<Duration>(
        std::chrono::seconds(timeout.tv_sec));
    const auto micros = std::chrono::duration_cast<Duration>(
        std::chrono::microseconds(timeout.tv_usec));
    if (micros > Duration::zero() && seconds > Duration::max() - micros) {
        return handshakeRemaining.value_or(Duration::max());
    }
    const auto quicRemaining = std::max(Duration::zero(), seconds + micros);
    return handshakeRemaining ? std::min(quicRemaining, *handshakeRemaining) : quicRemaining;
}

}  // namespace ruvia::detail
