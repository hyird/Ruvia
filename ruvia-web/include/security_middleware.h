#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "http_header.h"
#include "middleware.h"
#include "task.h"

namespace ruvia {

struct csrf_protection_config final {
    std::string cookie_name_{"XSRF-TOKEN"};
    // 必须为脚本主动提交 token 的自定义字段；构造时拒绝 CORS-safelisted、
    // 浏览器保留及已知自动生成/代理字段（名称不区分大小写）。
    // 部署方还须保证代理不注入所选字段，且仅向可信 origin 授予携带凭据的
    // CORS 权限。不能用 Accept、Origin、Host 等环境信息代替主动提交的 token。
    std::string header_name_{"X-XSRF-TOKEN"};
};

// Stateless CSRF protection using the double-submit-cookie pattern (no
// server-side session store needed, so it works across SO_REUSEPORT workers).
// A safe request (GET/HEAD/OPTIONS) without a token cookie is issued a fresh
// one (readable by JavaScript so a SPA can echo it). An unsafe request must
// repeat that cookie's value in the header; a missing or mismatched token is
// rejected with 403. Register on a controller, group, or route that should
// enforce browser XSRF checks. Cookie and header names default to "XSRF-TOKEN"
// and "X-XSRF-TOKEN" and can be rebranded per app.
// This relies on the browser's custom-request-header boundary. It does not
// authenticate a user or protect an origin already trusted by credentialed CORS.
class csrf_protection final {
public:
    csrf_protection();
    explicit csrf_protection(const csrf_protection_config& config);

    csrf_protection(const csrf_protection&) = delete;
    csrf_protection& operator=(const csrf_protection&) = delete;
    csrf_protection(csrf_protection&&) = delete;
    csrf_protection& operator=(csrf_protection&&) = delete;

    task<void> invoke(context& c, next& next);

private:
    [[nodiscard]] static const csrf_protection_config& validate(const csrf_protection_config& source);

    std::string cookie_name_;
    std::string header_name_;
};

struct security_header final {
    std::string name_{};
    std::string value_{};
};
// Legacy browser XSS filters can introduce vulnerabilities in otherwise safe
// pages. Modern policy either emits X-XSS-Protection: 0 to disable the obsolete
// auditor, or omits the obsolete header entirely for clients that ignore it.
enum class xss_protection_header_policy : std::uint8_t {
    emit_disabled,
    omit,
};

enum class default_security_header_policy : std::uint8_t {
    omit,
    emit_default,
};

enum class security_header_conflict_policy : std::uint8_t {
    preserve_existing,
    replace_existing,
};

struct security_headers_config final {
    default_security_header_policy content_type_options_header_ = default_security_header_policy::emit_default;
    default_security_header_policy frame_options_header_ = default_security_header_policy::emit_default;
    // Emitted only for requests received over TLS. Plain HTTP responses must
    // never carry Strict-Transport-Security.
    default_security_header_policy strict_transport_security_header_ = default_security_header_policy::emit_default;
    xss_protection_header_policy xss_protection_header_ = xss_protection_header_policy::emit_disabled;

    std::string content_security_policy_{"default-src 'self'"};
    std::string referrer_policy_{"strict-origin-when-cross-origin"};
    std::string permissions_policy_{"geolocation=(), microphone=(), camera=()"};

    std::vector<security_header> custom_headers_{};
    security_header_conflict_policy existing_headers_ = security_header_conflict_policy::preserve_existing;
};

// Registered app-wide with the defaults as `app().use<security_headers_middleware>()`,
// or with an owning policy as `app().use<security_headers_middleware>(config)`.
class security_headers_middleware final {
public:
    // A 404 is a response to an attacker-reachable URL like any other, so it
    // needs the same CSP, frame and referrer policy a matched route gets. CORS
    // follows the same response-layer rule for unmatched requests.
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    security_headers_middleware();
    explicit security_headers_middleware(const security_headers_config& config);

    security_headers_middleware(const security_headers_middleware&) = delete;
    security_headers_middleware& operator=(const security_headers_middleware&) = delete;
    security_headers_middleware(security_headers_middleware&&) = delete;
    security_headers_middleware& operator=(security_headers_middleware&&) = delete;

    task<void> invoke(context& context, next& next);

private:
    bool emit_content_type_options_{true};
    bool emit_frame_options_{true};
    bool emit_strict_transport_security_{true};
    bool emit_disabled_xss_protection_{true};
    bool replace_existing_{false};
    std::string content_security_policy_;
    std::string referrer_policy_;
    std::string permissions_policy_;
    std::vector<http_header> custom_headers_;
};

}  // namespace ruvia
