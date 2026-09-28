#include "ruvia/web/detail/http3/Http3QuicStreamSet.h"

#include <stdexcept>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "ruvia/http/Http3VarInt.h"

namespace ruvia::detail {
namespace {

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

bool closedWriteState(int state) noexcept {
    return state == SSL_STREAM_STATE_FINISHED || state == SSL_STREAM_STATE_RESET_LOCAL ||
           state == SSL_STREAM_STATE_RESET_REMOTE || state == SSL_STREAM_STATE_CONN_CLOSED;
}

}  // namespace

Http3QuicStreamSet::Http3QuicStreamSet(SSL* parent, std::pmr::memory_resource* resource,
    std::size_t maxStreams, std::size_t maxLifetimePeerStreams)
    : parent_(parent),
      maxStreams_(maxStreams),
      maxLifetimePeerStreams_(maxLifetimePeerStreams),
      streams_(resource != nullptr ? resource : std::pmr::get_default_resource()) {
    if (parent == nullptr || resource == nullptr || maxStreams == 0 ||
        maxLifetimePeerStreams == 0) {
        throw std::invalid_argument(
            "QUIC stream set requires a parent, resource and positive stream limits");
    }
}

Http3QuicStreamSet::~Http3QuicStreamSet() {
    close();
}

std::size_t Http3QuicStreamSet::size() const noexcept {
    return streams_.size();
}

void Http3QuicStreamSet::close() noexcept {
    for (auto& [id, stream] : streams_) {
        (void)id;
        SSL_free(stream.ssl);
    }
    streams_.clear();
}

Http3QuicStreamSet::AcceptedStreams Http3QuicStreamSet::accept() {
    AcceptedStreams result;
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    result.error = Error::kFatal;
#else
    if (peerLimitExceeded_) {
        result.error = Error::kConnectionPeerStreamLimit;
        return result;
    }
    try {
        while (result.size < kAcceptBatchLimit && SSL_get_accept_stream_queue_len(parent_) != 0) {
            if (acceptedPeerStreams_ >= maxLifetimePeerStreams_) {
                peerLimitExceeded_ = true;
                result.error = Error::kConnectionPeerStreamLimit;
                return result;
            }
            if (streams_.size() >= maxStreams_) {
                result.error = Error::kStreamLimitRetry;
                return result;
            }
            ErrorQueueScope errors;
            SSL* const ssl = SSL_accept_stream(parent_, SSL_ACCEPT_STREAM_NO_BLOCK);
            if (ssl == nullptr) {
                const int error = SSL_get_error(parent_, 0);
                errors.discard();
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ||
                    error == SSL_ERROR_WANT_ACCEPT) {
                    break;
                }
                result.error = Error::kFatal;
                return result;
            }
            errors.discard();
            const auto id = SSL_get_stream_id(ssl);
            if (id == UINT64_MAX || streams_.contains(id)) {
                SSL_free(ssl);
                result.error = Error::kInvalidStreamId;
                return result;
            }
            const int type = SSL_get_stream_type(ssl);
            if (type == SSL_STREAM_TYPE_NONE) {
                SSL_free(ssl);
                result.error = Error::kFatal;
                return result;
            }
            try {
                streams_.emplace(id, StreamEntry{.ssl = ssl,
                                         .readable = (type & SSL_STREAM_TYPE_READ) != 0,
                                         .writeable = (type & SSL_STREAM_TYPE_WRITE) != 0});
            } catch (...) {
                SSL_free(ssl);
                throw;
            }
            ++acceptedPeerStreams_;
            result.streams[result.size++] = StreamInfo{id,
                (type & SSL_STREAM_TYPE_READ) != 0, (type & SSL_STREAM_TYPE_WRITE) != 0};
        }
    } catch (...) {
        for (std::size_t i = 0; i < result.size; ++i) {
            (void)close(result.streams[i].id);
        }
        result = {};
        result.error = Error::kFatal;
    }
#endif
    return result;
}

Http3QuicStreamSet::OpenStream Http3QuicStreamSet::createUnidirectional() {
    return create(SSL_STREAM_FLAG_UNI | SSL_STREAM_FLAG_NO_BLOCK);
}

Http3QuicStreamSet::OpenStream Http3QuicStreamSet::createBidirectional() {
    return create(SSL_STREAM_FLAG_NO_BLOCK);
}

Http3QuicStreamSet::OpenStream Http3QuicStreamSet::create(int flags) {
    OpenStream result;
    if (streams_.size() >= maxStreams_) {
        result.error = Error::kStreamLimitRetry;
        return result;
    }
    ErrorQueueScope errors;
    SSL* const ssl = SSL_new_stream(parent_, flags);
    if (ssl == nullptr) {
        const unsigned long code = ERR_peek_last_error();
        result.error = ERR_GET_REASON(code) == SSL_R_STREAM_COUNT_LIMITED
                           ? Error::kStreamLimitRetry
                           : Error::kFatal;
        errors.discard();
        return result;
    }
    errors.discard();
    const auto id = SSL_get_stream_id(ssl);
    if (id == UINT64_MAX || streams_.contains(id)) {
        SSL_free(ssl);
        result.error = Error::kFatal;
        return result;
    }
    const int type = SSL_get_stream_type(ssl);
    if (type == SSL_STREAM_TYPE_NONE) {
        SSL_free(ssl);
        result.error = Error::kFatal;
        return result;
    }
    try {
        streams_.emplace(id, StreamEntry{.ssl = ssl,
                                 .readable = (type & SSL_STREAM_TYPE_READ) != 0,
                                 .writeable = (type & SSL_STREAM_TYPE_WRITE) != 0});
    } catch (...) {
        // Release the SSL owner. The consumed QUIC stream ID cannot be rolled
        // back, so the caller must terminate this connection on failure.
        SSL_free(ssl);
        throw;
    }
    result.id = id;
    return result;
}

Http3QuicStreamSet::StreamWrite Http3QuicStreamSet::write(
    StreamId id, std::span<const char> input) {
    StreamWrite result;
    const auto found = streams_.find(id);
    if (found == streams_.end()) {
        result.status = StreamWrite::Status::kNoStream;
        return result;
    }
    auto& stream = found->second;
    if (!stream.writeable) {
        result.status = StreamWrite::Status::kWrongDirection;
        return result;
    }
    if (stream.sendFinished) {
        result.status = StreamWrite::Status::kClosed;
        return result;
    }
    if (stream.pendingData != nullptr &&
        (stream.pendingData != input.data() || stream.pendingSize != input.size())) {
        result.status = StreamWrite::Status::kRetryMismatch;
        return result;
    }
    if (closedWriteState(SSL_get_stream_write_state(stream.ssl))) {
        result.status = StreamWrite::Status::kClosed;
        return result;
    }
    if (input.empty()) {
        result.status = StreamWrite::Status::kAccepted;
        return result;
    }

    std::size_t written = 0;
    ErrorQueueScope errors;
    if (SSL_write_ex(stream.ssl, input.data(), input.size(), &written) == 1) {
        errors.discard();
        stream.pendingData = nullptr;
        stream.pendingSize = 0;
        result.status = StreamWrite::Status::kAccepted;
        result.bytes = written;
        return result;
    }
    const int sslError = SSL_get_error(stream.ssl, 0);
    const int state = SSL_get_stream_write_state(stream.ssl);
    errors.discard();
    if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE ||
        sslError == SSL_ERROR_WANT_ACCEPT) {
        stream.pendingData = input.data();
        stream.pendingSize = input.size();
        result.status = StreamWrite::Status::kWouldBlock;
    } else if (state == SSL_STREAM_STATE_WRONG_DIR) {
        result.status = StreamWrite::Status::kWrongDirection;
    } else if (closedWriteState(state)) {
        result.status = StreamWrite::Status::kClosed;
    } else {
        result.status = StreamWrite::Status::kFatal;
    }
    return result;
}

Http3QuicStreamSet::Error Http3QuicStreamSet::finish(StreamId id) {
    const auto found = streams_.find(id);
    if (found == streams_.end()) {
        return Error::kNoStream;
    }
    auto& stream = found->second;
    if (!stream.writeable) {
        return Error::kWrongDirection;
    }
    if (stream.sendFinished) {
        return Error::kClosed;
    }
    if (stream.pendingData != nullptr) {
        return Error::kPendingWrite;
    }
    if (closedWriteState(SSL_get_stream_write_state(stream.ssl))) {
        return Error::kClosed;
    }
    ErrorQueueScope errors;
    if (SSL_stream_conclude(stream.ssl, 0) == 1) {
        errors.discard();
        stream.sendFinished = true;
        stream.sendFinAccepted = true;
        return Error::kNone;
    }
    errors.discard();
    return Error::kFatal;
}

Http3QuicStreamSet::Error Http3QuicStreamSet::writeHealth(StreamId id) const {
    const auto found = streams_.find(id);
    if (found == streams_.end()) {
        return Error::kNoStream;
    }
    if (!found->second.writeable) {
        return Error::kWrongDirection;
    }
    ErrorQueueScope errors;
    const int state = SSL_get_stream_write_state(found->second.ssl);
    errors.discard();
    if (state == SSL_STREAM_STATE_OK) {
        return Error::kNone;
    }
    return closedWriteState(state) ? Error::kClosed : Error::kFatal;
}

Http3QuicStreamSet::Error Http3QuicStreamSet::reset(StreamId id, std::uint64_t errorCode) {
    const auto found = streams_.find(id);
    if (found == streams_.end()) {
        return Error::kNoStream;
    }
    auto& stream = found->second;
    if (!stream.writeable) {
        return Error::kWrongDirection;
    }
    if (errorCode > kHttp3VarIntMax) {
        return Error::kInvalidErrorCode;
    }
    if (stream.sendFinished) {
        return Error::kClosed;
    }
    SSL_STREAM_RESET_ARGS args{.quic_error_code = errorCode};
    ErrorQueueScope errors;
    if (SSL_stream_reset(stream.ssl, &args, sizeof(args)) == 1) {
        errors.discard();
        stream.pendingData = nullptr;
        stream.pendingSize = 0;
        stream.sendFinished = true;
        return Error::kNone;
    }
    // State is meaningful only when the operation left it in a terminal state;
    // do not translate a failed reset using stale WANT/error state.
    const int state = SSL_get_stream_write_state(stream.ssl);
    errors.discard();
    return closedWriteState(state) ? Error::kClosed : Error::kFatal;
}

Http3QuicStreamSet::StreamTermination Http3QuicStreamSet::terminateBidirectional(
    StreamId id, std::uint64_t sendErrorCode) {
    const auto found = streams_.find(id);
    if (found == streams_.end()) {
        return {};
    }
    if (!found->second.readable || !found->second.writeable) {
        return {.send = Error::kWrongDirection, .close = Error::kWrongDirection};
    }
    if (sendErrorCode > kHttp3VarIntMax) {
        return {.send = Error::kInvalidErrorCode, .close = Error::kInvalidErrorCode};
    }
    const auto send = found->second.sendFinished ? Error::kClosed : reset(id, sendErrorCode);
    return {.send = send, .close = close(id)};
}

Http3QuicStreamSet::StreamRead Http3QuicStreamSet::read(
    StreamId id, std::span<char> output) {
    StreamRead result;
    const auto found = streams_.find(id);
    if (found == streams_.end()) {
        result.status = StreamRead::Status::kNoStream;
        return result;
    }
    auto& stream = found->second;
    if (!stream.readable) {
        result.status = StreamRead::Status::kWrongDirection;
        return result;
    }
    if (stream.receiveTerminal) {
        result.status = stream.receiveReset ? StreamRead::Status::kReset : StreamRead::Status::kFin;
        result.peerResetErrorCode = stream.peerResetErrorCode;
        return result;
    }
    std::size_t readSize = 0;
    ErrorQueueScope errors;
    if (SSL_read_ex(stream.ssl, output.data(), output.size(), &readSize) == 1) {
        errors.discard();
        result.status = readSize == 0 ? StreamRead::Status::kWouldBlock : StreamRead::Status::kData;
        result.size = readSize;
        return result;
    }
    const int sslError = SSL_get_error(stream.ssl, 0);
    const int state = SSL_get_stream_read_state(stream.ssl);
    if (state == SSL_STREAM_STATE_CONN_CLOSED) {
        result.status = StreamRead::Status::kClosed;
    } else if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE ||
               sslError == SSL_ERROR_WANT_ACCEPT) {
        result.status = StreamRead::Status::kWouldBlock;
    } else if (state == SSL_STREAM_STATE_RESET_REMOTE || state == SSL_STREAM_STATE_RESET_LOCAL) {
        result.status = StreamRead::Status::kReset;
        if (state == SSL_STREAM_STATE_RESET_REMOTE) {
            std::uint64_t code{};
            stream.receiveTerminal = true;
            stream.receiveReset = true;
            if (SSL_get_stream_read_error_code(stream.ssl, &code) == 1) {
                stream.peerResetErrorCode = code;
                result.peerResetErrorCode = code;
            }
        }
    } else if (sslError == SSL_ERROR_ZERO_RETURN) {
        stream.receiveTerminal = true;
        result.status = StreamRead::Status::kFin;
    } else {
        result.status = StreamRead::Status::kFatal;
    }
    errors.discard();
    return result;
}

Http3QuicStreamSet::Error Http3QuicStreamSet::retireCompletedBidirectional(StreamId id) {
    const auto found = streams_.find(id);
    if (found == streams_.end()) {
        return Error::kNoStream;
    }
    const auto& stream = found->second;
    if (!stream.readable || !stream.writeable) {
        return Error::kWrongDirection;
    }
    if (!stream.sendFinAccepted || !stream.receiveTerminal) {
        return Error::kWouldBlock;
    }
    if (stream.pendingData != nullptr) {
        return Error::kPendingWrite;
    }
    // Both directions are complete, so SSL_free cannot synthesize STOP_SENDING.
    SSL_free(stream.ssl);
    streams_.erase(found);
    return Error::kNone;
}

Http3QuicStreamSet::Error Http3QuicStreamSet::close(StreamId id) {
    const auto found = streams_.find(id);
    if (found == streams_.end()) {
        return Error::kNoStream;
    }
    // In OpenSSL 3.6, dropping the SSL wrapper can schedule STOP_SENDING,
    // but the underlying QUIC stream may remain until ACK or connection close.
    // This erases only our stream owner; it is not a QUIC memory-reclamation
    // guarantee for an indefinitely reused connection.
    SSL_free(found->second.ssl);
    streams_.erase(found);
    return Error::kNone;
}

}  // namespace ruvia::detail
