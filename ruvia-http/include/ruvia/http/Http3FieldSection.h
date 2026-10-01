#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

namespace ruvia {

enum class Http3FieldSectionError : std::uint8_t {
    kNeedMoreData,
    kIntegerOverflow,
    kInvalidPrefix,
    kNonzeroRequiredInsertCount,
    kInvalidBase,
    kDynamicReference,
    kInvalidIndex,
    kInvalidHuffman,
    kFieldSectionTooLarge,
    kFieldListTooLarge,
    kTooManyFields,
    kOutputTooSmall,
    kCallbackStopped,
    kQpackEncodingFailed,
};

struct Http3FieldSectionFieldView final {
    std::string_view name;
    std::string_view value;
    bool neverIndexed{false};
};

struct Http3FieldSectionLimits final {
    std::size_t maxEncodedBytes{64 * 1024};
    // RFC 9114 field-list size: name bytes + value bytes + 32 per field.
    std::size_t maxDecodedBytes{64 * 1024};
    std::size_t maxFields{256};
};

using Http3FieldSectionCallback = bool (*)(void*, Http3FieldSectionFieldView);

// Decodes a QPACK field section with a permanently empty dynamic table. Callback
// views borrow the input and remain valid only for the duration of the callback.
[[nodiscard]] std::expected<std::size_t, Http3FieldSectionError> decodeHttp3FieldSection(
    std::span<const char> input, Http3FieldSectionCallback callback, void* context,
    Http3FieldSectionLimits limits = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

// Encodes fields using exact static-table matches, static name references, or
// literals. The returned bytes are owned by the supplied PMR resource.
[[nodiscard]] std::expected<std::pmr::vector<char>, Http3FieldSectionError> encodeHttp3FieldSection(
    std::span<const Http3FieldSectionFieldView> fields, std::pmr::memory_resource* resource);

}  // namespace ruvia
