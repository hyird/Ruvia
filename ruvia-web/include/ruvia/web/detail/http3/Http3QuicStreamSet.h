#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <unordered_map>

#include <openssl/types.h>

namespace ruvia::detail {

class Http3QuicServerTransport;

// Owns streams created from one parent QUIC connection. The parent SSL and the
// supplied PMR resource must outlive this object; stream SSL objects are freed
// before the parent by the owning connection.
class Http3QuicStreamSet final {
public:
    using StreamId = std::uint64_t;
    static constexpr std::size_t kMaxStreamsPerConnection = 32;
    static constexpr std::size_t kAcceptBatchLimit = 32;
    // Default policy for clients and server transports; each stream set stores
    // its own lifetime peer-admission cap. This is not a QUIC byte-memory limit.
    static constexpr std::size_t kMaxLifetimePeerStreams = 128;

    enum class Error : unsigned char {
        kNone,
        kNoConnection,
        kNoStream,
        kInvalidStreamId,
        kInvalidErrorCode,
        kHandshakePending,
        kAlpnMismatch,
        kStreamLimitRetry,
        kConnectionRequestLimit,
        kConnectionPeerStreamLimit,
        kWrongDirection,
        kPendingWrite,
        kWouldBlock,
        kClosed,
        kFatal,
    };
    struct StreamInfo final {
        StreamId id{};
        bool readable{};
        bool writeable{};
    };
    struct AcceptedStreams final {
        std::array<StreamInfo, kAcceptBatchLimit> streams{};
        std::size_t size{};
        Error error{Error::kNone};
    };
    struct OpenStream final {
        Error error{Error::kNone};
        StreamId id{};
    };
    struct StreamWrite final {
        enum class Status : unsigned char {
            kAccepted,
            kWouldBlock,
            kRetryMismatch,
            kNoConnection,
            kNoStream,
            kWrongDirection,
            kClosed,
            kFatal,
        } status{Status::kFatal};
        std::size_t bytes{};
    };
    struct StreamTermination final {
        Error send{Error::kNoStream};
        Error close{Error::kNoStream};
    };
    struct StreamRead final {
        enum class Status : unsigned char {
            kData,
            kFin,
            kWouldBlock,
            kReset,
            kClosed,
            kFatal,
            kNoConnection,
            kNoStream,
            kWrongDirection,
        } status{Status::kWouldBlock};
        std::size_t size{};
        // Only a peer RESET_STREAM supplies this value. Local STOP_SENDING
        // and connection failure must never become peer rejection evidence.
        std::optional<std::uint64_t> peerResetErrorCode{};
    };

    Http3QuicStreamSet(SSL* parent, std::pmr::memory_resource* resource,
        std::size_t maxStreams = kMaxStreamsPerConnection,
        std::size_t maxLifetimePeerStreams = kMaxLifetimePeerStreams);
    ~Http3QuicStreamSet();
    Http3QuicStreamSet(const Http3QuicStreamSet&) = delete;
    Http3QuicStreamSet& operator=(const Http3QuicStreamSet&) = delete;
    Http3QuicStreamSet(Http3QuicStreamSet&&) = delete;
    Http3QuicStreamSet& operator=(Http3QuicStreamSet&&) = delete;

    [[nodiscard]] std::size_t size() const noexcept;
    void close() noexcept;
    // kConnectionPeerStreamLimit is terminal: the owner must close the QUIC
    // connection, not retry after freeing a stream slot.
    [[nodiscard]] AcceptedStreams accept();
    [[nodiscard]] OpenStream createUnidirectional();
    [[nodiscard]] OpenStream createBidirectional();
    // After kWouldBlock, the caller owns the pending bytes and must retry the
    // same address, size and contents until accepted or the stream is closed.
    // This stream set borrows the input; it does not own its storage.
    [[nodiscard]] StreamWrite write(StreamId id, std::span<const char> input);
    [[nodiscard]] Error finish(StreamId id);
    // Local critical streams must remain writable for the whole connection,
    // even after their prefixes have been accepted and no writes are pending.
    [[nodiscard]] Error writeHealth(StreamId id) const;
    // Resets only this endpoint's sending direction; receiving may continue.
    [[nodiscard]] Error reset(StreamId id, std::uint64_t errorCode);
    // Retires both directions of a bidi stream even if sending reset fails.
    // SSL_free schedules STOP_SENDING(0) only if receive data is incomplete;
    // it neither confirms wire delivery nor immediately reclaims QUIC state.
    // A wrong-direction stream or invalid code is rejected without closing.
    [[nodiscard]] StreamTermination terminateBidirectional(
        StreamId id, std::uint64_t sendErrorCode);
    [[nodiscard]] StreamRead read(StreamId id, std::span<char> output);
    // Force-closes either direction. Incomplete reads may schedule STOP_SENDING.
    [[nodiscard]] Error close(StreamId id);

private:
    friend class Http3QuicServerTransport;

    // Server-only normal retirement: frees the SSL wrapper without scheduling
    // STOP_SENDING, but only after a successfully concluded send direction and
    // a read() that observed peer FIN or RESET after draining buffered bytes.
    [[nodiscard]] Error retireCompletedBidirectional(StreamId id);

    struct StreamEntry final {
        SSL* ssl{};
        const char* pendingData{};
        std::size_t pendingSize{};
        bool readable{};
        bool writeable{};
        bool sendFinished{};
        bool sendFinAccepted{};
        bool receiveTerminal{};
        bool receiveReset{};
        std::optional<std::uint64_t> peerResetErrorCode{};
    };
    [[nodiscard]] OpenStream create(int flags);
    SSL* parent_{};
    std::size_t maxStreams_{};
    std::size_t maxLifetimePeerStreams_{};
    std::size_t acceptedPeerStreams_{};
    bool peerLimitExceeded_{};
    std::pmr::unordered_map<StreamId, StreamEntry> streams_;
};

}  // namespace ruvia::detail
