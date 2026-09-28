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

// Incrementally consumes a peer's control stream. All retained storage uses the
// caller-provided resource, which must outlive this object.
class Http3ControlStream final {
public:
    Http3ControlStream(Http3ControlRole role, std::pmr::memory_resource* resource,
        Http3StreamFramesConfig config = {}) noexcept;

    [[nodiscard]] Http3ControlStreamStatus feed(std::span<const char> input, bool fin);
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
    Http3ControlStreamStatus error_{Http3ControlStreamStatus::kNeedMoreData};
};

}  // namespace ruvia
