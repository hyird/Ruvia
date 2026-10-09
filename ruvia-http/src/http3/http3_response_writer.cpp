#include "ruvia/http/http3_response_writer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <vector>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/field/http_header_section_size.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/detail/util/ascii_case.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/http_status.h"

#include "coding/http_content_length.h"
#include "field/binary_field_name.h"
#include "field/http_interim_response_validation.h"
#include "http3/http3_field_section_encoder.h"
#include "server/http_date_cache.h"

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
    lowercase_field_names(std::pmr::memory_resource* resource, std::size_t bytes_value)
        : storage_(resource) {
        storage_.reserve(bytes_value);
    }

    std::string_view project(std::string_view name) {
        if (!has_uppercase(name)) {
            return name;
        }
        const auto start = storage_.size();
        for (const unsigned char ch : name) {
            storage_.push_back(static_cast<char>(http_ascii_to_lower(ch)));
        }
        return {storage_.data() + start, name.size()};
    }

private:
    std::pmr::vector<char> storage_;
};

bool valid_field(const http3_field_section_field_view& field) noexcept {
    if (!detail::is_valid_binary_field_name(field.name_)) {
        return false;
    }
    return detail::is_valid_http_field_value_bytes(field.value_);
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
    response_fields(http_status_code status, http_known_method method, std::size_t capacity,
        http3_field_section_limits limits, std::pmr::memory_resource* resource)
        : body_plan_(plan_http_response_body(method, status)),
          limits_(limits),
          fields_(resource != nullptr ? resource : std::pmr::get_default_resource()) {
        const auto token = detail::http_status_code_token(status);
        std::copy_n(token.begin(), status_bytes_.size(), status_bytes_.begin());
        const auto status_value = std::string_view(status_bytes_.data(), status_bytes_.size());
        fields_.reserve(capacity);
        fields_.push_back({":status", status_value, false});
    }

    response_fields(const response_fields&) = delete;
    response_fields& operator=(const response_fields&) = delete;
    response_fields(response_fields&&) = delete;
    response_fields& operator=(response_fields&&) = delete;

    [[nodiscard]] RUVIA_HTTP3_RESPONSE_INLINE std::optional<http3_response_head_failure> append(const http3_field_section_field_view& field) {
        if (!valid_field(field)) {
            return http3_response_head_failure{http3_response_head_error::invalid_field};
        }
        if (detail::is_forbidden_http_binary_response_field(field.name_)) {
            return http3_response_head_failure{http3_response_head_error::forbidden_field};
        }
        if (http_ascii_equals_ignore_case(field.name_, "content-length")) {
            if (content_length_.parse_single_value(field.value_) != detail::http_content_length_parse_status::ok ||
                (!body_plan_.explicit_content_length_allowed() &&
                    !(body_plan_.response_status() == http_status::reset_content && *content_length_.value() == 0))) {
                return http3_response_head_failure{http3_response_head_error::invalid_field};
            }
        }
        fields_.push_back(field);
        return std::nullopt;
    }

    [[nodiscard]] std::variant<http3_response_head, http3_response_head_failure> encode(
        http3_qpack_encoder* encoder, std::uint64_t stream_id) const {
        // Validate the aggregate only after every field's grammar and framing.
        detail::http_header_section_size section_size(limits_.max_decoded_bytes_);
        for (const auto& field : fields_) {
            if (!section_size.add(field.name_, field.value_)) {
                return http3_response_head_failure{http3_response_head_error::field_section_error,
                    http3_field_section_error::field_list_too_large};
            }
        }
        auto encoded = detail::encode_http3_fields(fields_, fields_.get_allocator().resource(), limits_, encoder, stream_id);
        if ((encoded.index() != 0)) {
            return http3_response_head_failure{http3_response_head_error::field_section_error, std::get<1>(encoded)};
        }
        if (std::get<0>(encoded).size() > limits_.max_encoded_bytes_) {
            return http3_response_head_failure{http3_response_head_error::field_section_error,
                http3_field_section_error::field_section_too_large};
        }
        return http3_response_head(std::move(std::get<0>(encoded)), body_plan_, section_size.bytes(), content_length_.value());
    }

private:
    http_response_body_plan body_plan_;
    http3_field_section_limits limits_;
    detail::http_content_length_state<std::uint64_t> content_length_;
    std::array<char, 3> status_bytes_{};
    std::pmr::vector<http3_field_section_field_view> fields_;
};

#undef RUVIA_HTTP3_RESPONSE_INLINE

std::optional<http3_response_head_failure> response_head_preflight(
    http_status_code status, std::size_t header_count, http3_field_section_limits limits) noexcept {
    if (status == http_status::switching_protocols) {
        return http3_response_head_failure{http3_response_head_error::unsupported_status};
    }
    if (header_count >= limits.max_fields_) {
        return http3_response_head_failure{http3_response_head_error::field_section_error,
            http3_field_section_error::too_many_fields};
    }
    return std::nullopt;
}

bool valid_response_header(std::string_view name, std::string_view value) noexcept {
    return detail::is_valid_http_header_name(name) && detail::is_valid_http_field_value_bytes(value);
}

}  // namespace

static std::variant<http3_response_field_section, http3_response_head_failure> encode_response_trailers(
    std::span<const http3_field_section_field_view> fields_value, http3_field_section_limits limits,
    std::pmr::memory_resource* resource, http3_qpack_encoder* encoder, std::uint64_t stream_id) {
    if (fields_value.size() > limits.max_fields_) {
        return http3_response_head_failure{http3_response_head_error::field_section_error,
            http3_field_section_error::too_many_fields};
    }

    detail::http_header_section_size section_size(limits.max_decoded_bytes_);
    std::size_t lowercase_bytes = 0;
    for (const auto& field : fields_value) {
        if (!detail::is_valid_http_field_name(field.name_) ||
            !detail::is_valid_http_field_value(field.value_)) {
            return http3_response_head_failure{http3_response_head_error::invalid_field};
        }
        if (detail::is_forbidden_response_trailer_name(field.name_)) {
            return http3_response_head_failure{http3_response_head_error::forbidden_field};
        }
        if (!section_size.add(field.name_, field.value_)) {
            return http3_response_head_failure{http3_response_head_error::field_section_error,
                http3_field_section_error::field_list_too_large};
        }
        if (!add_lowercase_name_bytes(lowercase_bytes, field.name_)) {
            return http3_response_head_failure{http3_response_head_error::field_section_error,
                http3_field_section_error::field_list_too_large};
        }
    }

    // Every QPACK field section starts with the two-byte zero required-insert-count/base prefix.
    if (limits.max_encoded_bytes_ < 2) {
        return http3_response_head_failure{http3_response_head_error::field_section_error,
            http3_field_section_error::field_section_too_large};
    }
    auto* const memory = resource != nullptr ? resource : std::pmr::get_default_resource();
    lowercase_field_names lowercase(memory, lowercase_bytes);
    std::pmr::vector<http3_field_section_field_view> normalized(memory);
    normalized.reserve(fields_value.size());
    for (const auto& field : fields_value) {
        normalized.push_back({lowercase.project(field.name_), field.value_, true});
    }
    auto encoded = detail::encode_http3_fields(normalized, memory, limits, encoder, stream_id);
    if ((encoded.index() != 0)) {
        return http3_response_head_failure{http3_response_head_error::field_section_error, std::get<1>(encoded)};
    }
    if (std::get<0>(encoded).size() > limits.max_encoded_bytes_) {
        return http3_response_head_failure{http3_response_head_error::field_section_error,
            http3_field_section_error::field_section_too_large};
    }
    return http3_response_field_section(std::move(std::get<0>(encoded)), section_size.bytes());
}

static std::variant<http3_response_head, http3_response_head_failure> encode_response_head(
    http_status_code status, http_known_method request_method,
    std::span<const http3_field_section_field_view> fields_value, http3_field_section_limits limits,
    std::pmr::memory_resource* resource, http3_qpack_encoder* encoder, std::uint64_t stream_id) {
    if (const auto failure = response_head_preflight(status, fields_value.size(), limits)) {
        return *failure;
    }
    response_fields projected(status, request_method, fields_value.size() + 1, limits, resource);
    for (const auto& field : fields_value) {
        if (const auto failure = projected.append(field)) {
            return *failure;
        }
    }
    return projected.encode(encoder, stream_id);
}

static std::variant<http3_response_head, http3_response_head_failure> encode_response_head(
    const http_response& response, http_buffered_response_write_plan write_plan,
    http3_field_section_limits limits, std::pmr::memory_resource* resource, http3_qpack_encoder* encoder, std::uint64_t stream_id) {
    const auto status = response.status();
    if (status == http_status::switching_protocols) {
        return http3_response_head_failure{http3_response_head_error::unsupported_status};
    }
    if (write_plan.response_status() != status || !write_plan.matches_response(response)) {
        return http3_response_head_failure{http3_response_head_error::invalid_field};
    }

    const auto& headers = response.headers();
    std::size_t content_length_count = 0;
    detail::http_content_length_state<std::uint64_t> explicit_length;
    std::size_t name_bytes = 0;
    std::size_t projected_count = 1;
    bool has_date = false;
    for (const auto& header : headers) {
        auto name = header.name();
        const auto value = header.value();
        if (!valid_response_header(name, value)) {
            return http3_response_head_failure{http3_response_head_error::invalid_field};
        }
        if (detail::is_forbidden_http_binary_response_field(name)) {
            return http3_response_head_failure{http3_response_head_error::forbidden_field};
        }
        if (http_ascii_equals_ignore_case(name, "date")) {
            has_date = true;
        }
        if (http_ascii_equals_ignore_case(name, "content-length")) {
            ++content_length_count;
            if (content_length_count > 1 ||
                explicit_length.parse_single_value(value) != detail::http_content_length_parse_status::ok ||
                !write_plan.explicit_content_length_allowed() ||
                (status != http_status::not_modified && *explicit_length.value() != write_plan.content_length())) {
                return http3_response_head_failure{http3_response_head_error::invalid_field};
            }
        }
        if (projected_count == std::numeric_limits<std::size_t>::max()) {
            return http3_response_head_failure{http3_response_head_error::field_section_error,
                http3_field_section_error::too_many_fields};
        }
        ++projected_count;
        if (!add_lowercase_name_bytes(name_bytes, name)) {
            return http3_response_head_failure{http3_response_head_error::field_section_error,
                http3_field_section_error::field_list_too_large};
        }
    }

    const auto generated_date = has_date ? std::string_view{} : detail::cached_date_value();
    if (!generated_date.empty()) {
        if (projected_count == std::numeric_limits<std::size_t>::max()) {
            return http3_response_head_failure{
                http3_response_head_error::field_section_error,
                http3_field_section_error::too_many_fields};
        }
        ++projected_count;
    }
    const bool synthesize_length = content_length_count == 0 && write_plan.auto_content_length_allowed();
    if (synthesize_length) {
        if (projected_count == std::numeric_limits<std::size_t>::max()) {
            return http3_response_head_failure{http3_response_head_error::field_section_error,
                http3_field_section_error::too_many_fields};
        }
        ++projected_count;
    }
    if (projected_count > limits.max_fields_) {
        return http3_response_head_failure{http3_response_head_error::field_section_error,
            http3_field_section_error::too_many_fields};
    }
    // QPACK static indices can encode large decoded fields in a single byte;
    // an upper-bound estimate would incorrectly reject a fitting section.
    // The encoder checks the exact encoded size below.
    if (limits.max_encoded_bytes_ < 2) {
        return http3_response_head_failure{http3_response_head_error::field_section_error,
            http3_field_section_error::field_section_too_large};
    }

    auto* const memory = resource != nullptr ? resource : std::pmr::get_default_resource();
    std::array<char, 20> length_bytes{};
    const bool emit_length = content_length_count != 0 || synthesize_length;
    const auto canonical_length = content_length_count != 0 && status == http_status::not_modified
                                      ? *explicit_length.value()
                                      : write_plan.content_length();
    std::size_t canonical_length_size = 0;
    if (emit_length) {
        const auto [end, error] = std::to_chars(length_bytes.data(), length_bytes.data() + length_bytes.size(),
            canonical_length);
        if (error != std::errc{}) {
            return http3_response_head_failure{http3_response_head_error::invalid_field};
        }
        canonical_length_size = static_cast<std::size_t>(end - length_bytes.data());
    }
    lowercase_field_names lowercase(memory, name_bytes);
    response_fields projected(status, write_plan.request_method(), projected_count, limits, memory);
    if (!generated_date.empty()) {
        if (const auto failure = projected.append({"date", generated_date, false})) {
            return *failure;
        }
    }
    for (const auto& header : headers) {
        const auto original_name = header.name();
        auto value = header.value();
        const bool content_length_field = http_ascii_equals_ignore_case(original_name, "content-length");
        const auto name = lowercase.project(original_name);
        if (content_length_field) {
            value = std::string_view(length_bytes.data(), canonical_length_size);
        }
        if (const auto failure = projected.append({name, value, false})) {
            return *failure;
        }
    }
    if (synthesize_length) {
        if (const auto failure = projected.append({"content-length", std::string_view(length_bytes.data(), canonical_length_size), false})) {
            return *failure;
        }
    }
    auto encoded = projected.encode(encoder, stream_id);
    if ((encoded.index() != 0)) {
        return encoded;
    }
    std::get<0>(encoded).body_plan_ = write_plan.body_plan();
    return encoded;
}

static std::variant<http3_response_head, http3_response_head_failure> encode_interim_response_head(
    const http_interim_response_head& response, http3_field_section_limits limits, std::pmr::memory_resource* resource, http3_qpack_encoder* encoder, std::uint64_t stream_id) {
    auto* const memory = resource != nullptr ? resource : std::pmr::get_default_resource();
    if (detail::validate_http_interim_response_headers(response) != detail::http_interim_response_header_validation_status::ok) {
        return http3_response_head_failure{http3_response_head_error::invalid_field};
    }
    if (const auto failure = response_head_preflight(response.status(), response.headers().size(), limits)) {
        return *failure;
    }
    std::size_t lowercase_bytes = 0;
    for (const auto& field : response.headers()) {
        if (!add_lowercase_name_bytes(lowercase_bytes, field.name())) {
            return http3_response_head_failure{http3_response_head_error::field_section_error,
                http3_field_section_error::field_list_too_large};
        }
    }
    lowercase_field_names lowercase(memory, lowercase_bytes);
    response_fields fields_value(response.status(), http_known_method::get, response.headers().size() + 1, limits, memory);
    for (const auto& field : response.headers()) {
        if (const auto failure = fields_value.append({lowercase.project(field.name()), field.value(), false})) {
            return *failure;
        }
    }
    return fields_value.encode(encoder, stream_id);
}

static std::variant<http3_streaming_response_head, http3_response_head_failure>
encode_streaming_response_head(http_response response, http_known_method method,
    http_response_stream_kind kind, http_response_trailer_intent trailers, http3_field_section_limits limits,
    std::pmr::memory_resource* resource, http3_qpack_encoder* encoder, std::uint64_t stream_id) {
    const auto plan = plan_http_response_stream_commit(http_response_stream_framing::http3_frames, method, response.status(), trailers);
    if (response.status().is_informational()) {
        return http3_response_head_failure{http3_response_head_error::unsupported_status};
    }
    if (!plan.trailer_intent_allowed()) {
        return http3_response_head_failure{http3_response_head_error::invalid_field};
    }
    auto prepared = prepare_http_response_stream_head(std::move(response), kind, plan);
    if (const auto failure = response_head_preflight(prepared.response().status(), prepared.response().headers().size(), limits)) {
        return *failure;
    }
    auto* memory = resource != nullptr ? resource : std::pmr::get_default_resource();
    std::size_t lowercase_bytes = 0;
    bool has_date = false;
    for (const auto& header : prepared.response().headers()) {
        has_date = has_date || http_ascii_equals_ignore_case(header.name(), "date");
        if (!add_lowercase_name_bytes(lowercase_bytes, header.name())) {
            return http3_response_head_failure{http3_response_head_error::field_section_error,
                http3_field_section_error::field_list_too_large};
        }
    }
    const auto header_count = prepared.response().headers().size() + (has_date ? 0U : 1U);
    if (const auto failure = response_head_preflight(prepared.response().status(), header_count, limits)) {
        return *failure;
    }
    lowercase_field_names lowercase(memory, lowercase_bytes);
    response_fields fields_value(prepared.response().status(), method, header_count + 1, limits, memory);
    for (const auto& header : prepared.response().headers()) {
        const auto name = lowercase.project(header.name());
        if (const auto failure = fields_value.append({name, header.value(), false})) {
            return *failure;
        }
    }
    if (!has_date) {
        if (const auto failure = fields_value.append({"date", detail::cached_date_value(), false})) {
            return *failure;
        }
    }
    auto encoded = fields_value.encode(encoder, stream_id);
    if ((encoded.index() != 0)) {
        return std::get<1>(encoded);
    }
    return http3_streaming_response_head{std::move(std::get<0>(encoded)), plan};
}

std::variant<http3_streaming_response_head, http3_response_head_failure> encode_http3_streaming_response_head(http_response response, http_known_method method,
    http_response_stream_kind kind, http_response_trailer_intent trailers, http3_field_section_limits limits, std::pmr::memory_resource* resource) {
    return encode_streaming_response_head(std::move(response), method, kind, trailers, limits, resource, nullptr, 0);
}
std::variant<http3_streaming_response_head, http3_response_head_failure> encode_http3_streaming_response_head(http3_qpack_encoder& encoder, std::uint64_t stream_id, http_response response, http_known_method method,
    http_response_stream_kind kind, http_response_trailer_intent trailers, http3_field_section_limits limits, std::pmr::memory_resource* resource) {
    return encode_streaming_response_head(std::move(response), method, kind, trailers, limits, resource, &encoder, stream_id);
}
std::variant<http3_response_head, http3_response_head_failure> encode_http3_interim_response_head(const http_interim_response_head& response,
    http3_field_section_limits limits, std::pmr::memory_resource* resource) {
    return encode_interim_response_head(response, limits, resource, nullptr, 0);
}
std::variant<http3_response_head, http3_response_head_failure> encode_http3_interim_response_head(http3_qpack_encoder& encoder, std::uint64_t stream_id, const http_interim_response_head& response,
    http3_field_section_limits limits, std::pmr::memory_resource* resource) {
    return encode_interim_response_head(response, limits, resource, &encoder, stream_id);
}

std::variant<http3_response_head, http3_response_head_failure> encode_http3_response_head(
    http_status_code status, http_known_method method, std::span<const http3_field_section_field_view> fields_value,
    http3_field_section_limits limits, std::pmr::memory_resource* resource) {
    return encode_response_head(status, method, fields_value, limits, resource, nullptr, 0);
}
std::variant<http3_response_head, http3_response_head_failure> encode_http3_response_head(
    const http_response& response, http_buffered_response_write_plan plan,
    http3_field_section_limits limits, std::pmr::memory_resource* resource) {
    return encode_response_head(response, plan, limits, resource, nullptr, 0);
}
std::variant<http3_response_field_section, http3_response_head_failure> encode_http3_response_trailers(
    std::span<const http3_field_section_field_view> fields_value,
    http3_field_section_limits limits, std::pmr::memory_resource* resource) {
    return encode_response_trailers(fields_value, limits, resource, nullptr, 0);
}

std::variant<http3_response_head, http3_response_head_failure> encode_http3_response_head(
    http3_qpack_encoder& encoder, std::uint64_t stream_id, http_status_code status, http_known_method method, std::span<const http3_field_section_field_view> fields_value,
    http3_field_section_limits limits, std::pmr::memory_resource* resource) {
    return encode_response_head(status, method, fields_value, limits, resource, &encoder, stream_id);
}
std::variant<http3_response_head, http3_response_head_failure> encode_http3_response_head(
    http3_qpack_encoder& encoder, std::uint64_t stream_id, const http_response& response, http_buffered_response_write_plan plan,
    http3_field_section_limits limits, std::pmr::memory_resource* resource) {
    return encode_response_head(response, plan, limits, resource, &encoder, stream_id);
}
std::variant<http3_response_field_section, http3_response_head_failure> encode_http3_response_trailers(
    http3_qpack_encoder& encoder, std::uint64_t stream_id, std::span<const http3_field_section_field_view> fields_value,
    http3_field_section_limits limits, std::pmr::memory_resource* resource) {
    return encode_response_trailers(fields_value, limits, resource, &encoder, stream_id);
}

}  // namespace ruvia
