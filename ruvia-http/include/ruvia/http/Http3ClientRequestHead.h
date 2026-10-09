#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/Http3FieldSection.h"

namespace ruvia {

// All views are borrowed only for the duration of encodeHttp3ClientRequestHead.
struct Http3ClientRequestHeadView final {
    std::string_view method{};
    std::string_view scheme{};
    std::string_view authority{};
    std::string_view path{};
    std::span<const Http3FieldSectionFieldView> fields{};
    std::optional<std::uint64_t> bodyLength{};
    // Suppress automatic Content-Length generation while still validating an
    // explicitly supplied field against bodyLength.
    bool emit_content_length{true};
    // RFC 9220 Extended CONNECT; only valid with method CONNECT.
    std::string_view protocol{};
    // Must come from the received peer SETTINGS, not local configuration.
    bool peerEnableConnectProtocol{false};
};

enum class Http3ClientRequestHeadError : std::uint8_t {
    kInvalidMethod,
    kInvalidTarget,
    kInvalidAuthority,
    kInvalidField,
    kForbiddenField,
    kInvalidContentLength,
    kInvalidProtocol,
    kConnectProtocolDisabled,
    kFieldSectionError,
};

struct Http3ClientRequestBodyPlan final {
    std::optional<std::uint64_t> expectedLength{};
    [[nodiscard]] bool matches(std::uint64_t bytes) const noexcept {
        return !expectedLength || *expectedLength == bytes;
    }
};

struct Http3ClientRequestHead final {
    // Both values are owned by the resource supplied to the encoder.
    std::pmr::vector<char> fieldSection;
    Http3ClientRequestBodyPlan bodyPlan{};

    explicit Http3ClientRequestHead(std::pmr::memory_resource* resource)
        : fieldSection(resource) {}
};

struct Http3ClientRequestHeadFailure final {
    Http3ClientRequestHeadError kind;
    Http3FieldSectionError fieldSectionError{Http3FieldSectionError::kInvalidPrefix};
};

// Produces the QPACK field-section payload (not an HTTP/3 frame). QPACK's
// dynamic table capacity is zero; body bytes are never copied or concatenated.
[[nodiscard]] std::variant<Http3ClientRequestHead, Http3ClientRequestHeadFailure>
encodeHttp3ClientRequestHead(Http3ClientRequestHeadView view,
    Http3FieldSectionLimits limits = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

class Http3QpackEncoder;
[[nodiscard]] std::variant<Http3ClientRequestHead, Http3ClientRequestHeadFailure> encodeHttp3ClientRequestHead(
    Http3QpackEncoder& encoder, std::uint64_t streamId, Http3ClientRequestHeadView view,
    Http3FieldSectionLimits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());

}  // namespace ruvia
