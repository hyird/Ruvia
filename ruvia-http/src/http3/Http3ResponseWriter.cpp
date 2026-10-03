#include "ruvia/http/Http3ResponseWriter.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <vector>

#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/field/HttpInterimResponseValidation.h"
#include "ruvia/http/detail/http3/Http3FieldSectionEncoder.h"
#include "ruvia/http/detail/server/HttpDateCache.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"

namespace ruvia {
namespace {

constexpr bool isTokenChar(unsigned char ch) noexcept {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '!' || ch == '#' || ch == '$' || ch == '%' || ch == '&' || ch == '\'' ||
           ch == '*' || ch == '+' || ch == '-' || ch == '.' || ch == '^' || ch == '_' ||
           ch == '`' || ch == '|' || ch == '~';
}

constexpr bool has_uppercase(std::string_view name) noexcept {
    return std::any_of(name.begin(), name.end(), [](unsigned char ch) {
        return ch >= 'A' && ch <= 'Z';
    });
}

bool add_lowercase_name_bytes(std::size_t& total, std::string_view name) noexcept {
    if (!has_uppercase(name)) {
        return true;
    }
    if (name.size() > std::numeric_limits<std::size_t>::max() - total) {
        return false;
    }
    total += name.size();
    return true;
}

class lowercase_field_names final {
public:
    lowercase_field_names(std::pmr::memory_resource* resource, std::size_t bytes)
        : storage_(resource) {
        storage_.reserve(bytes);
    }

    std::string_view project(std::string_view name) {
        if (!has_uppercase(name)) {
            return name;
        }
        const auto start = storage_.size();
        for (const unsigned char ch : name) {
            storage_.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch));
        }
        return {storage_.data() + start, name.size()};
    }

private:
    std::pmr::vector<char> storage_;
};

constexpr bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        auto ch = static_cast<unsigned char>(lhs[i]);
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<unsigned char>(ch + ('a' - 'A'));
        }
        if (ch != static_cast<unsigned char>(rhs[i])) {
            return false;
        }
    }
    return true;
}

bool validField(const Http3FieldSectionFieldView& field) noexcept {
    if (field.name.empty()) {
        return false;
    }
    for (const unsigned char ch : field.name) {
        if (!isTokenChar(ch)) {
            return false;
        }
    }
    for (const unsigned char ch : field.value) {
        if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
            return false;
        }
    }
    return true;
}

bool validResponseHeader(std::string_view name, std::string_view value) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const unsigned char ch : name) {
        if (!isTokenChar(ch) && !(ch >= 'A' && ch <= 'Z')) {
            return false;
        }
    }
    for (const unsigned char ch : value) {
        if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
            return false;
        }
    }
    return true;
}

std::optional<std::uint64_t> parseContentLength(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    std::uint64_t length = 0;
    for (const unsigned char ch : value) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (length > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        length = length * 10 + digit;
    }
    return length;
}

bool forbiddenField(std::string_view name) noexcept {
    // RFC 9114 permits TE: trailers only in requests, never in responses.
    return equalsIgnoreCase(name, "connection") || equalsIgnoreCase(name, "keep-alive") ||
           equalsIgnoreCase(name, "proxy-connection") || equalsIgnoreCase(name, "transfer-encoding") ||
           equalsIgnoreCase(name, "upgrade") || equalsIgnoreCase(name, "te");
}

bool addDecodedFieldSize(std::size_t& decodedSize, std::string_view name, std::string_view value,
    std::size_t limit) noexcept {
    constexpr std::size_t kFieldOverhead = 32;
    if (decodedSize > limit) {
        return false;
    }
    const auto remaining = limit - decodedSize;
    if (remaining < kFieldOverhead || name.size() > remaining - kFieldOverhead ||
        value.size() > remaining - kFieldOverhead - name.size()) {
        return false;
    }
    decodedSize += kFieldOverhead + name.size() + value.size();
    return true;
}

}  // namespace

static std::expected<Http3ResponseFieldSection, Http3ResponseHeadFailure> encodeResponseTrailers(
    std::span<const Http3FieldSectionFieldView> fields, Http3FieldSectionLimits limits,
    std::pmr::memory_resource* resource, Http3QpackEncoder* encoder, std::uint64_t streamId) {
    if (fields.size() > limits.maxFields) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
            Http3FieldSectionError::kTooManyFields});
    }

    std::size_t decodedBytes = 0;
    std::size_t lowercase_bytes = 0;
    for (const auto& field : fields) {
        if (!detail::isValidResponseTrailerName(field.name) ||
            !detail::isValidResponseTrailerValue(field.value)) {
            return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField});
        }
        if (detail::isForbiddenResponseTrailerName(field.name)) {
            return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kForbiddenField});
        }
        if (!addDecodedFieldSize(decodedBytes, field.name, field.value, limits.maxDecodedBytes)) {
            return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kFieldListTooLarge});
        }
        if (!add_lowercase_name_bytes(lowercase_bytes, field.name)) {
            return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kFieldListTooLarge});
        }
    }

    // Every QPACK field section starts with the two-byte zero required-insert-count/base prefix.
    if (limits.maxEncodedBytes < 2) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
            Http3FieldSectionError::kFieldSectionTooLarge});
    }
    auto* const memory = resource != nullptr ? resource : std::pmr::get_default_resource();
    lowercase_field_names lowercase(memory, lowercase_bytes);
    std::pmr::vector<Http3FieldSectionFieldView> normalized(memory);
    normalized.reserve(fields.size());
    for (const auto& field : fields) {
        normalized.push_back({lowercase.project(field.name), field.value, true});
    }
    auto encoded = detail::encodeHttp3Fields(normalized, memory, limits, encoder, streamId);
    if (!encoded) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError, encoded.error()});
    }
    if (encoded->size() > limits.maxEncodedBytes) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
            Http3FieldSectionError::kFieldSectionTooLarge});
    }
    return Http3ResponseFieldSection(std::move(*encoded), decodedBytes);
}

static std::expected<Http3ResponseHead, Http3ResponseHeadFailure> encodeResponseHead(
    HttpStatusCode status, HttpKnownMethod requestMethod,
    std::span<const Http3FieldSectionFieldView> fields, Http3FieldSectionLimits limits,
    std::pmr::memory_resource* resource, Http3QpackEncoder* encoder, std::uint64_t streamId) {
    if (status == http_status::kSwitchingProtocols) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kUnsupportedStatus});
    }

    const auto bodyPlan = planHttpResponseBody(requestMethod, status);
    std::array<char, 3> statusBytes{};
    const auto statusToken = detail::httpStatusCodeToken(status);
    for (std::size_t i = 0; i < statusBytes.size(); ++i) {
        statusBytes[i] = statusToken[i];
    }

    if (fields.size() >= limits.maxFields) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
            Http3FieldSectionError::kTooManyFields});
    }
    std::pmr::vector<Http3FieldSectionFieldView> outputFields(
        resource != nullptr ? resource : std::pmr::get_default_resource());
    outputFields.reserve(fields.size() + 1);
    outputFields.push_back({":status", std::string_view(statusBytes.data(), statusBytes.size()), false});
    std::optional<std::uint64_t> contentLength;
    for (const auto& field : fields) {
        if (!validField(field)) {
            return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField});
        }
        if (forbiddenField(field.name)) {
            return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kForbiddenField});
        }
        if (equalsIgnoreCase(field.name, "content-length")) {
            const auto parsed = parseContentLength(field.value);
            if (!parsed || (!bodyPlan.explicitContentLengthAllowed() && !(status == http_status::kResetContent && *parsed == 0)) ||
                (contentLength && *contentLength != *parsed)) {
                return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField});
            }
            contentLength = *parsed;
        }
        outputFields.push_back(field);
    }
    std::size_t decodedBytes = 0;
    for (const auto& field : outputFields) {
        if (!addDecodedFieldSize(decodedBytes, field.name, field.value, limits.maxDecodedBytes)) {
            return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kFieldListTooLarge});
        }
    }
    auto encoded = detail::encodeHttp3Fields(outputFields, resource ? resource : std::pmr::get_default_resource(), limits, encoder, streamId);
    if (!encoded) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError, encoded.error()});
    }
    if (encoded->size() > limits.maxEncodedBytes) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
            Http3FieldSectionError::kFieldSectionTooLarge});
    }
    return Http3ResponseHead(std::move(*encoded), bodyPlan, decodedBytes, contentLength);
}

static std::expected<Http3ResponseHead, Http3ResponseHeadFailure> encodeResponseHead(
    const HttpResponse& response, HttpBufferedResponseWritePlan writePlan,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource, Http3QpackEncoder* encoder, std::uint64_t streamId) {
    const auto status = response.status();
    if (status == http_status::kSwitchingProtocols) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kUnsupportedStatus});
    }
    if (writePlan.responseStatus() != status || !writePlan.matchesResponse(response)) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField});
    }

    const auto& headers = response.headers();
    std::size_t contentLengthCount = 0;
    std::optional<std::uint64_t> explicitLengthValue;
    std::size_t nameBytes = 0;
    std::size_t projectedCount = 1;
    bool hasDate = false;
    for (const auto& header : headers) {
        auto name = header.name();
        const auto value = header.value();
        if (!validResponseHeader(name, value)) {
            return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField});
        }
        if (forbiddenField(name)) {
            return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kForbiddenField});
        }
        if (equalsIgnoreCase(name, "date")) {
            hasDate = true;
        }
        if (equalsIgnoreCase(name, "content-length")) {
            ++contentLengthCount;
            const auto parsed = parseContentLength(value);
            if (contentLengthCount > 1 || !parsed || !writePlan.explicitContentLengthAllowed() ||
                (status != http_status::kNotModified && *parsed != writePlan.contentLength())) {
                return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField});
            }
            explicitLengthValue = *parsed;
        }
        if (projectedCount == std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kTooManyFields});
        }
        ++projectedCount;
        if (!add_lowercase_name_bytes(nameBytes, name)) {
            return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kFieldListTooLarge});
        }
    }

    const auto generatedDate = hasDate ? std::string_view{} : detail::cachedDateValue();
    if (!generatedDate.empty()) {
        if (projectedCount == std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(Http3ResponseHeadFailure{
                Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kTooManyFields});
        }
        ++projectedCount;
    }
    const bool synthesizeLength = contentLengthCount == 0 && writePlan.autoContentLengthAllowed();
    if (synthesizeLength) {
        if (projectedCount == std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kTooManyFields});
        }
        ++projectedCount;
    }
    if (projectedCount > limits.maxFields) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
            Http3FieldSectionError::kTooManyFields});
    }
    // QPACK static indices can encode large decoded fields in a single byte;
    // an upper-bound estimate would incorrectly reject a fitting section.
    // The encoder checks the exact encoded size below.
    if (limits.maxEncodedBytes < 2) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
            Http3FieldSectionError::kFieldSectionTooLarge});
    }

    auto* const memory = resource != nullptr ? resource : std::pmr::get_default_resource();
    std::array<char, 20> lengthBytes{};
    const bool emitLength = contentLengthCount != 0 || synthesizeLength;
    const auto canonicalLength = contentLengthCount != 0 && status == http_status::kNotModified
                                     ? *explicitLengthValue
                                     : writePlan.contentLength();
    std::size_t canonicalLengthSize = 0;
    if (emitLength) {
        const auto [end, error] = std::to_chars(lengthBytes.data(), lengthBytes.data() + lengthBytes.size(),
            canonicalLength);
        if (error != std::errc{}) {
            return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField});
        }
        canonicalLengthSize = static_cast<std::size_t>(end - lengthBytes.data());
    }
    lowercase_field_names lowercase(memory, nameBytes);
    std::pmr::vector<Http3FieldSectionFieldView> projected(memory);
    projected.reserve(projectedCount - 1);
    if (!generatedDate.empty()) {
        projected.push_back({"date", generatedDate, false});
    }
    for (const auto& header : headers) {
        const auto original_name = header.name();
        auto value = header.value();
        const bool contentLengthField = equalsIgnoreCase(original_name, "content-length");
        const auto name = lowercase.project(original_name);
        if (contentLengthField) {
            value = std::string_view(lengthBytes.data(), canonicalLengthSize);
        }
        projected.push_back({name, value, false});
    }
    if (synthesizeLength) {
        projected.push_back({"content-length", std::string_view(lengthBytes.data(), canonicalLengthSize), false});
    }
    auto encoded = encodeResponseHead(status, writePlan.requestMethod(), projected, limits, memory, encoder, streamId);
    if (!encoded) {
        return encoded;
    }
    return Http3ResponseHead(std::move(encoded->fieldSection), writePlan.bodyPlan(),
        encoded->decodedFieldSectionSize(), encoded->declaredContentLength);
}

static std::expected<Http3ResponseHead, Http3ResponseHeadFailure> encodeInterimResponseHead(
    const HttpInterimResponseHead& response, Http3FieldSectionLimits limits, std::pmr::memory_resource* resource, Http3QpackEncoder* encoder, std::uint64_t streamId) {
    auto* const memory = resource != nullptr ? resource : std::pmr::get_default_resource();
    if (detail::validateHttpInterimResponseHeaders(response) != detail::HttpInterimResponseHeaderValidationStatus::kOk) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField});
    }
    std::size_t lowercase_bytes = 0;
    for (const auto& field : response.headers()) {
        if (!add_lowercase_name_bytes(lowercase_bytes, field.name())) {
            return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kFieldListTooLarge});
        }
    }
    lowercase_field_names lowercase(memory, lowercase_bytes);
    std::pmr::vector<Http3FieldSectionFieldView> fields(memory);
    fields.reserve(response.headers().size());
    for (const auto& field : response.headers()) {
        fields.push_back({lowercase.project(field.name()), field.value(), false});
    }
    return encodeResponseHead(response.status(), HttpKnownMethod::kGet, fields, limits, memory, encoder, streamId);
}

static std::expected<Http3StreamingResponseHead, Http3ResponseHeadFailure>
encodeStreamingResponseHead(HttpResponse response, HttpKnownMethod method,
    ResponseStreamKind kind, ResponseTrailerIntent trailers, Http3FieldSectionLimits limits,
    std::pmr::memory_resource* resource, Http3QpackEncoder* encoder, std::uint64_t streamId) {
    const auto plan = planHttpResponseStreamCommit(ResponseStreamFraming::kHttp3Frames, method, response.status(), trailers);
    if (response.status().isInformational()) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kUnsupportedStatus});
    }
    if (!plan.trailerIntentAllowed()) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField});
    }
    auto prepared = prepareHttpResponseStreamHead(std::move(response), kind, plan);
    auto* memory = resource != nullptr ? resource : std::pmr::get_default_resource();
    std::size_t lowercase_bytes = 0;
    for (const auto& header : prepared.response().headers()) {
        if (!add_lowercase_name_bytes(lowercase_bytes, header.name())) {
            return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kFieldListTooLarge});
        }
    }
    lowercase_field_names lowercase(memory, lowercase_bytes);
    std::pmr::vector<Http3FieldSectionFieldView> fields(memory);
    fields.reserve(prepared.response().headers().size() + 1);
    bool hasDate = false;
    for (const auto& header : prepared.response().headers()) {
        const auto name = lowercase.project(header.name());
        fields.push_back({name, header.value(), false});
        hasDate = hasDate || name == "date";
    }
    if (!hasDate) {
        fields.push_back({"date", detail::cachedDateValue(), false});
    }
    auto encoded = encodeResponseHead(prepared.response().status(), method, fields, limits, memory, encoder, streamId);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return Http3StreamingResponseHead{std::move(*encoded), plan};
}

std::expected<Http3StreamingResponseHead, Http3ResponseHeadFailure> encodeHttp3StreamingResponseHead(HttpResponse response, HttpKnownMethod method,
    ResponseStreamKind kind, ResponseTrailerIntent trailers, Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeStreamingResponseHead(std::move(response), method, kind, trailers, limits, resource, nullptr, 0);
}
std::expected<Http3StreamingResponseHead, Http3ResponseHeadFailure> encodeHttp3StreamingResponseHead(Http3QpackEncoder& encoder, std::uint64_t streamId, HttpResponse response, HttpKnownMethod method,
    ResponseStreamKind kind, ResponseTrailerIntent trailers, Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeStreamingResponseHead(std::move(response), method, kind, trailers, limits, resource, &encoder, streamId);
}
std::expected<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3InterimResponseHead(const HttpInterimResponseHead& response,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeInterimResponseHead(response, limits, resource, nullptr, 0);
}
std::expected<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3InterimResponseHead(Http3QpackEncoder& encoder, std::uint64_t streamId, const HttpInterimResponseHead& response,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeInterimResponseHead(response, limits, resource, &encoder, streamId);
}

std::expected<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3ResponseHead(
    HttpStatusCode status, HttpKnownMethod method, std::span<const Http3FieldSectionFieldView> fields,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeResponseHead(status, method, fields, limits, resource, nullptr, 0);
}
std::expected<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3ResponseHead(
    const HttpResponse& response, HttpBufferedResponseWritePlan plan,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeResponseHead(response, plan, limits, resource, nullptr, 0);
}
std::expected<Http3ResponseFieldSection, Http3ResponseHeadFailure> encodeHttp3ResponseTrailers(
    std::span<const Http3FieldSectionFieldView> fields,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeResponseTrailers(fields, limits, resource, nullptr, 0);
}

std::expected<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3ResponseHead(
    Http3QpackEncoder& encoder, std::uint64_t streamId, HttpStatusCode status, HttpKnownMethod method, std::span<const Http3FieldSectionFieldView> fields,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeResponseHead(status, method, fields, limits, resource, &encoder, streamId);
}
std::expected<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3ResponseHead(
    Http3QpackEncoder& encoder, std::uint64_t streamId, const HttpResponse& response, HttpBufferedResponseWritePlan plan,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeResponseHead(response, plan, limits, resource, &encoder, streamId);
}
std::expected<Http3ResponseFieldSection, Http3ResponseHeadFailure> encodeHttp3ResponseTrailers(
    Http3QpackEncoder& encoder, std::uint64_t streamId, std::span<const Http3FieldSectionFieldView> fields,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeResponseTrailers(fields, limits, resource, &encoder, streamId);
}

}  // namespace ruvia
