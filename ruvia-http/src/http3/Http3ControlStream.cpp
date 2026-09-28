#include "ruvia/http/Http3ControlStream.h"

#include <array>

#include "ruvia/http/Http3VarInt.h"

namespace ruvia {

Http3ControlStream::Http3ControlStream(Http3ControlRole role, std::pmr::memory_resource* resource,
    Http3StreamFramesConfig config) noexcept
    : role_(role),
      resource_(resource != nullptr ? resource : std::pmr::get_default_resource()),
      frames_(Http3StreamKind::kControl, resource_, config),
      settingsPayload_(resource_) {}

void Http3ControlStream::onFrame(void* context, Http3StreamFrameEvent event) {
    static_cast<Http3ControlStream*>(context)->consume(event);
}

void Http3ControlStream::consume(Http3StreamFrameEvent event) {
    if (error_ != Http3ControlStreamStatus::kNeedMoreData) {
        return;
    }
    if (event.kind == Http3StreamFrameEventKind::kSettings) {
        if (event.payload.size() > frames_.config().maxSettingsPayloadBytes ||
            settingsPayload_.size() > frames_.config().maxSettingsPayloadBytes - event.payload.size()) {
            error_ = Http3ControlStreamStatus::kLimit;
            return;
        }
        settingsPayload_.insert(settingsPayload_.end(), event.payload.begin(), event.payload.end());
        if (!event.endFrame) {
            return;
        }
        auto decoded = decodeHttp3Settings(settingsPayload_, resource_);
        if (!decoded) {
            error_ = Http3ControlStreamStatus::kSettingsError;
            return;
        }
        settings_ = std::move(*decoded);
        std::pmr::vector<char> released(resource_);
        settingsPayload_.swap(released);
        return;
    }
    if (event.kind != Http3StreamFrameEventKind::kCancelPush &&
        event.kind != Http3StreamFrameEventKind::kGoaway &&
        event.kind != Http3StreamFrameEventKind::kMaxPushId) {
        return;
    }

    if (fixedPayloadSize_ == 0) {
        fixedKind_ = event.kind;
    }
    if (fixedKind_ != event.kind || event.payload.size() > fixedPayload_.size() - fixedPayloadSize_) {
        error_ = Http3ControlStreamStatus::kFrameError;
        return;
    }
    for (char byte : event.payload) {
        fixedPayload_[fixedPayloadSize_++] = byte;
    }
    if (!event.endFrame) {
        return;
    }
    if (fixedPayloadSize_ == 0) {
        error_ = Http3ControlStreamStatus::kFrameError;
        return;
    }
    const auto decoded = decodeHttp3VarInt(std::span<const char>(fixedPayload_).first(fixedPayloadSize_));
    if (!decoded || decoded->encodedBytes != fixedPayloadSize_) {
        error_ = Http3ControlStreamStatus::kFrameError;
        return;
    }
    const auto value = decoded->value;
    switch (fixedKind_) {
        case Http3StreamFrameEventKind::kCancelPush:
            if (role_ == Http3ControlRole::kServer && (!maxPushId_ || value > *maxPushId_)) {
                error_ = Http3ControlStreamStatus::kIdError;
            } else {
                cancelPushId_ = value;
            }
            break;
        case Http3StreamFrameEventKind::kGoaway:
            if (role_ == Http3ControlRole::kClient && (value & 0x3U) != 0) {
                error_ = Http3ControlStreamStatus::kIdError;
            } else if (goawayId_ && value > *goawayId_) {
                error_ = Http3ControlStreamStatus::kIdError;
            } else {
                goawayId_ = value;
            }
            break;
        case Http3StreamFrameEventKind::kMaxPushId:
            if (role_ == Http3ControlRole::kClient || (maxPushId_ && value < *maxPushId_)) {
                error_ = role_ == Http3ControlRole::kClient
                             ? Http3ControlStreamStatus::kFrameUnexpected
                             : Http3ControlStreamStatus::kIdError;
            } else {
                maxPushId_ = value;
            }
            break;
        default:
            break;
    }
    fixedPayloadSize_ = 0;
}

Http3ControlStreamStatus Http3ControlStream::feed(std::span<const char> input, bool fin) {
    if (error_ != Http3ControlStreamStatus::kNeedMoreData) {
        return error_;
    }
    const auto status = frames_.feed(input, fin, onFrame, this);
    if (error_ != Http3ControlStreamStatus::kNeedMoreData) {
        return error_;
    }
    switch (status) {
        case Http3StreamFrameStatus::kNeedMoreData:
        case Http3StreamFrameStatus::kMessageEnd:
            return Http3ControlStreamStatus::kNeedMoreData;
        case Http3StreamFrameStatus::kClosedCriticalStream:
            error_ = Http3ControlStreamStatus::kClosedCriticalStream;
            break;
        case Http3StreamFrameStatus::kMissingSettings:
            error_ = Http3ControlStreamStatus::kMissingSettings;
            break;
        case Http3StreamFrameStatus::kFrameUnexpected:
        case Http3StreamFrameStatus::kPushPromise:
            error_ = Http3ControlStreamStatus::kFrameUnexpected;
            break;
        case Http3StreamFrameStatus::kLimit:
            error_ = Http3ControlStreamStatus::kLimit;
            break;
        case Http3StreamFrameStatus::kFrameError:
            error_ = Http3ControlStreamStatus::kFrameError;
            break;
    }
    return error_;
}

}  // namespace ruvia
