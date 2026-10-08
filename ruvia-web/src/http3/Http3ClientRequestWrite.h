#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/http/Http3DataWritePlan.h"
#include "ruvia/http/HttpKnownMethod.h"

#include "client/HttpClientRequestStorage.h"

namespace ruvia::detail {
class Http3ClientSansIoSessionEngine;

enum class Http3ClientRequestWriteError : std::uint8_t {
    kInvalidRequest,
    kUnsupportedTunnel,
    kRequestEncoding,
    kOutOfMemory,
    kInvalidState,
    kExcessiveAcknowledgement,
    kDataPlan,
};

// Owns the completed request in worker-local storage. Offered spans remain
// stable until acknowledged; acknowledge(0) leaves the same span available.
class Http3ClientRequestWrite final {
public:
    class PreparedTag final {
    private:
        friend class Http3ClientRequestWrite;
        PreparedTag() = default;
    };

    using Error = Http3ClientRequestWriteError;
    using Segment = std::span<const char>;

    // Failure leaves request untouched. Ownership is committed only after all
    // encoding and allocations have succeeded.
    [[nodiscard]] static std::expected<Http3ClientRequestWrite, Error> create(
        HttpClientRequestStorage&& request, std::string_view scheme, std::string_view authority,
        std::pmr::memory_resource* workerPool, Http3FieldSectionLimits limits = {}) noexcept;

    Http3ClientRequestWrite(const Http3ClientRequestWrite&) = delete;
    Http3ClientRequestWrite& operator=(const Http3ClientRequestWrite&) = delete;
    Http3ClientRequestWrite(Http3ClientRequestWrite&& other);
    Http3ClientRequestWrite& operator=(Http3ClientRequestWrite&&) = delete;

    [[nodiscard]] bool prepareConnectionHead(std::uint64_t streamId, Http3ClientSansIoSessionEngine& engine);
    [[nodiscard]] std::expected<Segment, Error> next() noexcept;
    [[nodiscard]] std::expected<void, Error> acknowledge(std::size_t count) noexcept;
    [[nodiscard]] std::expected<void, Error> acknowledgeFin(bool successful) noexcept;
    [[nodiscard]] bool requiresConnectSettings() const noexcept {
        return request_.isTunnel() && !request_.tunnelProtocol().empty();
    }
    [[nodiscard]] bool waitingForContent() const noexcept;
    void stopSending() noexcept;
    [[nodiscard]] bool finReady() const noexcept;
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] HttpKnownMethod knownMethod() const noexcept;
    // Owner must first stop all transport access, including a pending WANT
    // write. Invalidates offered spans and disables this cursor. Transfers the
    // original request once, retaining its allocator (which must outlive it).
    [[nodiscard]] std::optional<HttpClientRequestStorage> takeRequestAfterRetirement();

    // Only create() can produce PreparedTag; this lets expected construct the
    // completed cursor in place without a potentially throwing cursor move.
    explicit Http3ClientRequestWrite(PreparedTag, std::pmr::memory_resource* resource,
        HttpClientRequestStorage&& request, std::pmr::string&& scheme,
        std::pmr::string&& authority, std::pmr::vector<char>&& headers,
        Http3DataWritePlan dataPlan, Http3FieldSectionLimits limits = {}) noexcept;

private:
    enum class State : std::uint8_t { kHeaders,
        kDataHeader,
        kDataBody,
        kTrailers,
        kFin,
        kFinished,
        kFailed };
    [[nodiscard]] static Http3ClientRequestWrite& requireNoOutstandingSegment(
        Http3ClientRequestWrite& other);
    [[nodiscard]] Segment activeSegment() const noexcept;
    [[nodiscard]] std::expected<void, Error> prepareData() noexcept;
    [[nodiscard]] std::expected<void, Error> prepareTrailers() noexcept;
    [[nodiscard]] std::expected<void, Error> failPlan() noexcept;

    std::pmr::memory_resource* workerPool_;
    Http3FieldSectionLimits fieldLimits_{};
    HttpClientRequestStorage request_;
    std::pmr::string scheme_;
    std::pmr::string authority_;
    std::pmr::vector<char> headers_;
    std::optional<Http3DataWritePlan> dataPlan_;
    Http3DataWritePlan::Chunk chunk_{};
    std::size_t segmentOffset_{};
    std::size_t bodyOffset_{};
    State state_{State::kHeaders};
    bool chunkPending_{};
    bool offered_{};
    bool requestTaken_{};
};

}  // namespace ruvia::detail
