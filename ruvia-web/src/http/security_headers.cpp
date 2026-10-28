#include "ruvia/web/security_headers.h"

#include <stdexcept>
#include <type_traits>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/util/registration_resource.h"

#include "context/context_access.h"

namespace ruvia {
namespace {

[[nodiscard]] bool has_security_header(context& context_value, std::string_view name) {
    return detail::context_access::has_response_header(context_value, name);
}

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

struct security_headers_flags final {
    bool emit_content_type_options_;
    bool emit_frame_options_;
    bool emit_strict_transport_security_;
    bool emit_disabled_xss_protection_;
    bool replace_existing_;
};

struct security_headers_policy final {
    security_headers_flags flags_;
    std::string_view content_security_policy_;
    std::string_view referrer_policy_;
    std::string_view permissions_policy_;
};

[[nodiscard]] security_headers_flags validate_security_headers_config(
    const security_headers_config& config) {
    const bool emit_content_type_options = emits_default_security_header(config.content_type_options_header_);
    const bool emit_frame_options = emits_default_security_header(config.frame_options_header_);
    const bool emit_strict_transport_security =
        emits_default_security_header(config.strict_transport_security_header_);
    const bool replace_existing = replaces_existing_security_header(config.existing_headers_);
    bool emit_disabled_xss_protection = false;
    switch (config.xss_protection_header_) {
        case xss_protection_header_policy::emit_disabled:
            emit_disabled_xss_protection = true;
            break;
        case xss_protection_header_policy::omit:
            break;
        default:
            throw std::invalid_argument("X-XSS-Protection header policy is invalid");
    }

    const auto validate_value = [](std::string_view value) {
        if (!value.empty() && !is_valid_http_header_value(value)) {
            throw std::invalid_argument("security header value is invalid");
        }
    };
    validate_value(config.content_security_policy_);
    validate_value(config.referrer_policy_);
    validate_value(config.permissions_policy_);
    for (const auto& header : config.custom_headers_) {
        if (!is_valid_http_header_name(header.name_)) {
            throw std::invalid_argument("custom security header name is invalid");
        }
        if (!is_valid_http_header_value(header.value_)) {
            throw std::invalid_argument("custom security header value is invalid");
        }
    }
    return security_headers_flags{
        .emit_content_type_options_ = emit_content_type_options,
        .emit_frame_options_ = emit_frame_options,
        .emit_strict_transport_security_ = emit_strict_transport_security,
        .emit_disabled_xss_protection_ = emit_disabled_xss_protection,
        .replace_existing_ = replace_existing,
    };
}

[[nodiscard]] http_header_view custom_header_view(const http_header& header) noexcept {
    return {header.name(), header.value()};
}

template <typename target_type, typename header_range_type>
void apply_security_headers_to(target_type& target, const security_headers_policy& policy,
    const header_range_type& custom_headers, bool secure_transport) {
    const auto set_header = [&target, replace_existing = policy.flags_.replace_existing_](
                                std::string_view name, std::string_view value, bool skip_empty) {
        if (skip_empty && value.empty()) {
            return;
        }
        if constexpr (std::is_same_v<target_type, http_response>) {
            if (const auto existing = target.header(name)) {
                if (!replace_existing || *existing == value) {
                    return;
                }
            }
        } else if (!replace_existing && has_security_header(target, name)) {
            return;
        }
        target.header(name, value);
    };
    const auto set_secure_transport_header = [&set_header, secure_transport](
                                                 std::string_view name, std::string_view value) {
        if (!secure_transport &&
            http_ascii_equals_ignore_case(name, "Strict-Transport-Security")) {
            return;
        }
        set_header(name, value, false);
    };

    if (policy.flags_.emit_content_type_options_) {
        set_header("X-Content-Type-Options", "nosniff", true);
    }
    if (policy.flags_.emit_frame_options_) {
        set_header("X-Frame-Options", "DENY", true);
    }
    // RFC 6797 section 7.2: an HSTS host MUST NOT send STS over a
    // non-secure transport. This decision requires context connection metadata.
    if (policy.flags_.emit_strict_transport_security_ && secure_transport) {
        set_header("Strict-Transport-Security", "max-age=31536000; includeSubDomains", true);
    }
    if (policy.flags_.emit_disabled_xss_protection_) {
        set_header("X-XSS-Protection", "0", true);
    }

    set_header("Content-Security-Policy", policy.content_security_policy_, true);
    set_header("Referrer-Policy", policy.referrer_policy_, true);
    set_header("Permissions-Policy", policy.permissions_policy_, true);

    for (const auto& header : custom_headers) {
        const auto field = custom_header_view(header);
        set_secure_transport_header(field.name(), field.value());
    }
}

}  // namespace

security_headers_middleware::security_headers_middleware(
    const security_headers_config& source_value, std::pmr::memory_resource* resource)
    : content_security_policy_(resource),
      referrer_policy_(resource),
      permissions_policy_(resource),
      custom_headers_(resource) {
    const auto flags = validate_security_headers_config(source_value);
    emit_content_type_options_ = flags.emit_content_type_options_;
    emit_frame_options_ = flags.emit_frame_options_;
    emit_strict_transport_security_ = flags.emit_strict_transport_security_;
    emit_disabled_xss_protection_ = flags.emit_disabled_xss_protection_;
    replace_existing_ = flags.replace_existing_;
    content_security_policy_.assign(source_value.content_security_policy_);
    referrer_policy_.assign(source_value.referrer_policy_);
    permissions_policy_.assign(source_value.permissions_policy_);
    custom_headers_.reserve(source_value.custom_headers_.size());
    for (const auto& header : source_value.custom_headers_) {
        custom_headers_.emplace_back(header.name_, header.value_, resource);
    }
}

security_headers_middleware::security_headers_middleware()
    : security_headers_middleware(security_headers_config{}) {}

security_headers_middleware::security_headers_middleware(const security_headers_config& config)
    : security_headers_middleware(config, detail::registration_resource()) {}

task<void> security_headers_middleware::handle(context& context_value, next& next_value) {
    const auto connection = context_value.conn();
    const bool secure_transport = connection.scheme() == http_scheme::https;
    const security_headers_policy policy{
        .flags_ =
            security_headers_flags{
                .emit_content_type_options_ = emit_content_type_options_,
                .emit_frame_options_ = emit_frame_options_,
                .emit_strict_transport_security_ = emit_strict_transport_security_,
                .emit_disabled_xss_protection_ = emit_disabled_xss_protection_,
                .replace_existing_ = replace_existing_,
            },
        .content_security_policy_ = content_security_policy_,
        .referrer_policy_ = referrer_policy_,
        .permissions_policy_ = permissions_policy_,
    };
    apply_security_headers_to(context_value, policy, custom_headers_, secure_transport);
    co_await next_value();
    apply_security_headers_to(detail::context_access::response_storage(context_value), policy,
        custom_headers_, secure_transport);
}

}  // namespace ruvia
