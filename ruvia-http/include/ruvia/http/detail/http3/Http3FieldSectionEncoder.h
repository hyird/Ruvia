#pragma once

#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3QpackConnection.h"

namespace ruvia::detail {

inline std::expected<std::pmr::vector<char>, Http3FieldSectionError> encodeHttp3Fields(
    std::span<const Http3FieldSectionFieldView> fields, std::pmr::memory_resource* resource,
    Http3FieldSectionLimits limits, Http3QpackEncoder* encoder, std::uint64_t streamId) {
    if (!encoder) {
        return encodeHttp3FieldSection(fields, resource);
    }
    auto result = encoder->encode(streamId, fields, limits);
    if (!result) {
        return std::unexpected(result.error() == Http3QpackConnectionError::kLimit
                                   ? Http3FieldSectionError::kFieldSectionTooLarge
                                   : Http3FieldSectionError::kQpackEncodingFailed);
    }
    // Detached results have the caller's lifetime, independent of the encoder.
    if (result->get_allocator().resource()->is_equal(*resource)) {
        return std::move(*result);
    }
    return std::pmr::vector<char>(result->begin(), result->end(), resource);
}

}  // namespace ruvia::detail
