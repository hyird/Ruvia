#include "ruvia/http/Http3StreamFrames.h"

#include <algorithm>
#include <limits>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3VarInt.h"

namespace ruvia {

std::size_t Http3StreamFrames::fieldSectionBufferLimit() const noexcept {
    const auto overhead = frameType_ == 0x5 ? std::size_t{8} : std::size_t{0};
    return config_.maxFieldSectionSize + std::min(overhead,
                                             std::numeric_limits<std::size_t>::max() - config_.maxFieldSectionSize);
}

std::optional<Http3ConnectionErrorCode> http3ConnectionErrorCodeForStreamFrameStatus(
    Http3StreamFrameStatus status) noexcept {
    switch (status) {
        case Http3StreamFrameStatus::kNeedMoreData:
        case Http3StreamFrameStatus::kPaused:
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

Http3StreamFrameStatus Http3StreamFrames::beginFrame(std::uint64_t type, std::uint64_t length) {
    frameType_ = type;
    frameLength_ = length;
    remaining_ = frameLength_;

    // HTTP/2 frame types are reserved in HTTP/3 and cannot be skipped as extensions.
    if (frameType_ == 0x2 || frameType_ == 0x6 || frameType_ == 0x8 || frameType_ == 0x9) {
        return Http3StreamFrameStatus::kFrameUnexpected;
    }
    const bool knownFrame = frameType_ == 0x0 || frameType_ == 0x1 || frameType_ == 0x3 ||
                            frameType_ == 0x4 || frameType_ == 0x5 || frameType_ == 0x7 ||
                            frameType_ == 0xd || frameType_ == 0xf0700 || frameType_ == 0xf0701;
    if (kind_ == Http3StreamKind::kControl) {
        if (firstFrame_ && frameType_ != 0x4) {
            return Http3StreamFrameStatus::kMissingSettings;
        }
        if ((frameType_ == 0xc || frameType_ == 0xf0700 || frameType_ == 0xf0701) && frameLength_ > config_.maxSettingsPayloadBytes) {
            return Http3StreamFrameStatus::kLimit;
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
        if (kind_ == Http3StreamKind::kResponse && frameType_ == 0x5 && !config_.allowPush && !firstFrame_) {
            return Http3StreamFrameStatus::kPushPromise;
        }
        if (knownFrame && frameType_ != 0x0 && frameType_ != 0x1 &&
            !(frameType_ == 0x5 && kind_ == Http3StreamKind::kResponse && config_.allowPush)) {
            return Http3StreamFrameStatus::kFrameUnexpected;
        }
        if (firstFrame_ && frameType_ != 0x1 &&
            !(frameType_ == 0x5 && kind_ == Http3StreamKind::kResponse && config_.allowPush)) {
            return Http3StreamFrameStatus::kFrameUnexpected;
        }
        if (frameType_ == 0x1 || frameType_ == 0x5) {
            if (kind_ == Http3StreamKind::kRequest && trailersSeen_) {
                return Http3StreamFrameStatus::kFrameUnexpected;
            }
            if (kind_ == Http3StreamKind::kRequest && headersSeen_) {
                trailersSeen_ = true;
            }
            if (frameLength_ > fieldSectionBufferLimit()) {
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

    consumed_ = 0;
    paused_ = false;
    std::size_t offset = 0;
    struct ConsumeGuard {
        std::size_t& consumed;
        std::size_t& offset;
        ~ConsumeGuard() {
            consumed = offset;
        }
    } guard{consumed_, offset};
    while (offset < input.size() || (phase_ == Phase::kFramePayload && remaining_ == 0)) {
        if (phase_ == Phase::kFrameHeader) {
            if (offset == input.size()) {
                break;
            }
            // Complete headers borrow the current input. Only a fragmented
            // header needs to survive this feed in decoder-owned storage.
            if (headerBytesUsed_ == 0) {
                if (const auto decoded = decodeHttp3FrameHeader(input.subspan(offset))) {
                    offset += decoded->encodedBytes;
                    const auto status = beginFrame(decoded->type, decoded->length);
                    if (status != Http3StreamFrameStatus::kNeedMoreData) {
                        phase_ = Phase::kFailed;
                        return status;
                    }
                    continue;
                }
            }
            header_[headerBytesUsed_++] = input[offset++];
            const auto type_width = std::size_t{1} << (static_cast<std::uint8_t>(header_[0]) >> 6);
            if (headerBytesUsed_ <= type_width) {
                continue;
            }
            const auto length_width = std::size_t{1} << (static_cast<std::uint8_t>(header_[type_width]) >> 6);
            if (headerBytesUsed_ < type_width + length_width) {
                continue;
            }
            const auto decoded = decodeHttp3FrameHeader(std::span<const char>(header_, headerBytesUsed_));
            const auto status = beginFrame(decoded->type, decoded->length);
            if (status != Http3StreamFrameStatus::kNeedMoreData) {
                phase_ = Phase::kFailed;
                return status;
            }
            continue;
        }

        if (phase_ == Phase::kFramePayload) {
            if (remaining_ == 0) {
                if (frameType_ == 0x5) {
                    const auto id = decodeHttp3VarInt(fieldSection_);
                    if (!id || fieldSection_.size() - id->encodedBytes > config_.maxFieldSectionSize) {
                        phase_ = Phase::kFailed;
                        return id ? Http3StreamFrameStatus::kLimit : Http3StreamFrameStatus::kFrameError;
                    }
                }
                if (frameType_ == 0x0 || frameType_ == 0x1 || frameType_ == 0x4 ||
                    frameType_ == 0x3 || frameType_ == 0x7 || frameType_ == 0xd || frameType_ == 0x5 || frameType_ == 0xf0700 || frameType_ == 0xf0701 || (frameType_ == 0xc && kind_ == Http3StreamKind::kControl)) {
                    callback(context, Http3StreamFrameEvent{
                                          .kind = frameType_ == 0xc       ? Http3StreamFrameEventKind::kOrigin
                                                  : frameType_ == 0xf0700 ? Http3StreamFrameEventKind::kRequestPriorityUpdate
                                                  : frameType_ == 0xf0701 ? Http3StreamFrameEventKind::kPushPriorityUpdate
                                                  : frameType_ == 0x5     ? Http3StreamFrameEventKind::kPushPromise
                                                  : frameType_ == 0x1     ? Http3StreamFrameEventKind::kHeaders
                                                  : frameType_ == 0x4     ? Http3StreamFrameEventKind::kSettings
                                                  : frameType_ == 0x3     ? Http3StreamFrameEventKind::kCancelPush
                                                  : frameType_ == 0x7     ? Http3StreamFrameEventKind::kGoaway
                                                  : frameType_ == 0xd     ? Http3StreamFrameEventKind::kMaxPushId
                                                                          : Http3StreamFrameEventKind::kData,
                                          .payload = (frameType_ == 0x1 || frameType_ == 0x5) ? std::span<const char>(fieldSection_.data(), fieldSection_.size())
                                                                                              : std::span<const char>{},
                                          .trailers = frameType_ == 0x1 && kind_ == Http3StreamKind::kRequest && trailersSeen_,
                                          .endFrame = true,
                                          .fin = fin && offset == input.size(),
                                      });
                }
                if (paused_) {
                    return Http3StreamFrameStatus::kPaused;
                }
                finishFrame();
                continue;
            }
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, input.size() - offset));
            const auto chunk = input.subspan(offset, count);
            const bool endFrame = count == remaining_;
            if (frameType_ == 0x1 || frameType_ == 0x5) {
                if (count > fieldSectionBufferLimit() || fieldSection_.size() > fieldSectionBufferLimit() - count) {
                    phase_ = Phase::kFailed;
                    return Http3StreamFrameStatus::kLimit;
                }
                fieldSection_.insert(fieldSection_.end(), chunk.begin(), chunk.end());
            } else if (frameType_ == 0x0 || frameType_ == 0x4 || frameType_ == 0x3 ||
                       frameType_ == 0x7 || frameType_ == 0xd || frameType_ == 0xf0700 || frameType_ == 0xf0701 || (frameType_ == 0xc && kind_ == Http3StreamKind::kControl)) {
                callback(context, Http3StreamFrameEvent{
                                      .kind = frameType_ == 0xc       ? Http3StreamFrameEventKind::kOrigin
                                              : frameType_ == 0xf0700 ? Http3StreamFrameEventKind::kRequestPriorityUpdate
                                              : frameType_ == 0xf0701 ? Http3StreamFrameEventKind::kPushPriorityUpdate
                                              : frameType_ == 0x0     ? Http3StreamFrameEventKind::kData
                                              : frameType_ == 0x4     ? Http3StreamFrameEventKind::kSettings
                                              : frameType_ == 0x3     ? Http3StreamFrameEventKind::kCancelPush
                                              : frameType_ == 0x7     ? Http3StreamFrameEventKind::kGoaway
                                                                      : Http3StreamFrameEventKind::kMaxPushId,
                                      .payload = chunk,
                                      .trailers = false,
                                      .endFrame = endFrame,
                                      .fin = fin && endFrame && offset + count == input.size(),
                                  });
            }
            offset += count;
            remaining_ -= count;
            if (remaining_ == 0 && frameType_ != 0x1 && frameType_ != 0x5) {
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
