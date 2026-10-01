#include "ruvia/http/Http3LocalCriticalStreams.h"

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3VarInt.h"

namespace ruvia {

std::expected<Http3LocalCriticalStreams, Http3LocalCriticalStreamsError>
Http3LocalCriticalStreams::create(const Http3Settings& settings) noexcept {
    Http3LocalCriticalStreams streams;
    const auto streamType = encodeHttp3VarInt(streams.control_, 0);
    if (!streamType) {
        return std::unexpected(Http3LocalCriticalStreamsError::kSettingsEncodingError);
    }
    std::array<char, 5 * 2 * kHttp3VarIntMaxBytes> settingsPayload{};
    const auto settingsSize = encodeHttp3Settings(settingsPayload, settings);
    if (!settingsSize) {
        return std::unexpected(Http3LocalCriticalStreamsError::kSettingsEncodingError);
    }
    const auto frameHeader = encodeHttp3FrameHeader(
        std::span<char>(streams.control_).subspan(*streamType),
        static_cast<std::uint64_t>(Http3FrameType::kSettings), *settingsSize);
    if (!frameHeader) {
        return std::unexpected(Http3LocalCriticalStreamsError::kSettingsEncodingError);
    }
    const auto payloadOffset = *streamType + *frameHeader;
    if (payloadOffset > streams.control_.size() || *settingsSize > streams.control_.size() - payloadOffset) {
        return std::unexpected(Http3LocalCriticalStreamsError::kSettingsEncodingError);
    }
    for (std::size_t i = 0; i < *settingsSize; ++i) {
        streams.control_[payloadOffset + i] = settingsPayload[i];
    }
    streams.controlSize_ = payloadOffset + *settingsSize;

    const auto encoderType = encodeHttp3VarInt(streams.qpackEncoder_, 2);
    const auto decoderType = encodeHttp3VarInt(streams.qpackDecoder_, 3);
    if (!encoderType || !decoderType) {
        return std::unexpected(Http3LocalCriticalStreamsError::kSettingsEncodingError);
    }
    streams.qpackEncoderSize_ = *encoderType;
    streams.qpackDecoderSize_ = *decoderType;
    return streams;
}

}  // namespace ruvia
