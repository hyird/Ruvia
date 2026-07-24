#pragma once

#include <memory_resource>
#include <string_view>
#include <vector>

#include "ruvia/http/MultipartParser.h"
#include "ruvia/web/ContextRequest.h"

// Turning a buffered request body into form data: the urlencoded and
// multipart/form-data grammars, the dotted-name and array-suffix policies applied
// to field names, and the prototype-pollution guard. Reading the result back is
// RequestFormData's own job.

namespace ruvia::detail {

// Parse `requestBody` according to `contentType`. A content type that is neither
// form grammar yields empty form data; a malformed body or boundary throws the
// matching protocol error.
[[nodiscard]] ContextRequest::RequestFormData parseFormBodyFromView(std::string_view contentType, std::string_view requestBody, std::pmr::memory_resource* resource, ContextRequest::ParseBodyOptions options);

// Parse a complete multipart body into its parts, throwing the protocol error a
// malformed body commits.
[[nodiscard]] std::pmr::vector<MultipartPart> parseCompleteMultipartBody(std::string_view requestBody, MultipartBoundary boundary, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
