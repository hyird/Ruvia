#include "ruvia/http/Http3Settings.h"

#include <array>
#include <memory_resource>
#include <unordered_set>
#include <variant>

#include "ruvia/http/Http3VarInt.h"

namespace ruvia {
namespace {

constexpr std::uint64_t kQpackMaxTableCapacity = 0x1;
constexpr std::uint64_t kMaxFieldSectionSize = 0x6;
constexpr std::uint64_t kQpackBlockedStreams = 0x7;
constexpr std::uint64_t kEnableConnectProtocol = 0x8;
constexpr std::uint64_t kH3Datagram = 0x33;

[[nodiscard]] constexpr bool isForbiddenSetting(std::uint64_t identifier) noexcept {
    return identifier == 0 || (identifier >= 0x2 && identifier <= 0x5);
}

}  // namespace

std::variant<Http3Settings, Http3SettingsError> decodeHttp3Settings(
    std::span<const char> payload, std::pmr::memory_resource* resource) {
    if (resource == nullptr) {
        resource = std::pmr::get_default_resource();
    }
    std::pmr::unordered_set<std::uint64_t> identifiers(resource);
    Http3Settings settings;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto identifier = decodeHttp3VarInt(payload.subspan(offset));
        if ((identifier.index() != 0)) {
            return Http3SettingsError::kNeedMoreData;
        }
        offset += std::get<0>(identifier).encodedBytes;
        const auto value = decodeHttp3VarInt(payload.subspan(offset));
        if ((value.index() != 0)) {
            return Http3SettingsError::kNeedMoreData;
        }
        offset += std::get<0>(value).encodedBytes;

        if (!identifiers.insert(std::get<0>(identifier).value).second) {
            return Http3SettingsError::kDuplicateIdentifier;
        }
        if (isForbiddenSetting(std::get<0>(identifier).value)) {
            return Http3SettingsError::kForbiddenIdentifier;
        }
        switch (std::get<0>(identifier).value) {
            case kQpackMaxTableCapacity:
                settings.qpackMaxTableCapacity = std::get<0>(value).value;
                break;
            case kMaxFieldSectionSize:
                settings.maxFieldSectionSize = std::get<0>(value).value;
                break;
            case kQpackBlockedStreams:
                settings.qpackBlockedStreams = std::get<0>(value).value;
                break;
            case kEnableConnectProtocol:
                if (std::get<0>(value).value > 1) {
                    return Http3SettingsError::kValueOutOfRange;
                }
                settings.enableConnectProtocol = std::get<0>(value).value == 1;
                break;
            case kH3Datagram:
                if (std::get<0>(value).value > 1) {
                    return Http3SettingsError::kValueOutOfRange;
                }
                settings.h3Datagram = std::get<0>(value).value == 1;
                break;
            default:
                break;
        }
    }
    return settings;
}

std::variant<std::size_t, Http3SettingsError> encodeHttp3Settings(
    std::span<char> output, const Http3Settings& settings) noexcept {
    constexpr std::array<std::uint64_t, 5> identifiers{
        kQpackMaxTableCapacity, kMaxFieldSectionSize, kQpackBlockedStreams, kEnableConnectProtocol, kH3Datagram};
    const std::array<std::optional<std::uint64_t>, 5> values{settings.qpackMaxTableCapacity,
        settings.maxFieldSectionSize, settings.qpackBlockedStreams,
        settings.enableConnectProtocol ? std::optional<std::uint64_t>{1} : std::nullopt,
        settings.h3Datagram ? std::optional<std::uint64_t>{1} : std::nullopt};
    std::size_t required = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!values[i]) {
            continue;
        }
        if (*values[i] > kHttp3VarIntMax) {
            return Http3SettingsError::kValueOutOfRange;
        }
        required += http3VarIntEncodedSize(identifiers[i]) + http3VarIntEncodedSize(*values[i]);
    }
    if (output.size() < required) {
        return Http3SettingsError::kOutputTooSmall;
    }

    std::size_t offset = 0;
    for (std::size_t i = 0; i < identifiers.size(); ++i) {
        if (!values[i]) {
            continue;
        }
        for (const auto value : {identifiers[i], *values[i]}) {
            const auto written = encodeHttp3VarInt(output.subspan(offset), value);
            if ((written.index() != 0)) {
                return Http3SettingsError::kValueOutOfRange;
            }
            offset += std::get<0>(written);
        }
    }
    return offset;
}

}  // namespace ruvia
