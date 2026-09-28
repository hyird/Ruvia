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
#include "ruvia/http/Http3ResponseWriter.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

enum class Http3BufferedResponseWriteError : std::uint8_t {
    kInvalidResponsePlan,
    kFileBodyUnsupported,
    kResponseEncoding,
    kOutOfMemory,
    kInvalidState,
    kExcessiveAcknowledgement,
    kDataPlan,
};

// Worker-affine cursor over a buffered response. The response must outlive this
// cursor; the write plan is consumed during create(). Returned spans remain
// stable until acknowledged; a zero-byte acknowledgement changes nothing.
class Http3BufferedResponseWrite final {
public:
    using Error = Http3BufferedResponseWriteError;
    using Segment = std::span<const char>;

    enum class NextStep : std::uint8_t { kBytes,
        kFin,
        kComplete,
        kFailed };

    [[nodiscard]] static std::expected<Http3BufferedResponseWrite, Error> create(
        const HttpResponse& response, const HttpBufferedResponseWritePlan& writePlan,
        std::pmr::memory_resource* workerPool) noexcept;

    Http3BufferedResponseWrite(const Http3BufferedResponseWrite&) = delete;
    Http3BufferedResponseWrite& operator=(const Http3BufferedResponseWrite&) = delete;
    // Moving is allowed only when no offered span is awaiting acknowledgement.
    Http3BufferedResponseWrite(Http3BufferedResponseWrite&& other);
    Http3BufferedResponseWrite& operator=(Http3BufferedResponseWrite&&) = delete;

    // Ordered output: one complete HEADERS frame, then zero or more DATA frame
    // headers and borrowed payload spans. An empty segment means FIN is ready.
    [[nodiscard]] std::expected<Segment, Error> next() noexcept;
    // Pure query: does not plan, offer, acknowledge, allocate, free or mutate
    // any cursor state.
    // Each next DATA chunk is planned when the preceding segment is acknowledged,
    // so headers-only output reports FIN immediately after its HEADERS ack.
    [[nodiscard]] NextStep nextStep() const noexcept;
    [[nodiscard]] std::expected<void, Error> acknowledge(std::size_t count) noexcept;
    // FIN is a separate transport operation. Only acknowledgeFin(true) commits
    // the protocol plan; false terminates the cursor without claiming success.
    [[nodiscard]] std::expected<void, Error> acknowledgeFin(bool successful) noexcept;
    [[nodiscard]] bool finReady() const noexcept;
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    // RFC 9114 decoded field-list size, including :status and projected fields.
    [[nodiscard]] std::size_t decodedFieldSectionSize() const noexcept {
        return decodedFieldSectionSize_;
    }

private:
    enum class State : std::uint8_t { kHeaders,
        kDataHeader,
        kDataBody,
        kFin,
        kFinished,
        kFailed };

    explicit Http3BufferedResponseWrite(std::pmr::memory_resource* workerPool);
    [[nodiscard]] std::expected<void, Error> prepareData() noexcept;
    [[nodiscard]] Segment activeSegment() const noexcept;
    [[nodiscard]] std::expected<void, Error> failDataPlan() noexcept;
    [[nodiscard]] static Http3BufferedResponseWrite& requireNoOutstandingSegment(
        Http3BufferedResponseWrite& other);

    std::pmr::memory_resource* workerPool_;
    std::pmr::vector<char> headers_;
    std::size_t decodedFieldSectionSize_{0};
    std::string_view body_;
    std::optional<Http3DataWritePlan> dataPlan_;
    Http3DataWritePlan::Chunk chunk_{};
    std::size_t segmentOffset_{0};
    std::size_t bodyOffset_{0};
    State state_{State::kHeaders};
    bool chunkPending_{false};
    bool offered_{false};
};

}  // namespace ruvia::detail
