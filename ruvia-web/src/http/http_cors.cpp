#include "http/http_cors.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_name_list.h"
#include "ruvia/http/http_origin.h"

namespace ruvia::detail {
namespace {

bool has_response_header(const http_response& response, std::string_view name) {
    return response.header(name).has_value();
}

void set_response_header_if_missing(
    http_response& response, std::string_view name, std::string_view value) {
    if (!has_response_header(response, name)) {
        response.header(name, value);
    }
}

void add_vary_tokens(http_response& response, const std::string_view* tokens, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        response.add_vary_token(tokens[i]);
    }
}

void set_cors_max_age(http_response& response, const std::optional<std::chrono::seconds>& max_age) {
    if (!max_age.has_value() || has_response_header(response, "Access-Control-Max-Age")) {
        return;
    }
    std::array<char, 32> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), max_age->count());
    if (error != std::errc{}) {
        throw std::logic_error("CORS max age integer formatting failed");
    }
    response.header("Access-Control-Max-Age",
        std::string_view(buffer.data(), static_cast<std::size_t>(end - buffer.data())));
}

void reflect_cors_request_header_names(const http_request& request, http_response& response) {
    if (has_response_header(response, "Access-Control-Allow-Headers")) {
        return;
    }

    bool first = true;
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto& header_value = headers[i];
        if (!http_ascii_equals_ignore_case(
                header_value.name(), "Access-Control-Request-Headers")) {
            continue;
        }
        http_field_name_list names(header_value.value());
        bool saw_name = false;
        while (const auto name = names.next()) {
            saw_name = true;
            if (first) {
                set_response_header_if_missing(response, "Access-Control-Allow-Headers", *name);
                first = false;
            } else {
                response.header("Access-Control-Allow-Headers", *name,
                    http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
            }
        }
        if (!saw_name || !names.valid()) {
            throw std::logic_error("validated CORS request header list became invalid");
        }
    }
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

void append_cors_header_names(std::pmr::string& output, const std::vector<std::string>& names) {
    for (const auto& name : names) {
        if (!output.empty()) {
            output.append(", ");
        }
        output.append(name);
    }
}

void validate_cors_config(const cors_config& config) {
    switch (config.origin_.mode_) {
        case cors_origin_mode::any:
            if (!config.origin_.value_.empty()) {
                throw std::invalid_argument("CORS wildcard origin must not include a value");
            }
            break;
        case cors_origin_mode::exact:
        case cors_origin_mode::credentialed_exact:
            if (config.origin_.value_ != "null" &&
                !::ruvia::is_valid_http_serialized_origin(config.origin_.value_)) {
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
        stored.origin_ = config.origin_.value_;
    }

    stored.request_headers_mode_ = config.request_headers_.mode_;
    if (config.request_headers_.mode_ == cors_request_headers_mode::fixed) {
        append_cors_header_names(stored.request_headers_, config.request_headers_.names_);
    }
    append_cors_header_names(stored.expose_headers_, config.expose_headers_);
    stored.max_age_ = config.max_age_;
    return stored;
}

void apply_cors_headers(const http_request& request, http_response& response, const cors_options& cors) {
    const auto origin_field = request.header("Origin");
    const auto origin = origin_field.value_or(std::string_view{});
    const bool wildcard_origin = cors.origin_mode_ == cors_origin_mode::any;
    const auto allow_origin = wildcard_origin ? std::string_view("*") : std::string_view(cors.origin_);
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
    set_response_header_if_missing(response, "Access-Control-Allow-Origin", allow_origin);
    if (cors.origin_mode_ == cors_origin_mode::credentialed_exact) {
        set_response_header_if_missing(response, "Access-Control-Allow-Credentials", "true");
    }

    const auto requested_method = request.header("Access-Control-Request-Method");
    const bool preflight = options && !origin.empty() && requested_method.has_value() &&
                           !requested_method->empty();
    if (preflight) {
        if (const auto allow = response.header("Allow"); allow.has_value() && !allow->empty()) {
            set_response_header_if_missing(response, "Access-Control-Allow-Methods", *allow);
        }
        if (cors.request_headers_mode_ == cors_request_headers_mode::fixed) {
            set_response_header_if_missing(response, "Access-Control-Allow-Headers", cors.request_headers_);
        } else {
            reflect_cors_request_header_names(request, response);
        }
        add_vary_tokens(response, vary_tokens.data(), vary_token_count);
        set_cors_max_age(response, cors.max_age_);
        return;
    }

    add_vary_tokens(response, vary_tokens.data(), vary_token_count);
    if (!cors.expose_headers_.empty()) {
        set_response_header_if_missing(response, "Access-Control-Expose-Headers", cors.expose_headers_);
    }
}

}  // namespace ruvia::detail
