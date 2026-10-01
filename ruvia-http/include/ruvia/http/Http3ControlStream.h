#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

#include "ruvia/http/Http3Settings.h"
#include "ruvia/http/Http3StreamFrames.h"
#include "ruvia/http/HttpConnectionAdvertisement.h"
#include "ruvia/http/HttpPriority.h"

namespace ruvia {

enum class Http3ControlRole : std::uint8_t { kClient,
    kServer };
enum class Http3ControlStreamStatus : std::uint8_t {
    kNeedMoreData,
    kClosedCriticalStream,
    kMissingSettings,
    kFrameUnexpected,
    kSettingsError,
    kIdError,
    kFrameError,
    kLimit,
};

struct Http3ControlStreamEvent final {
    Http3StreamFrameEventKind kind;
    std::uint64_t id;
    std::optional<HttpPriorityUpdate> priorityUpdate{};
    const HttpOriginAdvertisement* originAdvertisement{nullptr};
};
using Http3ControlStreamCallback = void (*)(void*, Http3ControlStreamEvent);

// Incrementally consumes a peer's control stream. All retained storage uses the
// caller-provided resource, which must outlive this object.
class Http3ControlStream final {
public:
    Http3ControlStream(Http3ControlRole role, std::pmr::memory_resource* resource,
        Http3StreamFramesConfig config = {}) noexcept;

    [[nodiscard]] Http3ControlStreamStatus feed(std::span<const char> input, bool fin,
        Http3ControlStreamCallback callback = nullptr, void* context = nullptr);
    [[nodiscard]] const std::optional<Http3Settings>& peerSettings() const noexcept {
        return settings_;
    }
    [[nodiscard]] std::optional<std::uint64_t> goawayId() const noexcept {
        return goawayId_;
    }
    [[nodiscard]] std::optional<std::uint64_t> maxPushId() const noexcept {
        return maxPushId_;
    }
    [[nodiscard]] std::optional<std::uint64_t> lastCancelPushId() const noexcept {
        return cancelPushId_;
    }

private:
    static void onFrame(void* context, Http3StreamFrameEvent event);
    void consume(Http3StreamFrameEvent event);

    Http3ControlRole role_;
    std::pmr::memory_resource* resource_;
    Http3StreamFrames frames_;
    std::pmr::vector<char> settingsPayload_;
    std::optional<Http3Settings> settings_;
    std::optional<std::uint64_t> goawayId_;
    std::optional<std::uint64_t> maxPushId_;
    std::optional<std::uint64_t> cancelPushId_;
    std::array<char, 8> fixedPayload_{};
    std::size_t fixedPayloadSize_{0};
    Http3StreamFrameEventKind fixedKind_{Http3StreamFrameEventKind::kData};
    Http3ControlStreamCallback callback_{nullptr};
    void* context_{nullptr};
    bool feeding_{false};
    Http3ControlStreamStatus error_{Http3ControlStreamStatus::kNeedMoreData};
};

}  // namespace ruvia
