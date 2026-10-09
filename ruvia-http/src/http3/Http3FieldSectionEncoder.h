#pragma once

#include <variant>

#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3QpackConnection.h"

namespace ruvia::detail {

inline std::variant<std::pmr::vector<char>, Http3FieldSectionError> encodeHttp3Fields(
    std::span<const Http3FieldSectionFieldView> fields, std::pmr::memory_resource* resource,
    Http3FieldSectionLimits limits, Http3QpackEncoder* encoder, std::uint64_t streamId) {
    if (!encoder) {
        return encodeHttp3FieldSection(fields, resource);
    }
    auto result = encoder->encode(streamId, fields, limits, resource);
    if ((result.index() != 0)) {
        return std::get<1>(result) == Http3QpackConnectionError::kLimit
                   ? Http3FieldSectionError::kFieldSectionTooLarge
                   : Http3FieldSectionError::kQpackEncodingFailed;
    }
    return std::move(std::get<0>(result));
}

}  // namespace ruvia::detail
