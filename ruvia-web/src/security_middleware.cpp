#include "security_middleware.h"

#include <array>
#include <stdexcept>
#include <string_view>

#include "context.h"
#include "context_access.h"
#include "crypto.h"
#include "http_cookie.h"
#include "http_error.h"
#include "http_header.h"
#include "http_request.h"
#include "middleware.h"
#include "task.h"

namespace ruvia {
namespace {

[[nodiscard]] bool is_csrf_token_header(std::string_view name) noexcept {
    for (const std::string_view prefix : {"sec-", "proxy-", "access-control-request-", "x-forwarded-"}) {
        if (name.size() >= prefix.size() && http_ascii_equals_ignore_case(name.substr(0, prefix.size()), prefix)) {
            return false;
        }
    }
    for (const std::string_view reserved : {"accept", "accept-language", "content-language", "content-type", "range", "accept-charset", "accept-encoding", "connection", "content-length", "cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive", "origin", "permissions-policy", "referer", "set-cookie", "te", "trailer", "transfer-encoding", "upgrade", "via", "user-agent", "priority", "save-data", "device-memory", "dpr", "width", "viewport-width", "downlink", "ect", "rtt", "forwarded", "x-real-ip", "x-requested-with"}) {
        if (http_ascii_equals_ignore_case(name, reserved)) {
            return false;
        }
    }
    return true;
}

}  // namespace

const csrf_protection_config& csrf_protection::validate(const csrf_protection_config& source_value) {
    if (!is_valid_http_header_name(source_value.cookie_name_)) {
        throw std::invalid_argument("CSRF cookie name must be a valid HTTP token");
    }
    if (!is_valid_http_header_name(source_value.header_name_)) {
        throw std::invalid_argument("CSRF header name must be a valid HTTP field name");
    }
    if (!is_csrf_token_header(source_value.header_name_)) {
        throw std::invalid_argument("CSRF token requires a script-supplied custom request field");
    }
    return source_value;
}

csrf_protection::csrf_protection() :
    csrf_protection(csrf_protection_config{}) {}

csrf_protection::csrf_protection(const csrf_protection_config& config) :
    cookie_name_(validate(config).cookie_name_),
    header_name_(config.header_name_) {}

task<void> csrf_protection::invoke(context& c, next& next_value) {
    const auto method = c.req().known_method();
    const bool safe = method == http_known_method::get || method == http_known_method::head || method == http_known_method::options;
    // Repeated cookie or header occurrences never resolve to one token.
    const auto cookie = c.req().unique_cookie(cookie_name_);
    const bool has_cookie = cookie.status_ == unique_lookup_status::found && !cookie.value_.empty();
    if (!safe) {
        const auto header_value = c.req().unique_header(header_name_);
        if (!has_cookie || header_value.status_ != unique_lookup_status::found || header_value.value_.empty() || !detail::constant_time_bytes_equal(cookie.value_, header_value.value_)) {
            throw http_error({.status_ = ruvia::http_status::forbidden, .code_ = "csrf_token_mismatch", .message_ = "CSRF token missing or invalid"});
        }
    } else if (!has_cookie) {
        std::array<char, detail::secure_token_size> buffer;
        const auto token = detail::generate_secure_token(buffer);
        const auto connection = c.conn();
        const cookie_options options{
            .path_ = "/",
            .same_site_ = cookie_same_site::lax,
            .secure_ = connection.scheme() == http_scheme::https ? cookie_attribute_policy::emit : cookie_attribute_policy::omit,
        };
        c.policy().set_cookie({.name_ = cookie_name_, .value_ = token, .attributes_ = options});
    }
    co_await next_value();
}

namespace {

constexpr std::string_view content_security_policy_name = "Content-Security-Policy";
constexpr std::string_view referrer_policy_name = "Referrer-Policy";
constexpr std::string_view permissions_policy_name = "Permissions-Policy";

[[nodiscard]] bool emits_default_security_header(default_security_header_policy policy) {
    switch (policy) {
        case default_security_header_policy::emit_default:
            return true;
        case default_security_header_policy::omit:
            return false;
        default:
            throw std::invalid_argument("default security header policy is invalid");
    }
}

[[nodiscard]] bool replaces_existing_security_header(security_header_conflict_policy policy) {
    switch (policy) {
        case security_header_conflict_policy::preserve_existing:
            return false;
        case security_header_conflict_policy::replace_existing:
            return true;
        default:
            throw std::invalid_argument("security header conflict policy is invalid");
    }
}

}  // namespace

security_headers_middleware::security_headers_middleware() :
    security_headers_middleware(security_headers_config{}) {}

security_headers_middleware::security_headers_middleware(const security_headers_config& source_value) :
    emit_content_type_options_(emits_default_security_header(source_value.content_type_options_header_)),
    emit_frame_options_(emits_default_security_header(source_value.frame_options_header_)),
    emit_strict_transport_security_(emits_default_security_header(source_value.strict_transport_security_header_)),
    emit_disabled_xss_protection_([&] {
        switch (source_value.xss_protection_header_) {
            case xss_protection_header_policy::emit_disabled:
                return true;
            case xss_protection_header_policy::omit:
                return false;
            default:
                throw std::invalid_argument("X-XSS-Protection header policy is invalid");
        }
    }()),
    replace_existing_(replaces_existing_security_header(source_value.existing_headers_)) {
    const auto validate_value = [](std::string_view value) {
        if (!value.empty() && !is_valid_http_header_value(value)) {
            throw std::invalid_argument("security header value is invalid");
        }
    };
    validate_value(source_value.content_security_policy_);
    validate_value(source_value.referrer_policy_);
    validate_value(source_value.permissions_policy_);
    for (const auto& header : source_value.custom_headers_) {
        if (!is_valid_http_header_name(header.name_)) {
            throw std::invalid_argument("custom security header name is invalid");
        }
        if (!is_valid_http_header_value(header.value_)) {
            throw std::invalid_argument("custom security header value is invalid");
        }
    }
    content_security_policy_.assign(source_value.content_security_policy_);
    referrer_policy_.assign(source_value.referrer_policy_);
    permissions_policy_.assign(source_value.permissions_policy_);
    custom_headers_.reserve(source_value.custom_headers_.size());
    for (const auto& header : source_value.custom_headers_) {
        custom_headers_.emplace_back(header.name_, header.value_);
    }
}

task<void> security_headers_middleware::invoke(context& context_value, next& next_value) {
    const bool secure_transport = context_value.conn().scheme() == http_scheme::https;
    const auto set_header = [&context_value, replace_existing = replace_existing_](std::string_view name, std::string_view value, bool skip_empty) {
        if (skip_empty && value.empty()) {
            return;
        }
        detail::context_access::security_header(context_value, name, value, replace_existing);
    };

    if (emit_content_type_options_) {
        set_header("X-Content-Type-Options", "nosniff", true);
    }
    if (emit_frame_options_) {
        set_header("X-Frame-Options", "DENY", true);
    }
    if (emit_strict_transport_security_ && secure_transport) {
        set_header("Strict-Transport-Security", "max-age=31536000; includeSubDomains", true);
    }
    if (emit_disabled_xss_protection_) {
        set_header("X-XSS-Protection", "0", true);
    }

    set_header(content_security_policy_name, content_security_policy_, true);
    set_header(referrer_policy_name, referrer_policy_, true);
    set_header(permissions_policy_name, permissions_policy_, true);

    for (const auto& header : custom_headers_) {
        if (!secure_transport && http_ascii_equals_ignore_case(header.name(), "Strict-Transport-Security")) {
            continue;
        }
        set_header(header.name(), header.value(), false);
    }
    co_await next_value();
}

}  // namespace ruvia
