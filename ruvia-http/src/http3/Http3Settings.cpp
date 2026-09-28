#include "ruvia/http/Http3Settings.h"

#include <array>
#include <memory_resource>
#include <unordered_set>

#include "ruvia/http/Http3VarInt.h"

namespace ruvia {
namespace {

constexpr std::uint64_t kQpackMaxTableCapacity = 0x1;
constexpr std::uint64_t kMaxFieldSectionSize = 0x6;
constexpr std::uint64_t kQpackBlockedStreams = 0x7;

[[nodiscard]] constexpr bool isForbiddenSetting(std::uint64_t identifier) noexcept {
    return identifier == 0 || (identifier >= 0x2 && identifier <= 0x5);
}

}  // namespace

std::expected<Http3Settings, Http3SettingsError> decodeHttp3Settings(
    std::span<const char> payload, std::pmr::memory_resource* resource) {
    if (resource == nullptr) {
        resource = std::pmr::get_default_resource();
    }
    std::pmr::unordered_set<std::uint64_t> identifiers(resource);
    Http3Settings settings;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto identifier = decodeHttp3VarInt(payload.subspan(offset));
        if (!identifier) {
            return std::unexpected(Http3SettingsError::kNeedMoreData);
        }
        offset += identifier->encodedBytes;
        const auto value = decodeHttp3VarInt(payload.subspan(offset));
        if (!value) {
            return std::unexpected(Http3SettingsError::kNeedMoreData);
        }
        offset += value->encodedBytes;

        if (!identifiers.insert(identifier->value).second) {
            return std::unexpected(Http3SettingsError::kDuplicateIdentifier);
        }
        if (isForbiddenSetting(identifier->value)) {
            return std::unexpected(Http3SettingsError::kForbiddenIdentifier);
        }
        switch (identifier->value) {
            case kQpackMaxTableCapacity:
                settings.qpackMaxTableCapacity = value->value;
                break;
            case kMaxFieldSectionSize:
                settings.maxFieldSectionSize = value->value;
                break;
            case kQpackBlockedStreams:
                settings.qpackBlockedStreams = value->value;
                break;
            default:
                break;
        }
    }
    return settings;
}

std::expected<std::size_t, Http3SettingsError> encodeHttp3Settings(
    std::span<char> output, const Http3Settings& settings) noexcept {
    constexpr std::array<std::uint64_t, 3> identifiers{
        kQpackMaxTableCapacity, kMaxFieldSectionSize, kQpackBlockedStreams};
    const std::array<std::optional<std::uint64_t>, 3> values{settings.qpackMaxTableCapacity,
        settings.maxFieldSectionSize, settings.qpackBlockedStreams};
    std::size_t required = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!values[i]) {
            continue;
        }
        if (*values[i] > kHttp3VarIntMax) {
            return std::unexpected(Http3SettingsError::kValueOutOfRange);
        }
        required += http3VarIntEncodedSize(identifiers[i]) + http3VarIntEncodedSize(*values[i]);
    }
    if (output.size() < required) {
        return std::unexpected(Http3SettingsError::kOutputTooSmall);
    }

    std::size_t offset = 0;
    for (std::size_t i = 0; i < identifiers.size(); ++i) {
        if (!values[i]) {
            continue;
        }
        for (const auto value : {identifiers[i], *values[i]}) {
            const auto written = encodeHttp3VarInt(output.subspan(offset), value);
            if (!written) {
                return std::unexpected(Http3SettingsError::kValueOutOfRange);
            }
            offset += *written;
        }
    }
    return offset;
}

}  // namespace ruvia
