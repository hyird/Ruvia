#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "ruvia/http/quic_connection.h"

#include "http3/Http3ClientSansIoSessionEngine.h"

namespace ruvia::detail {

// Single-worker, sans-runtime receive adapter. A connection driver owns one
// instance and visits each active peer/request stream fairly; this accepts at
// most one bounded QUIC read per call. The HTTP/3 engine synchronously copies
// every exposed event before this scratch buffer is reused. The caller still
// owns stream retirement, transport close, cancellation, and driver Task join.
class Http3ClientReceiveDriver final {
public:
    static constexpr std::size_t kReadBlockBytes = 16 * 1024;
    using StreamId = std::uint64_t;
    enum class Status : std::uint8_t {
        kBlocked,
        kProgress,
        kResponseComplete,
        kStreamReset,
        kPeerStreamEnded,
        kStreamError,
        kConnectionError,
        kTransportError,
    };
    struct Result final {
        Status status{Status::kBlocked};
        std::size_t bytes{};
        Http3ClientSansIoSessionEngine::Result protocol{};
        std::optional<std::uint64_t> peer_reset_error_code{};
        bool peerReportsUnprocessed{false};
    };

    explicit Http3ClientReceiveDriver(Http3ClientSansIoSessionEngine& engine) noexcept
        : engine_(engine),
          pending_(engine.resource()) {}
    [[nodiscard]] Result acceptReset(StreamId id, std::optional<std::uint64_t> peerErrorCode) {
        const bool unprocessed = peerErrorCode && engine_.peerReportsUnprocessed(id, peerErrorCode);
        auto result = feed(id, {}, false, true, 0);
        if (result.status == Status::kStreamReset) {
            result.peer_reset_error_code = peerErrorCode;
            result.peerReportsUnprocessed = unprocessed;
        }
        return result;
    }
    void retire(StreamId id) noexcept {
        pending_.erase(id);
    }
    Http3ClientReceiveDriver(const Http3ClientReceiveDriver&) = delete;
    Http3ClientReceiveDriver& operator=(const Http3ClientReceiveDriver&) = delete;

    // read(id, output) -> quic_stream_read_result. The caller must stop
    // visiting a stream after terminal status; a connection-scope failure must
    // close the QUIC transport before any response is delivered as successful.
    // A streaming owner can pass remaining producer capacity as readBudget.
    // Zero does not consume stream bytes; OpenSSL can still buffer received
    // datagrams internally, so this alone is not an end-to-end memory bound.
    template <typename Read>
    [[nodiscard]] Result drive(StreamId id, Read&& read,
        std::size_t readBudget = kReadBlockBytes) {
        if (driving_) {
            // In particular, do not overwrite scratch_ while an outer feed's
            // callback still borrows its body span from those bytes.
            throw std::logic_error("HTTP/3 receive driver cannot be called recursively");
        }
        if (readBudget == 0) {
            return {};
        }
        driving_ = true;
        struct DriveGuard final {
            bool& driving;
            ~DriveGuard() {
                driving = false;
            }
        } guard{driving_};
        if (const auto pending = pending_.find(id); pending != pending_.end()) {
            if (pending->second.bytes.size() > readBudget) {
                return {};
            }
            return feed(id, pending->second.bytes, pending->second.fin, false, 0);
        }
        const auto input = read(id,
            std::span<char>(scratch_).first(std::min(readBudget, scratch_.size())));
        using ReadStatus = ruvia::quic_stream_read_status;
        switch (input.status) {
            case ReadStatus::would_block:
                return {};
            case ReadStatus::data:
                if (input.size == 0 || input.size > std::min(readBudget, scratch_.size())) {
                    return transportError();
                }
                return feed(id, {scratch_.data(), input.size}, false, false, input.size);
            case ReadStatus::fin:
                return feed(id, {}, true, false, 0);
            case ReadStatus::reset: {
                const bool unprocessed = input.peer_reset_error_code &&
                                         engine_.peerReportsUnprocessed(id, input.peer_reset_error_code);
                auto result = feed(id, {}, false, true, 0);
                if (result.status == Status::kStreamReset) {
                    result.peer_reset_error_code = input.peer_reset_error_code;
                    result.peerReportsUnprocessed = unprocessed;
                }
                return result;
            }
            case ReadStatus::closed:
                return transportError();
        }
        return transportError();
    }

private:
    [[nodiscard]] Result feed(StreamId id, std::span<const char> bytes,
        bool fin, bool reset, std::size_t acceptedBytes) {
        const auto parsed = engine_.feed(id, bytes, fin, reset);
        if (parsed.scope == Http3ConnectionErrorScope::kConnection) {
            return {.status = parsed.status == Http3ClientSansIoSessionStatus::kTransportError
                                  ? Status::kTransportError
                                  : Status::kConnectionError,
                .bytes = acceptedBytes,
                .protocol = parsed};
        }
        if (parsed.scope == Http3ConnectionErrorScope::kStream) {
            return {.status = Status::kStreamError, .bytes = acceptedBytes, .protocol = parsed};
        }
        if (parsed.status == Http3ClientSansIoSessionStatus::kInvalidState ||
            parsed.status == Http3ClientSansIoSessionStatus::kStreamLimitExceeded) {
            return transportError();
        }
        if (parsed.status == Http3ClientSansIoSessionStatus::kQpackBlocked || parsed.status == Http3ClientSansIoSessionStatus::kPushPromisePending) {
            if (parsed.consumedBytes > bytes.size()) {
                return transportError();
            }
            if (!pending_.contains(id) && pending_.size() >= engine_.maxLiveStreams()) {
                return transportError();
            }
            auto [pending, inserted] = pending_.try_emplace(id, engine_.resource());
            pending->second.bytes.assign(std::string_view(bytes.data(), bytes.size()).substr(parsed.consumedBytes));
            pending->second.fin = fin;
            return {.status = acceptedBytes != 0 || parsed.consumedBytes != 0 ? Status::kProgress : Status::kBlocked,
                .bytes = acceptedBytes,
                .protocol = parsed};
        }
        pending_.erase(id);
        if (reset) {
            return {.status = Status::kStreamReset, .protocol = parsed};
        }
        if (fin) {
            return {.status = parsed.status == Http3ClientSansIoSessionStatus::kMessageEnd
                                  ? Status::kResponseComplete
                                  : Status::kPeerStreamEnded,
                .protocol = parsed};
        }
        return {.status = Status::kProgress, .bytes = acceptedBytes, .protocol = parsed};
    }

    [[nodiscard]] Result transportError() noexcept {
        const auto failure = engine_.stop();
        return {.status = failure.status == Http3ClientSansIoSessionStatus::kTransportError
                              ? Status::kTransportError
                              : Status::kConnectionError,
            .protocol = failure};
    }

    struct PendingInput final {
        explicit PendingInput(std::pmr::memory_resource* resource)
            : bytes(resource) {}
        std::pmr::string bytes;
        bool fin{};
    };
    Http3ClientSansIoSessionEngine& engine_;
    std::pmr::unordered_map<StreamId, PendingInput> pending_;
    std::array<char, kReadBlockBytes> scratch_{};
    bool driving_{};
};

}  // namespace ruvia::detail
