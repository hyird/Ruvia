#pragma once

#include <cstdint>

namespace ruvia {

enum class Http3ConnectionErrorScope : std::uint8_t {
    kNone,
    kStream,
    kConnection,
};

enum class Http3ConnectionErrorCode : std::uint64_t {
    kNoError = 0x100,
    kGeneralProtocolError = 0x101,
    kInternalError = 0x102,
    kStreamCreationError = 0x103,
    kClosedCriticalStream = 0x104,
    kFrameUnexpected = 0x105,
    kFrameError = 0x106,
    kIdError = 0x108,
    kSettingsError = 0x109,
    kMissingSettings = 0x10a,
    kRequestRejected = 0x10b,
    kRequestCancelled = 0x10c,
    kMessageError = 0x10e,
    kQpackDecompressionFailed = 0x200,
    kQpackEncoderStreamError = 0x201,
    kQpackDecoderStreamError = 0x202,
    kExcessiveLoad = 0x107,
};

}  // namespace ruvia
