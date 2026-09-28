#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "ruvia/web/detail/http3/Http3ClientRequestWrite.h"
#include "ruvia/web/detail/http3/Http3QuicStreamSet.h"

namespace ruvia::detail {

// One worker-affine outbound request stream. The caller must first confirm that
// the local control and QPACK stream prefixes have been accepted. This driver
// only writes the request; a separate response owner handles reads and joins.
// Keep its address stable while a write is pending, and close the QUIC stream
// before destroying this driver on cancellation, fatal error or shutdown.
class Http3ClientRequestDriver final {
public:
    using StreamId = Http3QuicStreamSet::StreamId;
    enum class Result : std::uint8_t { kBlocked,
        kConnectionDraining,
        kProgress,
        kFinished,
        kFatal };

    explicit Http3ClientRequestDriver(Http3ClientRequestWrite&& request)
        : request_(std::move(request)) {}
    Http3ClientRequestDriver(const Http3ClientRequestDriver&) = delete;
    Http3ClientRequestDriver& operator=(const Http3ClientRequestDriver&) = delete;
    Http3ClientRequestDriver(Http3ClientRequestDriver&&) = delete;
    Http3ClientRequestDriver& operator=(Http3ClientRequestDriver&&) = delete;

    // open() -> OpenStream; registerResponse(id, knownMethod) -> bool;
    // write(id, bytes) -> StreamWrite; finish(id) -> Error. Register the
    // request and its HEAD semantics before emitting its HEADERS. At most one
    // nonblocking write and one FIN attempt per tick; no task is started here.
    // After kProgress/kFinished, pump QUIC before sleeping even if the previous
    // socket pump had no traffic: SSL may only queue output during this tick.
    // kBlocked is stream credit/WANT, not necessarily a UDP-writable signal.
    template <typename Open, typename Register, typename Write, typename Finish>
    [[nodiscard]] Result drive(Open&& open, Register&& registerResponse,
        Write&& write, Finish&& finish) {
        if (failed_) {
            return Result::kFatal;
        }
        if (request_.finished()) {
            return Result::kFinished;
        }
        bool progress = false;
        if (!streamId_) {
            const auto opened = open();
            if (opened.error == Http3QuicStreamSet::Error::kStreamLimitRetry ||
                opened.error == Http3QuicStreamSet::Error::kHandshakePending) {
                return Result::kBlocked;
            }
            if (opened.error == Http3QuicStreamSet::Error::kConnectionRequestLimit) {
                // No stream ID, registration or HEADERS was consumed. The
                // owning pool can use this same stable request on a new QUIC
                // connection rather than retrying an exhausted one.
                return Result::kConnectionDraining;
            }
            if (opened.error != Http3QuicStreamSet::Error::kNone) {
                return fail();
            }
            streamId_ = opened.id;
            progress = true;
        }
        if (!registered_) {
            try {
                if (!registerResponse(*streamId_, request_.knownMethod())) {
                    return fail();
                }
            } catch (...) {
                return fail();
            }
            registered_ = true;
        }
        const auto segment = request_.next();
        if (!segment) {
            return fail();
        }
        if (!segment->empty()) {
            const auto result = write(*streamId_, *segment);
            switch (result.status) {
                case Http3QuicStreamSet::StreamWrite::Status::kAccepted:
                    if (result.bytes > segment->size() || !request_.acknowledge(result.bytes)) {
                        return fail();
                    }
                    return progress || result.bytes != 0 ? Result::kProgress : Result::kBlocked;
                case Http3QuicStreamSet::StreamWrite::Status::kWouldBlock:
                    if (!request_.acknowledge(0)) {
                        return fail();
                    }
                    return progress ? Result::kProgress : Result::kBlocked;
                default:
                    return fail();
            }
        }
        if (!request_.finReady()) {
            return fail();
        }
        const auto error = finish(*streamId_);
        if (error == Http3QuicStreamSet::Error::kNone) {
            if (!request_.acknowledgeFin(true)) {
                return fail();
            }
            return Result::kFinished;
        }
        if (error == Http3QuicStreamSet::Error::kWouldBlock) {
            return progress ? Result::kProgress : Result::kBlocked;
        }
        (void)request_.acknowledgeFin(false);
        return fail();
    }

    [[nodiscard]] std::optional<StreamId> streamId() const noexcept {
        return streamId_;
    }
    [[nodiscard]] bool finished() const noexcept {
        return request_.finished();
    }
    [[nodiscard]] bool failed() const noexcept {
        return failed_;
    }
    [[nodiscard]] std::optional<HttpClientRequestStorage> takeRequestAfterRetirement() {
        failed_ = true;
        return request_.takeRequestAfterRetirement();
    }

private:
    [[nodiscard]] Result fail() noexcept {
        failed_ = true;
        return Result::kFatal;
    }

    Http3ClientRequestWrite request_;
    std::optional<StreamId> streamId_;
    bool registered_{};
    bool failed_{};
};

}  // namespace ruvia::detail
