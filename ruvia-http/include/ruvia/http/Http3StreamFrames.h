#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

#include "ruvia/http/Http3ConnectionError.h"

namespace ruvia {

enum class Http3StreamKind : std::uint8_t {
    kRequest,
    kResponse,
    kControl,
};

enum class Http3StreamFrameStatus : std::uint8_t {
    kNeedMoreData,
    kPaused,
    kMessageEnd,
    kFrameUnexpected,
    kPushPromise,
    kMissingSettings,
    kFrameError,
    kClosedCriticalStream,
    kLimit,
};

// Returns no value for non-error framing progress/end statuses. PUSH_PROMISE
// maps to FRAME_UNEXPECTED by default; callers may apply a narrower
// context-specific code when push authorization semantics require it.
[[nodiscard]] std::optional<Http3ConnectionErrorCode>
http3ConnectionErrorCodeForStreamFrameStatus(Http3StreamFrameStatus status) noexcept;

enum class Http3StreamFrameEventKind : std::uint8_t {
    kHeaders,
    kPushPromise,
    kData,
    kSettings,
    kCancelPush,
    kGoaway,
    kMaxPushId,
    kRequestPriorityUpdate,
    kPushPriorityUpdate,
    kOrigin,
};

struct Http3StreamFrameEvent final {
    Http3StreamFrameEventKind kind{Http3StreamFrameEventKind::kData};
    std::span<const char> payload{};
    bool trailers{false};
    bool endFrame{false};
    bool fin{false};
};

using Http3StreamFrameCallback = void (*)(void*, Http3StreamFrameEvent);

struct Http3StreamFramesConfig final {
    std::size_t maxFieldSectionSize{64 * 1024};
    std::size_t maxSettingsPayloadBytes{64 * 1024};
    bool allowPush{false};
};

// Incrementally decodes one HTTP/3 message direction or control stream. A
// response can have multiple informational HEADERS; its caller must validate
// the decoded status sequence and distinguish final headers from trailers.
// Callback views
// borrow the current feed input, except HEADERS which borrow decoder-owned
// storage; all views are valid only until the callback returns. The supplied
// memory resource must outlive this decoder.
class Http3StreamFrames final {
public:
    Http3StreamFrames(Http3StreamKind kind, std::pmr::memory_resource* resource,
        Http3StreamFramesConfig config = {}) noexcept;

    [[nodiscard]] Http3StreamFrameStatus feed(std::span<const char> input, bool fin,
        Http3StreamFrameCallback callback, void* context);

    // Called by a HEADERS callback to retain the complete section and stop
    // consuming input. The next feed retries that callback before new bytes.
    void allowPush() noexcept {
        config_.allowPush = true;
    }
    void pause() noexcept {
        paused_ = true;
    }
    [[nodiscard]] bool paused() const noexcept {
        return paused_;
    }
    [[nodiscard]] std::size_t consumedBytes() const noexcept {
        return consumed_;
    }

    [[nodiscard]] const Http3StreamFramesConfig& config() const noexcept {
        return config_;
    }

private:
    enum class Phase : std::uint8_t {
        kFrameHeader,
        kFramePayload,
        kFailed,
        kEnded,
    };

    [[nodiscard]] Http3StreamFrameStatus beginFrame();
    [[nodiscard]] std::size_t fieldSectionBufferLimit() const noexcept;
    Http3StreamFrameStatus finishFrame() noexcept;

    Http3StreamKind kind_;
    Http3StreamFramesConfig config_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<char> fieldSection_;
    Phase phase_{Phase::kFrameHeader};
    std::uint64_t frameType_{0};
    std::uint64_t frameLength_{0};
    std::uint64_t remaining_{0};
    std::uint8_t firstHeaderByte_{0};
    std::size_t headerBytesNeeded_{0};
    std::size_t headerBytesUsed_{0};
    char header_[16]{};
    bool paused_{false};
    std::size_t consumed_{0};
    bool firstFrame_{true};
    bool headersSeen_{false};
    bool trailersSeen_{false};
    bool settingsSeen_{false};
};

}  // namespace ruvia
