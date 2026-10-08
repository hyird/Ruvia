#include "ruvia/http/Http3RequestWriter.h"

#include <limits>
#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/field/HttpTrailerFields.h"

#include "http3/Http3FieldSectionEncoder.h"

namespace ruvia {
namespace {
using Error = Http3RequestTrailerError;
std::expected<std::pmr::vector<char>, Error> requestTrailers(std::span<const Http3FieldSectionFieldView> fields,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource, Http3QpackEncoder* encoder, std::uint64_t streamId) {
    auto* memory = resource ? resource : std::pmr::get_default_resource();
    if (fields.size() > limits.maxFields) {
        return std::unexpected(Error::kTooManyFields);
    }
    std::size_t decoded = 0, nameBytes = 0;
    for (const auto& field : fields) {
        if (!isValidHttpHeaderName(field.name) || !isValidHttpHeaderValue(field.value)) {
            return std::unexpected(Error::kInvalidField);
        }
        if (detail::isForbiddenHttpRequestTrailerName(field.name)) {
            return std::unexpected(Error::kForbiddenField);
        }
        if (limits.maxDecodedBytes < decoded || limits.maxDecodedBytes - decoded < 32 ||
            field.name.size() > limits.maxDecodedBytes - decoded - 32 ||
            field.value.size() > limits.maxDecodedBytes - decoded - 32 - field.name.size()) {
            return std::unexpected(Error::kFieldListTooLarge);
        }
        decoded += 32 + field.name.size() + field.value.size();
        nameBytes += field.name.size();
    }
    std::pmr::vector<char> names(memory);
    names.reserve(nameBytes);
    std::pmr::vector<Http3FieldSectionFieldView> normalized(memory);
    normalized.reserve(fields.size());
    for (const auto& field : fields) {
        const auto offset = names.size();
        for (const unsigned char ch : field.name) {
            names.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : static_cast<char>(ch));
        }
        normalized.push_back({std::string_view(names.data() + offset, field.name.size()), field.value, field.neverIndexed});
    }
    auto encoded = detail::encodeHttp3Fields(normalized, memory, limits, encoder, streamId);
    if (!encoded) {
        return std::unexpected(encoded.error() == Http3FieldSectionError::kQpackEncodingFailed ? Error::kQpackEncodingFailed : Error::kFieldSectionTooLarge);
    }
    if (encoded->size() > limits.maxEncodedBytes) {
        return std::unexpected(Error::kFieldSectionTooLarge);
    }
    return std::move(*encoded);
}
}  // namespace
std::expected<std::pmr::vector<char>, Error> encodeHttp3RequestTrailers(std::span<const Http3FieldSectionFieldView> fields,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return requestTrailers(fields, limits, resource, nullptr, 0);
}
std::expected<std::pmr::vector<char>, Error> encodeHttp3RequestTrailers(Http3QpackEncoder& encoder, std::uint64_t streamId,
    std::span<const Http3FieldSectionFieldView> fields, Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return requestTrailers(fields, limits, resource, &encoder, streamId);
}
}  // namespace ruvia
