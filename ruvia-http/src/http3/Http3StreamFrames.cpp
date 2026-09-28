#include "ruvia/http/Http3StreamFrames.h"

#include <algorithm>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3VarInt.h"

namespace ruvia {

std::optional<Http3ConnectionErrorCode> http3ConnectionErrorCodeForStreamFrameStatus(
    Http3StreamFrameStatus status) noexcept {
    switch (status) {
        case Http3StreamFrameStatus::kNeedMoreData:
        case Http3StreamFrameStatus::kMessageEnd:
            return std::nullopt;
        case Http3StreamFrameStatus::kFrameUnexpected:
        case Http3StreamFrameStatus::kPushPromise:
            return Http3ConnectionErrorCode::kFrameUnexpected;
        case Http3StreamFrameStatus::kMissingSettings:
            return Http3ConnectionErrorCode::kMissingSettings;
        case Http3StreamFrameStatus::kFrameError:
            return Http3ConnectionErrorCode::kFrameError;
        case Http3StreamFrameStatus::kClosedCriticalStream:
            return Http3ConnectionErrorCode::kClosedCriticalStream;
        case Http3StreamFrameStatus::kLimit:
            return Http3ConnectionErrorCode::kExcessiveLoad;
    }
    return Http3ConnectionErrorCode::kFrameError;
}

Http3StreamFrames::Http3StreamFrames(Http3StreamKind kind, std::pmr::memory_resource* resource,
    Http3StreamFramesConfig config) noexcept
    : kind_(kind),
      config_(config),
      resource_(resource),
      fieldSection_(resource) {}

Http3StreamFrameStatus Http3StreamFrames::beginFrame() {
    const auto decoded = decodeHttp3FrameHeader(std::span<const char>(header_, headerBytesUsed_));
    if (!decoded) {
        return Http3StreamFrameStatus::kFrameError;
    }
    frameType_ = decoded->type;
    frameLength_ = decoded->length;
    remaining_ = frameLength_;

    // HTTP/2 frame types are reserved in HTTP/3 and cannot be skipped as extensions.
    if (frameType_ == 0x2 || frameType_ == 0x6 || frameType_ == 0x8 || frameType_ == 0x9) {
        return Http3StreamFrameStatus::kFrameUnexpected;
    }
    const bool knownFrame = frameType_ == 0x0 || frameType_ == 0x1 || frameType_ == 0x3 ||
                            frameType_ == 0x4 || frameType_ == 0x5 || frameType_ == 0x7 ||
                            frameType_ == 0xd;
    if (kind_ == Http3StreamKind::kControl) {
        if (firstFrame_ && frameType_ != 0x4) {
            return Http3StreamFrameStatus::kMissingSettings;
        }
        if (frameType_ == 0x4) {
            if (settingsSeen_) {
                return Http3StreamFrameStatus::kFrameUnexpected;
            }
            if (frameLength_ > config_.maxSettingsPayloadBytes) {
                return Http3StreamFrameStatus::kLimit;
            }
            settingsSeen_ = true;
        } else if (frameType_ == 0x0 || frameType_ == 0x1 || frameType_ == 0x5) {
            return Http3StreamFrameStatus::kFrameUnexpected;
        }
    } else {
        if (kind_ == Http3StreamKind::kResponse && frameType_ == 0x5 && !firstFrame_) {
            return Http3StreamFrameStatus::kPushPromise;
        }
        if (knownFrame && frameType_ != 0x0 && frameType_ != 0x1) {
            return Http3StreamFrameStatus::kFrameUnexpected;
        }
        if (firstFrame_ && frameType_ != 0x1) {
            return Http3StreamFrameStatus::kFrameUnexpected;
        }
        if (frameType_ == 0x1) {
            if (kind_ == Http3StreamKind::kRequest && trailersSeen_) {
                return Http3StreamFrameStatus::kFrameUnexpected;
            }
            if (kind_ == Http3StreamKind::kRequest && headersSeen_) {
                trailersSeen_ = true;
            }
            if (frameLength_ > config_.maxFieldSectionSize) {
                return Http3StreamFrameStatus::kLimit;
            }
            fieldSection_.clear();
        } else if (frameType_ == 0x0) {
            if (!headersSeen_ || (kind_ == Http3StreamKind::kRequest && trailersSeen_)) {
                return Http3StreamFrameStatus::kFrameUnexpected;
            }
        }
    }
    firstFrame_ = false;
    phase_ = Phase::kFramePayload;
    return Http3StreamFrameStatus::kNeedMoreData;
}

Http3StreamFrameStatus Http3StreamFrames::finishFrame() noexcept {
    if (kind_ != Http3StreamKind::kControl && frameType_ == 0x1) {
        headersSeen_ = true;
    }
    phase_ = Phase::kFrameHeader;
    headerBytesUsed_ = 0;
    headerBytesNeeded_ = 0;
    frameLength_ = 0;
    remaining_ = 0;
    return Http3StreamFrameStatus::kNeedMoreData;
}

Http3StreamFrameStatus Http3StreamFrames::feed(std::span<const char> input, bool fin,
    Http3StreamFrameCallback callback, void* context) {
    if (phase_ == Phase::kFailed) {
        return Http3StreamFrameStatus::kFrameError;
    }
    if (phase_ == Phase::kEnded) {
        return Http3StreamFrameStatus::kFrameError;
    }
    if (resource_ == nullptr || callback == nullptr) {
        phase_ = Phase::kFailed;
        return Http3StreamFrameStatus::kFrameError;
    }

    std::size_t offset = 0;
    while (offset < input.size() || (phase_ == Phase::kFramePayload && remaining_ == 0)) {
        if (phase_ == Phase::kFrameHeader) {
            if (offset == input.size()) {
                break;
            }
            header_[headerBytesUsed_++] = input[offset++];
            const auto typeWidth = std::size_t{1} << (static_cast<std::uint8_t>(header_[0]) >> 6);
            if (headerBytesUsed_ < typeWidth) {
                continue;
            }
            if (headerBytesUsed_ == typeWidth) {
                continue;
            }
            const auto lengthOffset = typeWidth;
            const auto lengthWidth = std::size_t{1} << (static_cast<std::uint8_t>(header_[lengthOffset]) >> 6);
            headerBytesNeeded_ = typeWidth + lengthWidth;
            if (headerBytesUsed_ < headerBytesNeeded_) {
                continue;
            }
            const auto status = beginFrame();
            if (status != Http3StreamFrameStatus::kNeedMoreData) {
                phase_ = Phase::kFailed;
                return status;
            }
            continue;
        }

        if (phase_ == Phase::kFramePayload) {
            if (remaining_ == 0) {
                if (frameType_ == 0x0 || frameType_ == 0x1 || frameType_ == 0x4 ||
                    frameType_ == 0x3 || frameType_ == 0x7 || frameType_ == 0xd) {
                    callback(context, Http3StreamFrameEvent{
                                          .kind = frameType_ == 0x1   ? Http3StreamFrameEventKind::kHeaders
                                                  : frameType_ == 0x4 ? Http3StreamFrameEventKind::kSettings
                                                  : frameType_ == 0x3 ? Http3StreamFrameEventKind::kCancelPush
                                                  : frameType_ == 0x7 ? Http3StreamFrameEventKind::kGoaway
                                                  : frameType_ == 0xd ? Http3StreamFrameEventKind::kMaxPushId
                                                                      : Http3StreamFrameEventKind::kData,
                                          .payload = frameType_ == 0x1 ? std::span<const char>(fieldSection_.data(), fieldSection_.size())
                                                                       : std::span<const char>{},
                                          .trailers = frameType_ == 0x1 && kind_ == Http3StreamKind::kRequest && trailersSeen_,
                                          .endFrame = true,
                                          .fin = fin && offset == input.size(),
                                      });
                }
                finishFrame();
                continue;
            }
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, input.size() - offset));
            const auto chunk = input.subspan(offset, count);
            const bool endFrame = count == remaining_;
            if (frameType_ == 0x1) {
                if (fieldSection_.size() > config_.maxFieldSectionSize - count) {
                    phase_ = Phase::kFailed;
                    return Http3StreamFrameStatus::kLimit;
                }
                fieldSection_.insert(fieldSection_.end(), chunk.begin(), chunk.end());
            } else if (frameType_ == 0x0 || frameType_ == 0x4 || frameType_ == 0x3 ||
                       frameType_ == 0x7 || frameType_ == 0xd) {
                callback(context, Http3StreamFrameEvent{
                                      .kind = frameType_ == 0x0   ? Http3StreamFrameEventKind::kData
                                              : frameType_ == 0x4 ? Http3StreamFrameEventKind::kSettings
                                              : frameType_ == 0x3 ? Http3StreamFrameEventKind::kCancelPush
                                              : frameType_ == 0x7 ? Http3StreamFrameEventKind::kGoaway
                                                                  : Http3StreamFrameEventKind::kMaxPushId,
                                      .payload = chunk,
                                      .trailers = false,
                                      .endFrame = endFrame,
                                      .fin = fin && endFrame && offset + count == input.size(),
                                  });
            }
            offset += count;
            remaining_ -= count;
            if (remaining_ == 0 && frameType_ != 0x1) {
                if (count == 0) {
                    continue;
                }
                finishFrame();
            }
        }
    }

    if (fin) {
        if (phase_ != Phase::kFrameHeader || headerBytesUsed_ != 0) {
            phase_ = Phase::kFailed;
            return Http3StreamFrameStatus::kFrameError;
        }
        if (kind_ == Http3StreamKind::kControl) {
            phase_ = Phase::kFailed;
            return Http3StreamFrameStatus::kClosedCriticalStream;
        }
        if (!headersSeen_) {
            phase_ = Phase::kFailed;
            return Http3StreamFrameStatus::kFrameUnexpected;
        }
        phase_ = Phase::kEnded;
        return Http3StreamFrameStatus::kMessageEnd;
    }
    return Http3StreamFrameStatus::kNeedMoreData;
}

}  // namespace ruvia
