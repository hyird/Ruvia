#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <optional>
#include <span>

namespace ruvia {

enum class Http3SettingsError : std::uint8_t {
    kNeedMoreData,
    kDuplicateIdentifier,
    kForbiddenIdentifier,
    kValueOutOfRange,
    kOutputTooSmall,
};

struct Http3Settings final {
    std::uint64_t qpackMaxTableCapacity{0};
    // Absent means no advertised limit; an explicit zero forbids nonempty sections.
    std::optional<std::uint64_t> maxFieldSectionSize{};
    std::uint64_t qpackBlockedStreams{0};
    // RFC 9220 SETTINGS_ENABLE_CONNECT_PROTOCOL; false is the omitted default.
    bool enableConnectProtocol{false};
};

// Decodes a complete SETTINGS payload (RFC 9114 §7.2.4.1). Unknown settings are
// validated and ignored; `resource` owns temporary duplicate-detection storage.
[[nodiscard]] std::expected<Http3Settings, Http3SettingsError> decodeHttp3Settings(
    std::span<const char> payload,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

// Always emits both QPACK settings; emits the field-section limit only if present
// and ENABLE_CONNECT_PROTOCOL only when enabled.
[[nodiscard]] std::expected<std::size_t, Http3SettingsError> encodeHttp3Settings(
    std::span<char> output, const Http3Settings& settings) noexcept;

}  // namespace ruvia
