#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "ruvia/http/Http3FieldSection.h"

namespace ruvia {

enum class Http3QpackConnectionError : std::uint8_t {
    kDecompressionFailed,
    kEncoderStreamError,
    kDecoderStreamError,
    kClosedCriticalStream,
    kLimit,
    kInvalidStreamId,
};

struct Http3QpackDecoderConfig final {
    std::size_t maxTableCapacity{4096};
    std::size_t maxBlockedStreams{16};
    Http3FieldSectionLimits fields{};
    std::size_t maxPendingOutputBytes{256 * 1024};
};

struct Http3QpackEncoderConfig final {
    // Values received in the peer's SETTINGS.
    std::size_t maxTableCapacity{4096};
    std::size_t maxBlockedStreams{16};
    Http3FieldSectionLimits fields{};
    // Local policy may choose a smaller table without changing Required Insert
    // Count wrapping, which always uses the peer's advertised maximum.
    std::optional<std::size_t> tableCapacity{};
    std::size_t maxOutstandingSections{4096};
    std::size_t maxPendingOutputBytes{256 * 1024};
};

enum class Http3QpackDecodeStatus : std::uint8_t { kDecoded,
    kBlocked,
    kCallbackStopped };
struct Http3QpackDecodeResult final {
    Http3QpackDecodeStatus status{Http3QpackDecodeStatus::kDecoded};
    std::size_t fields{0};
};

// RFC 9204 receive context, shared by all streams in one HTTP/3 connection.
// Blocking retains only the stream ID: the driver must retain the complete
// field section and retry after consumeEncoder() advances insertCount().
// Callback views expire on return; no callback may reenter this context.
// Both contexts own all storage in resource, which must outlive them. They
// are worker-affine and do not perform transport I/O. Critical-stream output
// excludes the stream-type prefix. Consume output only after it is transmitted.
class Http3QpackDecoder final {
public:
    Http3QpackDecoder(Http3QpackDecoderConfig config,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    ~Http3QpackDecoder();
    Http3QpackDecoder(const Http3QpackDecoder&) = delete;
    Http3QpackDecoder& operator=(const Http3QpackDecoder&) = delete;
    [[nodiscard]] std::variant<std::monostate, Http3QpackConnectionError> consumeEncoder(
        std::span<const char> bytes, bool fin = false);
    [[nodiscard]] std::variant<Http3QpackDecodeResult, Http3QpackConnectionError> decode(
        std::uint64_t streamId, std::span<const char> section,
        Http3FieldSectionCallback callback, void* context);
    // Cancels a blocked or outstanding field section; emits Stream Cancellation.
    [[nodiscard]] std::variant<std::monostate, Http3QpackConnectionError> cancel(std::uint64_t streamId);
    [[nodiscard]] std::span<const char> pendingDecoderOutput() const& noexcept;
    std::span<const char> pendingDecoderOutput() const&& = delete;
    [[nodiscard]] bool consumeDecoderOutput(std::size_t bytes) noexcept;
    [[nodiscard]] std::uint64_t insertCount() const noexcept;
    [[nodiscard]] std::size_t blockedStreamCount() const noexcept;

private:
    struct Impl;
    std::pmr::memory_resource* resource_;
    Impl* impl_;
};

// Dynamic references are pinned until peer acknowledgment/cancellation. When
// capacity, blocked-stream allowance, or eviction safety prevents insertion,
// encoding falls back to valid static/literal representations. A kLimit result
// is recoverable; encoding may already have inserted entries and queued valid
// encoder-stream instructions, so the caller must continue draining pending
// encoder output. An exception propagates and latches kDecoderStreamError as a
// terminal connection failure. After an exception, do not send any remaining
// pending encoder output or retry/recreate the encoder on the same connection.
// The returned field-section vector uses result_resource (or the encoder's
// resource when null); that resource must outlive the returned vector. An
// exception, including result allocation failure, leaves the encoder terminal.
class Http3QpackEncoder final {
public:
    Http3QpackEncoder(Http3QpackEncoderConfig config,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    ~Http3QpackEncoder();
    Http3QpackEncoder(const Http3QpackEncoder&) = delete;
    Http3QpackEncoder& operator=(const Http3QpackEncoder&) = delete;
    [[nodiscard]] std::variant<std::pmr::vector<char>, Http3QpackConnectionError> encode(
        std::uint64_t streamId, std::span<const Http3FieldSectionFieldView> fields,
        Http3FieldSectionLimits limits = {}, std::pmr::memory_resource* result_resource = nullptr);
    [[nodiscard]] std::variant<std::monostate, Http3QpackConnectionError> consumeDecoder(
        std::span<const char> bytes, bool fin = false);
    [[nodiscard]] std::span<const char> pendingEncoderOutput() const& noexcept;
    std::span<const char> pendingEncoderOutput() const&& = delete;
    [[nodiscard]] bool consumeEncoderOutput(std::size_t bytes) noexcept;
    [[nodiscard]] std::uint64_t insertCount() const noexcept;
    [[nodiscard]] std::uint64_t knownReceivedCount() const noexcept;

private:
    struct Impl;
    std::pmr::memory_resource* resource_;
    Impl* impl_;
};
}  // namespace ruvia
