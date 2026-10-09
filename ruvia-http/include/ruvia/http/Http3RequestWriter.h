#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <variant>
#include <vector>

#include "ruvia/http/Http3FieldSection.h"

namespace ruvia {
class Http3QpackEncoder;
enum class Http3RequestTrailerError : std::uint8_t {
    kInvalidField,
    kForbiddenField,
    kTooManyFields,
    kFieldListTooLarge,
    kFieldSectionTooLarge,
    kQpackEncodingFailed,
};
// QPACK payload for a request's final trailing HEADERS. Fields borrow this call;
// output uses resource, which must outlive the returned bytes. Send a HEADERS
// frame with FIN after the request DATA plan permits completion. CONNECT tunnels
// do not carry trailers. The transport owns the head/DATA/trailer write ordering.
[[nodiscard]] std::variant<std::pmr::vector<char>, Http3RequestTrailerError> encodeHttp3RequestTrailers(
    std::span<const Http3FieldSectionFieldView> fields, Http3FieldSectionLimits limits = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::variant<std::pmr::vector<char>, Http3RequestTrailerError> encodeHttp3RequestTrailers(
    Http3QpackEncoder& encoder, std::uint64_t streamId, std::span<const Http3FieldSectionFieldView> fields,
    Http3FieldSectionLimits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
}  // namespace ruvia
