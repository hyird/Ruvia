#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/Http3FieldSection.h"

namespace ruvia {

enum class Http3MessageHeadKind : std::uint8_t { kRequest,
    kResponse };

// Errors are scoped to processing a HEADERS field section: malformed decoded
// HTTP semantics map to H3_MESSAGE_ERROR; failures from QPACK decoding map to
// QPACK_DECOMPRESSION_FAILED. Resource exceptions propagate to the caller.
enum class Http3MessageHeadError : std::uint8_t {
    kMessageError,
    kQpackDecompressionFailed,
    kFieldSectionTooLarge,
};

struct Http3MessageHeader final {
    std::pmr::string name;
    std::pmr::string value;

    explicit Http3MessageHeader(std::pmr::memory_resource* resource);
    Http3MessageHeader(std::string_view name, std::string_view value, std::pmr::memory_resource* resource);
};

// All strings and headers are owned by resource. The resource must outlive this
// value; destroying the value returns its allocations to that resource.
struct Http3MessageHead final {
    explicit Http3MessageHead(std::pmr::memory_resource* resource);

    std::pmr::string method;
    std::pmr::string protocol;
    std::pmr::string scheme;
    std::pmr::string authority;
    std::pmr::string path;
    std::uint16_t status{0};
    std::optional<std::uint64_t> contentLength{};
    std::pmr::vector<Http3MessageHeader> headers;
};

struct Http3MessageHeadLimits final {
    std::size_t maxFieldSectionSize{64 * 1024};
    std::size_t maxFields{256};
    std::size_t maxEncodedBytes{64 * 1024};
};

// Consumes a complete QPACK-encoded field section. maxFieldSectionSize
// counts each field as name + value + 32, per RFC 9114 Section 4.2.2; it is not
// an encoded-byte limit. Only ordinary fields are returned in headers.
[[nodiscard]] std::expected<Http3MessageHead, Http3MessageHeadError> decodeHttp3MessageHead(
    std::span<const char> fieldSection, Http3MessageHeadKind kind,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
    Http3MessageHeadLimits limits = {});

}  // namespace ruvia
