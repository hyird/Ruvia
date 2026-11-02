#pragma once

#include <memory_resource>
#include <string>

#include "ruvia/core/task.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/web/detail/middleware/middleware_registration.h"
#include "ruvia/web/middleware.h"

namespace ruvia {

class context;
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

    task<void> handle(context& c, next& next);

private:
    [[nodiscard]] static const csrf_protection_config& validate(const csrf_protection_config& source);
    csrf_protection(const csrf_protection_config& validated, std::pmr::memory_resource* resource);

    std::pmr::string cookie_name_;
    std::pmr::string header_name_;
};

}  // namespace ruvia
