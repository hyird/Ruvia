#include "http/http_cors.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_values.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_request_target.h"

namespace ruvia::detail {
namespace {

constexpr std::string_view allow_origin_header_name = "Access-Control-Allow-Origin";
constexpr std::string_view allow_headers_header_name = "Access-Control-Allow-Headers";
constexpr std::string_view expose_headers_header_name = "Access-Control-Expose-Headers";
constexpr std::string_view max_age_header_name = "Access-Control-Max-Age";

bool has_response_header(const http_response& response, std::string_view name) {
    return response.header(name).has_value();
}

void set_response_header_if_missing(
    http_response& response, std::string_view name, std::string_view value) {
    if (!has_response_header(response, name)) {
        response.header_stable_view(name, value);
    }
}

void set_prefixed_response_header_if_missing(
    http_response& response, std::string_view stored, std::size_t name_size) {
    if (stored.size() <= name_size) {
        return;
    }
    set_response_header_if_missing(
        response, stored.substr(0, name_size), stored.substr(name_size));
}

void add_vary_tokens(http_response& response, const std::string_view* tokens, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        response.add_vary_token(tokens[i]);
    }
}

// Validate Origin, Access-Control-Request-Method and the complete
// Access-Control-Request-Headers list in one pass over the request fields.
struct cors_request_fields final {
    std::optional<std::string_view> origin_{};
    std::optional<std::string_view> requested_method_{};
    std::size_t request_header_names_size_{0};
};

[[nodiscard]] std::optional<cors_request_fields> scan_cors_request_fields(const http_request& request) {
    cors_request_fields result;
    bool first_header_name = true;
    for (const auto& field : request.headers()) {
        const auto name = field.name();
        switch (name.size()) {
            case 6:
                if (http_ascii_equals_ignore_case(name, "Origin")) {
                    result.origin_ = field.value();
                }
                break;
            case 29:
                if (http_ascii_equals_ignore_case(name, "Access-Control-Request-Method")) {
                    result.requested_method_ = field.value();
                }
                break;
            case 30:
                if (http_ascii_equals_ignore_case(name, "Access-Control-Request-Headers")) {
                    http_field_name_list names(field.value());
                    bool saw_name = false;
                    while (const auto header_name = names.next()) {
                        saw_name = true;
                        const auto separator_size = first_header_name ? std::size_t{0} : std::size_t{2};
                        const auto maximum = (std::numeric_limits<std::size_t>::max)();
                        if (header_name->size() > maximum - separator_size ||
                            result.request_header_names_size_ >
                                maximum - separator_size - header_name->size()) {
                            return std::nullopt;
                        }
                        result.request_header_names_size_ += separator_size + header_name->size();
                        first_header_name = false;
                    }
                    if (!saw_name || !names.valid()) {
                        return std::nullopt;
                    }
                }
                break;
            default:
                break;
        }
    }
    return result;
}

void reflect_cors_request_header_names(
    const http_request& request, http_response& response, std::size_t size) {
    if (size == 0 || has_response_header(response, "Access-Control-Allow-Headers")) {
        return;
    }
    static constexpr std::size_t inline_capacity = 256;
    if (size <= inline_capacity) {
        std::array<char, inline_capacity> buffer{};
        std::size_t length = 0;
        for (const auto& field : request.headers()) {
            if (!http_ascii_equals_ignore_case(field.name(), "Access-Control-Request-Headers")) {
                continue;
            }
            http_field_name_list names(field.value());
            while (const auto name = names.next()) {
                if (length != 0) {
                    buffer[length++] = ',';
                    buffer[length++] = ' ';
                }
                std::memcpy(buffer.data() + length, name->data(), name->size());
                length += name->size();
            }
        }
        response.header("Access-Control-Allow-Headers", std::string_view(buffer.data(), length));
        return;
    }
    std::pmr::string value(response.memory_resource());
    value.reserve(size);
    for (const auto& field : request.headers()) {
        if (!http_ascii_equals_ignore_case(field.name(), "Access-Control-Request-Headers")) {
            continue;
        }
        http_field_name_list names(field.value());
        while (const auto name = names.next()) {
            if (!value.empty()) {
                value.append(", ");
            }
            value.append(*name);
        }
    }
    response.header("Access-Control-Allow-Headers", value);
}

void validate_cors_header_names(const std::vector<std::string>& names, const char* empty_message) {
    if (empty_message != nullptr && names.empty()) {
        throw std::invalid_argument(empty_message);
    }
    for (const auto& name : names) {
        if (!is_valid_http_header_name(name)) {
            throw std::invalid_argument("CORS header names must be valid HTTP field names");
        }
    }
}

void append_cors_header_names(std::pmr::string& output, std::string_view header_name,
    const std::vector<std::string>& names) {
    if (names.empty()) {
        return;
    }
    std::size_t total_size = header_name.size();
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            total_size += 2U;
        }
        total_size += names[i].size();
    }
    output.reserve(total_size);
    output.append(header_name);
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            output.append(", ");
        }
        output.append(names[i]);
    }
}

void validate_cors_config(const cors_config& config) {
    const auto origin_value = config.origin_.value_.view();
    switch (config.origin_.mode_) {
        case cors_origin_mode::any:
            if (!origin_value.empty()) {
                throw std::invalid_argument("CORS wildcard origin must not include a value");
            }
            break;
        case cors_origin_mode::exact:
        case cors_origin_mode::credentialed_exact:
            if (origin_value != "null" &&
                !::ruvia::is_valid_http_serialized_origin(origin_value)) {
                throw std::invalid_argument("CORS origin must be a WHATWG serialized origin");
            }
            break;
        default:
            throw std::invalid_argument("CORS origin mode is invalid");
    }

    switch (config.request_headers_.mode_) {
        case cors_request_headers_mode::reflect:
            if (!config.request_headers_.names_.empty()) {
                throw std::invalid_argument(
                    "CORS reflected request headers must not include fixed names");
            }
            break;
        case cors_request_headers_mode::fixed:
            validate_cors_header_names(
                config.request_headers_.names_, "CORS fixed request headers must not be empty");
            break;
        default:
            throw std::invalid_argument("CORS request headers mode is invalid");
    }
    validate_cors_header_names(config.expose_headers_, nullptr);
    if (config.max_age_.has_value() && config.max_age_->count() < 0) {
        throw std::invalid_argument("CORS max age must not be negative");
    }
}

}  // namespace

cors_options make_cors_options(const cors_config& config, std::pmr::memory_resource* resource) {
    validate_cors_config(config);

    cors_options stored(resource);
    stored.origin_mode_ = config.origin_.mode_;
    if (config.origin_.mode_ != cors_origin_mode::any) {
        const auto origin_value = config.origin_.value_.view();
        stored.origin_.reserve(allow_origin_header_name.size() + origin_value.size());
        stored.origin_.append(allow_origin_header_name);
        stored.origin_.append(origin_value);
    }

    stored.request_headers_mode_ = config.request_headers_.mode_;
    if (config.request_headers_.mode_ == cors_request_headers_mode::fixed) {
        append_cors_header_names(stored.request_headers_, allow_headers_header_name,
            config.request_headers_.names_);
    }
    append_cors_header_names(
        stored.expose_headers_, expose_headers_header_name, config.expose_headers_);
    if (config.max_age_.has_value()) {
        // validate_cors_config rejects negative max ages.
        std::array<char, http_max_unsigned_decimal_size> buffer;
        const auto formatted =
            http_format_unsigned(buffer, static_cast<std::uint64_t>(config.max_age_->count()));
        stored.max_age_.reserve(max_age_header_name.size() + formatted.size());
        stored.max_age_.append(max_age_header_name);
        stored.max_age_.append(formatted);
    }
    return stored;
}

void apply_cors_headers(const http_request& request, http_response& response, const cors_options& cors) {
    const auto request_fields = scan_cors_request_fields(request);
    if (!request_fields) {
        return;
    }
    const auto& origin_field = request_fields->origin_;
    const auto origin = origin_field.value_or(std::string_view{});
    // CORS is an application policy, not message framing. Opaque or malformed
    // metadata may reach the handler but must not generate cross-origin grants.
    // Never select one origin from a multi-origin value for a CORS decision.
    if (origin_field && origin != "null" && !::ruvia::is_valid_http_serialized_origin(origin)) {
        return;
    }
    const auto& requested_method = request_fields->requested_method_;
    if (requested_method && !is_valid_http_method_token(*requested_method)) {
        return;
    }
    const auto request_header_names_size = request_fields->request_header_names_size_;
    const bool wildcard_origin = cors.origin_mode_ == cors_origin_mode::any;
    std::array<std::string_view, 3> vary_tokens{};
    std::size_t vary_token_count = 0;
    const bool options = request.known_method() == http_known_method::options;
    if (options) {
        vary_tokens[vary_token_count++] = "Origin";
        vary_tokens[vary_token_count++] = "Access-Control-Request-Method";
        if (cors.request_headers_mode_ == cors_request_headers_mode::reflect) {
            vary_tokens[vary_token_count++] = "Access-Control-Request-Headers";
        }
    }
    if (wildcard_origin) {
        set_response_header_if_missing(response, allow_origin_header_name, "*");
    } else {
        set_prefixed_response_header_if_missing(
            response, cors.origin_, allow_origin_header_name.size());
    }
    if (cors.origin_mode_ == cors_origin_mode::credentialed_exact) {
        set_response_header_if_missing(response, "Access-Control-Allow-Credentials", "true");
    }

    const bool preflight = options && !origin.empty() && requested_method.has_value() &&
                           !requested_method->empty();
    if (preflight) {
        if (const auto allow = response.header("Allow"); allow.has_value() && !allow->empty()) {
            set_response_header_if_missing(response, "Access-Control-Allow-Methods", *allow);
        }
        if (cors.request_headers_mode_ == cors_request_headers_mode::fixed) {
            set_prefixed_response_header_if_missing(
                response, cors.request_headers_, allow_headers_header_name.size());
        } else {
            reflect_cors_request_header_names(request, response, request_header_names_size);
        }
        add_vary_tokens(response, vary_tokens.data(), vary_token_count);
        if (!cors.max_age_.empty()) {
            set_prefixed_response_header_if_missing(
                response, cors.max_age_, max_age_header_name.size());
        }
        return;
    }

    add_vary_tokens(response, vary_tokens.data(), vary_token_count);
    if (!cors.expose_headers_.empty()) {
        set_prefixed_response_header_if_missing(
            response, cors.expose_headers_, expose_headers_header_name.size());
    }
}

}  // namespace ruvia::detail
