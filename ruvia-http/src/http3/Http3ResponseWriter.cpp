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

#include "ruvia/http/HttpResponseStream.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/field/HttpHeaderSectionSize.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"
#include "ruvia/http/detail/util/AsciiCase.h"

#include "coding/HttpContentLength.h"
#include "field/HttpInterimResponseValidation.h"
#include "field/binary_field_name.h"
#include "http3/Http3FieldSectionEncoder.h"
#include "server/HttpDateCache.h"

namespace ruvia {
namespace {

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
            storage_.push_back(static_cast<char>(httpAsciiToLower(ch)));
        }
        return {storage_.data() + start, name.size()};
    }

private:
    std::pmr::vector<char> storage_;
};

bool validField(const Http3FieldSectionFieldView& field) noexcept {
    if (!detail::is_valid_binary_field_name(field.name)) {
        return false;
    }
    return detail::is_valid_http_field_value_bytes(field.value);
}

// Keep per-field validation in its callers. MSVC otherwise outlines this
// helper inside the response projection loop.
#if defined(_MSC_VER)
#define RUVIA_HTTP3_RESPONSE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define RUVIA_HTTP3_RESPONSE_INLINE inline __attribute__((always_inline))
#else
#define RUVIA_HTTP3_RESPONSE_INLINE inline
#endif

// Own the one projected field list and its generated status bytes together.
// Other names and values are borrowed only through the synchronous encode call.
class response_fields final {
public:
    response_fields(HttpStatusCode status, HttpKnownMethod method, std::size_t capacity,
        Http3FieldSectionLimits limits, std::pmr::memory_resource* resource)
        : body_plan_(planHttpResponseBody(method, status)),
          limits_(limits),
          fields_(resource != nullptr ? resource : std::pmr::get_default_resource()) {
        const auto token = detail::httpStatusCodeToken(status);
        std::copy_n(token.begin(), status_bytes_.size(), status_bytes_.begin());
        const auto status_value = std::string_view(status_bytes_.data(), status_bytes_.size());
        fields_.reserve(capacity);
        fields_.push_back({":status", status_value, false});
    }

    response_fields(const response_fields&) = delete;
    response_fields& operator=(const response_fields&) = delete;
    response_fields(response_fields&&) = delete;
    response_fields& operator=(response_fields&&) = delete;

    [[nodiscard]] RUVIA_HTTP3_RESPONSE_INLINE std::optional<Http3ResponseHeadFailure> append(const Http3FieldSectionFieldView& field) {
        if (!validField(field)) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField};
        }
        if (detail::is_forbidden_http_binary_response_field(field.name)) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kForbiddenField};
        }
        if (httpAsciiEqualsIgnoreCase(field.name, "content-length")) {
            if (content_length_.parse_single_value(field.value) != detail::HttpContentLengthParseStatus::kOk ||
                (!body_plan_.explicitContentLengthAllowed() &&
                    !(body_plan_.responseStatus() == http_status::kResetContent && *content_length_.value() == 0))) {
                return Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField};
            }
        }
        fields_.push_back(field);
        return std::nullopt;
    }

    [[nodiscard]] std::variant<Http3ResponseHead, Http3ResponseHeadFailure> encode(
        Http3QpackEncoder* encoder, std::uint64_t stream_id) const {
        // Validate the aggregate only after every field's grammar and framing.
        detail::HttpHeaderSectionSize section_size(limits_.maxDecodedBytes);
        for (const auto& field : fields_) {
            if (!section_size.add(field.name, field.value)) {
                return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                    Http3FieldSectionError::kFieldListTooLarge};
            }
        }
        auto encoded = detail::encodeHttp3Fields(fields_, fields_.get_allocator().resource(), limits_, encoder, stream_id);
        if ((encoded.index() != 0)) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError, std::get<1>(encoded)};
        }
        if (std::get<0>(encoded).size() > limits_.maxEncodedBytes) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kFieldSectionTooLarge};
        }
        return Http3ResponseHead(std::move(std::get<0>(encoded)), body_plan_, section_size.bytes(), content_length_.value());
    }

private:
    HttpResponseBodyPlan body_plan_;
    Http3FieldSectionLimits limits_;
    detail::HttpContentLengthState<std::uint64_t> content_length_;
    std::array<char, 3> status_bytes_{};
    std::pmr::vector<Http3FieldSectionFieldView> fields_;
};

#undef RUVIA_HTTP3_RESPONSE_INLINE

std::optional<Http3ResponseHeadFailure> response_head_preflight(
    HttpStatusCode status, std::size_t header_count, Http3FieldSectionLimits limits) noexcept {
    if (status == http_status::kSwitchingProtocols) {
        return Http3ResponseHeadFailure{Http3ResponseHeadError::kUnsupportedStatus};
    }
    if (header_count >= limits.maxFields) {
        return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
            Http3FieldSectionError::kTooManyFields};
    }
    return std::nullopt;
}

bool validResponseHeader(std::string_view name, std::string_view value) noexcept {
    return detail::isValidHttpHeaderName(name) && detail::is_valid_http_field_value_bytes(value);
}

}  // namespace

static std::variant<Http3ResponseFieldSection, Http3ResponseHeadFailure> encodeResponseTrailers(
    std::span<const Http3FieldSectionFieldView> fields, Http3FieldSectionLimits limits,
    std::pmr::memory_resource* resource, Http3QpackEncoder* encoder, std::uint64_t streamId) {
    if (fields.size() > limits.maxFields) {
        return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
            Http3FieldSectionError::kTooManyFields};
    }

    detail::HttpHeaderSectionSize sectionSize(limits.maxDecodedBytes);
    std::size_t lowercase_bytes = 0;
    for (const auto& field : fields) {
        if (!detail::is_valid_http_field_name(field.name) ||
            !detail::is_valid_http_field_value(field.value)) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField};
        }
        if (detail::isForbiddenResponseTrailerName(field.name)) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kForbiddenField};
        }
        if (!sectionSize.add(field.name, field.value)) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kFieldListTooLarge};
        }
        if (!add_lowercase_name_bytes(lowercase_bytes, field.name)) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kFieldListTooLarge};
        }
    }

    // Every QPACK field section starts with the two-byte zero required-insert-count/base prefix.
    if (limits.maxEncodedBytes < 2) {
        return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
            Http3FieldSectionError::kFieldSectionTooLarge};
    }
    auto* const memory = resource != nullptr ? resource : std::pmr::get_default_resource();
    lowercase_field_names lowercase(memory, lowercase_bytes);
    std::pmr::vector<Http3FieldSectionFieldView> normalized(memory);
    normalized.reserve(fields.size());
    for (const auto& field : fields) {
        normalized.push_back({lowercase.project(field.name), field.value, true});
    }
    auto encoded = detail::encodeHttp3Fields(normalized, memory, limits, encoder, streamId);
    if ((encoded.index() != 0)) {
        return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError, std::get<1>(encoded)};
    }
    if (std::get<0>(encoded).size() > limits.maxEncodedBytes) {
        return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
            Http3FieldSectionError::kFieldSectionTooLarge};
    }
    return Http3ResponseFieldSection(std::move(std::get<0>(encoded)), sectionSize.bytes());
}

static std::variant<Http3ResponseHead, Http3ResponseHeadFailure> encodeResponseHead(
    HttpStatusCode status, HttpKnownMethod requestMethod,
    std::span<const Http3FieldSectionFieldView> fields, Http3FieldSectionLimits limits,
    std::pmr::memory_resource* resource, Http3QpackEncoder* encoder, std::uint64_t streamId) {
    if (const auto failure = response_head_preflight(status, fields.size(), limits)) {
        return *failure;
    }
    response_fields projected(status, requestMethod, fields.size() + 1, limits, resource);
    for (const auto& field : fields) {
        if (const auto failure = projected.append(field)) {
            return *failure;
        }
    }
    return projected.encode(encoder, streamId);
}

static std::variant<Http3ResponseHead, Http3ResponseHeadFailure> encodeResponseHead(
    const HttpResponse& response, HttpBufferedResponseWritePlan writePlan,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource, Http3QpackEncoder* encoder, std::uint64_t streamId) {
    const auto status = response.status();
    if (status == http_status::kSwitchingProtocols) {
        return Http3ResponseHeadFailure{Http3ResponseHeadError::kUnsupportedStatus};
    }
    if (writePlan.responseStatus() != status || !writePlan.matchesResponse(response)) {
        return Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField};
    }

    const auto& headers = response.headers();
    std::size_t contentLengthCount = 0;
    detail::HttpContentLengthState<std::uint64_t> explicitLength;
    std::size_t nameBytes = 0;
    std::size_t projectedCount = 1;
    bool hasDate = false;
    for (const auto& header : headers) {
        auto name = header.name();
        const auto value = header.value();
        if (!validResponseHeader(name, value)) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField};
        }
        if (detail::is_forbidden_http_binary_response_field(name)) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kForbiddenField};
        }
        if (httpAsciiEqualsIgnoreCase(name, "date")) {
            hasDate = true;
        }
        if (httpAsciiEqualsIgnoreCase(name, "content-length")) {
            ++contentLengthCount;
            if (contentLengthCount > 1 ||
                explicitLength.parse_single_value(value) != detail::HttpContentLengthParseStatus::kOk ||
                !writePlan.explicitContentLengthAllowed() ||
                (status != http_status::kNotModified && *explicitLength.value() != writePlan.contentLength())) {
                return Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField};
            }
        }
        if (projectedCount == std::numeric_limits<std::size_t>::max()) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kTooManyFields};
        }
        ++projectedCount;
        if (!add_lowercase_name_bytes(nameBytes, name)) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kFieldListTooLarge};
        }
    }

    const auto generatedDate = hasDate ? std::string_view{} : detail::cachedDateValue();
    if (!generatedDate.empty()) {
        if (projectedCount == std::numeric_limits<std::size_t>::max()) {
            return Http3ResponseHeadFailure{
                Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kTooManyFields};
        }
        ++projectedCount;
    }
    const bool synthesizeLength = contentLengthCount == 0 && writePlan.autoContentLengthAllowed();
    if (synthesizeLength) {
        if (projectedCount == std::numeric_limits<std::size_t>::max()) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kTooManyFields};
        }
        ++projectedCount;
    }
    if (projectedCount > limits.maxFields) {
        return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
            Http3FieldSectionError::kTooManyFields};
    }
    // QPACK static indices can encode large decoded fields in a single byte;
    // an upper-bound estimate would incorrectly reject a fitting section.
    // The encoder checks the exact encoded size below.
    if (limits.maxEncodedBytes < 2) {
        return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
            Http3FieldSectionError::kFieldSectionTooLarge};
    }

    auto* const memory = resource != nullptr ? resource : std::pmr::get_default_resource();
    std::array<char, 20> lengthBytes{};
    const bool emitLength = contentLengthCount != 0 || synthesizeLength;
    const auto canonicalLength = contentLengthCount != 0 && status == http_status::kNotModified
                                     ? *explicitLength.value()
                                     : writePlan.contentLength();
    std::size_t canonicalLengthSize = 0;
    if (emitLength) {
        const auto [end, error] = std::to_chars(lengthBytes.data(), lengthBytes.data() + lengthBytes.size(),
            canonicalLength);
        if (error != std::errc{}) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField};
        }
        canonicalLengthSize = static_cast<std::size_t>(end - lengthBytes.data());
    }
    lowercase_field_names lowercase(memory, nameBytes);
    response_fields projected(status, writePlan.requestMethod(), projectedCount, limits, memory);
    if (!generatedDate.empty()) {
        if (const auto failure = projected.append({"date", generatedDate, false})) {
            return *failure;
        }
    }
    for (const auto& header : headers) {
        const auto original_name = header.name();
        auto value = header.value();
        const bool contentLengthField = httpAsciiEqualsIgnoreCase(original_name, "content-length");
        const auto name = lowercase.project(original_name);
        if (contentLengthField) {
            value = std::string_view(lengthBytes.data(), canonicalLengthSize);
        }
        if (const auto failure = projected.append({name, value, false})) {
            return *failure;
        }
    }
    if (synthesizeLength) {
        if (const auto failure = projected.append({"content-length", std::string_view(lengthBytes.data(), canonicalLengthSize), false})) {
            return *failure;
        }
    }
    auto encoded = projected.encode(encoder, streamId);
    if ((encoded.index() != 0)) {
        return encoded;
    }
    std::get<0>(encoded).bodyPlan = writePlan.bodyPlan();
    return encoded;
}

static std::variant<Http3ResponseHead, Http3ResponseHeadFailure> encodeInterimResponseHead(
    const HttpInterimResponseHead& response, Http3FieldSectionLimits limits, std::pmr::memory_resource* resource, Http3QpackEncoder* encoder, std::uint64_t streamId) {
    auto* const memory = resource != nullptr ? resource : std::pmr::get_default_resource();
    if (detail::validateHttpInterimResponseHeaders(response) != detail::HttpInterimResponseHeaderValidationStatus::kOk) {
        return Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField};
    }
    if (const auto failure = response_head_preflight(response.status(), response.headers().size(), limits)) {
        return *failure;
    }
    std::size_t lowercase_bytes = 0;
    for (const auto& field : response.headers()) {
        if (!add_lowercase_name_bytes(lowercase_bytes, field.name())) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kFieldListTooLarge};
        }
    }
    lowercase_field_names lowercase(memory, lowercase_bytes);
    response_fields fields(response.status(), HttpKnownMethod::kGet, response.headers().size() + 1, limits, memory);
    for (const auto& field : response.headers()) {
        if (const auto failure = fields.append({lowercase.project(field.name()), field.value(), false})) {
            return *failure;
        }
    }
    return fields.encode(encoder, streamId);
}

static std::variant<Http3StreamingResponseHead, Http3ResponseHeadFailure>
encodeStreamingResponseHead(HttpResponse response, HttpKnownMethod method,
    http_response_stream_kind kind, http_response_trailer_intent trailers, Http3FieldSectionLimits limits,
    std::pmr::memory_resource* resource, Http3QpackEncoder* encoder, std::uint64_t streamId) {
    const auto plan = plan_http_response_stream_commit(http_response_stream_framing::http3_frames, method, response.status(), trailers);
    if (response.status().isInformational()) {
        return Http3ResponseHeadFailure{Http3ResponseHeadError::kUnsupportedStatus};
    }
    if (!plan.trailer_intent_allowed()) {
        return Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField};
    }
    auto prepared = prepare_http_response_stream_head(std::move(response), kind, plan);
    if (const auto failure = response_head_preflight(prepared.response().status(), prepared.response().headers().size(), limits)) {
        return *failure;
    }
    auto* memory = resource != nullptr ? resource : std::pmr::get_default_resource();
    std::size_t lowercase_bytes = 0;
    bool hasDate = false;
    for (const auto& header : prepared.response().headers()) {
        hasDate = hasDate || httpAsciiEqualsIgnoreCase(header.name(), "date");
        if (!add_lowercase_name_bytes(lowercase_bytes, header.name())) {
            return Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError,
                Http3FieldSectionError::kFieldListTooLarge};
        }
    }
    const auto header_count = prepared.response().headers().size() + (hasDate ? 0U : 1U);
    if (const auto failure = response_head_preflight(prepared.response().status(), header_count, limits)) {
        return *failure;
    }
    lowercase_field_names lowercase(memory, lowercase_bytes);
    response_fields fields(prepared.response().status(), method, header_count + 1, limits, memory);
    for (const auto& header : prepared.response().headers()) {
        const auto name = lowercase.project(header.name());
        if (const auto failure = fields.append({name, header.value(), false})) {
            return *failure;
        }
    }
    if (!hasDate) {
        if (const auto failure = fields.append({"date", detail::cachedDateValue(), false})) {
            return *failure;
        }
    }
    auto encoded = fields.encode(encoder, streamId);
    if ((encoded.index() != 0)) {
        return std::get<1>(encoded);
    }
    return Http3StreamingResponseHead{std::move(std::get<0>(encoded)), plan};
}

std::variant<Http3StreamingResponseHead, Http3ResponseHeadFailure> encodeHttp3StreamingResponseHead(HttpResponse response, HttpKnownMethod method,
    http_response_stream_kind kind, http_response_trailer_intent trailers, Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeStreamingResponseHead(std::move(response), method, kind, trailers, limits, resource, nullptr, 0);
}
std::variant<Http3StreamingResponseHead, Http3ResponseHeadFailure> encodeHttp3StreamingResponseHead(Http3QpackEncoder& encoder, std::uint64_t streamId, HttpResponse response, HttpKnownMethod method,
    http_response_stream_kind kind, http_response_trailer_intent trailers, Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeStreamingResponseHead(std::move(response), method, kind, trailers, limits, resource, &encoder, streamId);
}
std::variant<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3InterimResponseHead(const HttpInterimResponseHead& response,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeInterimResponseHead(response, limits, resource, nullptr, 0);
}
std::variant<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3InterimResponseHead(Http3QpackEncoder& encoder, std::uint64_t streamId, const HttpInterimResponseHead& response,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeInterimResponseHead(response, limits, resource, &encoder, streamId);
}

std::variant<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3ResponseHead(
    HttpStatusCode status, HttpKnownMethod method, std::span<const Http3FieldSectionFieldView> fields,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeResponseHead(status, method, fields, limits, resource, nullptr, 0);
}
std::variant<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3ResponseHead(
    const HttpResponse& response, HttpBufferedResponseWritePlan plan,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeResponseHead(response, plan, limits, resource, nullptr, 0);
}
std::variant<Http3ResponseFieldSection, Http3ResponseHeadFailure> encodeHttp3ResponseTrailers(
    std::span<const Http3FieldSectionFieldView> fields,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeResponseTrailers(fields, limits, resource, nullptr, 0);
}

std::variant<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3ResponseHead(
    Http3QpackEncoder& encoder, std::uint64_t streamId, HttpStatusCode status, HttpKnownMethod method, std::span<const Http3FieldSectionFieldView> fields,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeResponseHead(status, method, fields, limits, resource, &encoder, streamId);
}
std::variant<Http3ResponseHead, Http3ResponseHeadFailure> encodeHttp3ResponseHead(
    Http3QpackEncoder& encoder, std::uint64_t streamId, const HttpResponse& response, HttpBufferedResponseWritePlan plan,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeResponseHead(response, plan, limits, resource, &encoder, streamId);
}
std::variant<Http3ResponseFieldSection, Http3ResponseHeadFailure> encodeHttp3ResponseTrailers(
    Http3QpackEncoder& encoder, std::uint64_t streamId, std::span<const Http3FieldSectionFieldView> fields,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeResponseTrailers(fields, limits, resource, &encoder, streamId);
}

}  // namespace ruvia
