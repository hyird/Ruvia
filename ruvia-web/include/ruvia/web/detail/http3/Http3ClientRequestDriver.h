#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/http/quic_connection.h"
#include "ruvia/web/detail/http3/Http3ClientRequestWrite.h"

namespace ruvia::detail {

// One worker-affine outbound request stream. The caller must first confirm that
// the local control and QPACK stream prefixes have been accepted. This driver
// only writes the request; a separate response owner handles reads and joins.
// Keep its address stable while a write is pending, and close the QUIC stream
// before destroying this driver on cancellation, fatal error or shutdown.
class Http3ClientRequestDriver final {
public:
    using StreamId = std::uint64_t;
    enum class Result : std::uint8_t { kBlocked,
        kConnectionDraining,
        kProgress,
        kFinished,
        kFatal };

    explicit Http3ClientRequestDriver(Http3ClientRequestWrite&& request)
        : request_(std::in_place, std::move(request)) {}
    Http3ClientRequestDriver(const Http3ClientRequestDriver&) = delete;
    Http3ClientRequestDriver& operator=(const Http3ClientRequestDriver&) = delete;
    Http3ClientRequestDriver(Http3ClientRequestDriver&&) = delete;
    Http3ClientRequestDriver& operator=(Http3ClientRequestDriver&&) = delete;

    // open() -> quic_stream_open_result; registerResponse(id, knownMethod) -> bool;
    // write(id, bytes) -> quic_stream_write_result; finish(id) -> quic_operation_status. Register the
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
        if (request_->finished()) {
            return Result::kFinished;
        }
        bool progress = false;
        if (!streamId_) {
            const auto opened = open();
            if (opened.status == ruvia::quic_operation_status::would_block ||
                opened.status == ruvia::quic_operation_status::need_input) {
                return Result::kBlocked;
            }
            if (opened.status == ruvia::quic_operation_status::draining ||
                opened.status == ruvia::quic_operation_status::closing ||
                opened.status == ruvia::quic_operation_status::retired) {
                return Result::kConnectionDraining;
            }
            if (opened.status != ruvia::quic_operation_status::accepted) {
                return fail();
            }
            streamId_ = opened.stream_id;
            progress = true;
        }
        if (!registered_) {
            try {
                if (!registerResponse(*streamId_, request_->knownMethod())) {
                    return fail();
                }
            } catch (...) {
                return fail();
            }
            registered_ = true;
        }
        const auto segment = request_->next();
        if (!segment) {
            return fail();
        }
        if (!segment->empty()) {
            const auto result = write(*streamId_, *segment);
            switch (result.status) {
                case ruvia::quic_operation_status::accepted:
                    if (result.accepted > segment->size() || !request_->acknowledge(result.accepted)) {
                        return fail();
                    }
                    return progress || result.accepted != 0 ? Result::kProgress : Result::kBlocked;
                case ruvia::quic_operation_status::would_block:
                case ruvia::quic_operation_status::need_input:
                    if (!request_->acknowledge(0)) {
                        return fail();
                    }
                    return progress ? Result::kProgress : Result::kBlocked;
                default:
                    return fail();
            }
        }
        if (!request_->finReady()) {
            return progress ? Result::kProgress : Result::kBlocked;
        }
        const auto error = finish(*streamId_);
        if (error == ruvia::quic_operation_status::accepted) {
            if (!request_->acknowledgeFin(true)) {
                return fail();
            }
            return Result::kFinished;
        }
        if (error == ruvia::quic_operation_status::would_block ||
            error == ruvia::quic_operation_status::need_input) {
            return progress ? Result::kProgress : Result::kBlocked;
        }
        (void)request_->acknowledgeFin(false);
        return fail();
    }

    [[nodiscard]] bool prepareConnectionHead(std::uint64_t id, Http3ClientSansIoSessionEngine& engine) {
        return request_->prepareConnectionHead(id, engine);
    }
    void stopSending() noexcept {
        request_->stopSending();
    }
    [[nodiscard]] bool requiresConnectSettings() const noexcept {
        return request_->requiresConnectSettings();
    }
    [[nodiscard]] bool waitingForContent() const noexcept {
        return request_->waitingForContent();
    }
    [[nodiscard]] std::optional<StreamId> streamId() const noexcept {
        return streamId_;
    }
    [[nodiscard]] bool finished() const noexcept {
        return request_->finished();
    }
    [[nodiscard]] bool failed() const noexcept {
        return failed_;
    }
    [[nodiscard]] std::optional<HttpClientRequestStorage> takeRequestAfterRetirement() {
        failed_ = true;
        return request_->takeRequestAfterRetirement();
    }
    [[nodiscard]] bool replay_after_rejected_early_stream(
        std::string_view scheme, std::string_view authority,
        std::pmr::memory_resource* resource) {
        auto original = request_->takeRequestAfterRetirement();
        if (!original) {
            return false;
        }
        auto replay = Http3ClientRequestWrite::create(
            std::move(*original), scheme, authority, resource);
        if (!replay) {
            return false;
        }
        request_.reset();
        request_.emplace(std::move(*replay));
        streamId_.reset();
        registered_ = false;
        failed_ = false;
        return true;
    }

private:
    [[nodiscard]] Result fail() noexcept {
        failed_ = true;
        return Result::kFatal;
    }

    std::optional<Http3ClientRequestWrite> request_;
    std::optional<StreamId> streamId_;
    bool registered_{};
    bool failed_{};
};

}  // namespace ruvia::detail
