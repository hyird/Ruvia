#include "ruvia/http/Http3LocalCriticalStreams.h"

#include <variant>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3VarInt.h"

namespace ruvia {

std::variant<Http3LocalCriticalStreams, Http3LocalCriticalStreamsError>
Http3LocalCriticalStreams::create(const Http3Settings& settings) noexcept {
    Http3LocalCriticalStreams streams;
    const auto streamType = encodeHttp3VarInt(streams.control_, 0);
    if ((streamType.index() != 0)) {
        return Http3LocalCriticalStreamsError::kSettingsEncodingError;
    }
    std::array<char, 5 * 2 * kHttp3VarIntMaxBytes> settingsPayload{};
    const auto settingsSize = encodeHttp3Settings(settingsPayload, settings);
    if ((settingsSize.index() != 0)) {
        return Http3LocalCriticalStreamsError::kSettingsEncodingError;
    }
    const auto frameHeader = encodeHttp3FrameHeader(
        std::span<char>(streams.control_).subspan(std::get<0>(streamType)),
        static_cast<std::uint64_t>(Http3FrameType::kSettings), std::get<0>(settingsSize));
    if ((frameHeader.index() != 0)) {
        return Http3LocalCriticalStreamsError::kSettingsEncodingError;
    }
    const auto payloadOffset = std::get<0>(streamType) + std::get<0>(frameHeader);
    if (payloadOffset > streams.control_.size() || std::get<0>(settingsSize) > streams.control_.size() - payloadOffset) {
        return Http3LocalCriticalStreamsError::kSettingsEncodingError;
    }
    for (std::size_t i = 0; i < std::get<0>(settingsSize); ++i) {
        streams.control_[payloadOffset + i] = settingsPayload[i];
    }
    streams.controlSize_ = payloadOffset + std::get<0>(settingsSize);

    const auto encoderType = encodeHttp3VarInt(streams.qpackEncoder_, 2);
    const auto decoderType = encodeHttp3VarInt(streams.qpackDecoder_, 3);
    if ((encoderType.index() != 0) || (decoderType.index() != 0)) {
        return Http3LocalCriticalStreamsError::kSettingsEncodingError;
    }
    streams.qpackEncoderSize_ = std::get<0>(encoderType);
    streams.qpackDecoderSize_ = std::get<0>(decoderType);
    return streams;
}

}  // namespace ruvia
