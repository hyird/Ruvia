#include "test_harness.h"

#include <memory_resource>
#include <string_view>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/Session.h"

namespace {

ruvia::HttpResponse makeResponse() {
    return ruvia::HttpResponse(std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(session_cookie_append_preserves_existing_set_cookie) {
    auto response = makeResponse();
    response.header("Set-Cookie", "theme=light", {.append = true});

    ruvia::detail::appendSessionCookieHeader(response,
                                            std::pmr::new_delete_resource(),
                                            "abcdef",
                                            false);

    const auto& headers = response.headers();
    RUVIA_CHECK_EQ(headers.size(), std::size_t{2});
    auto it = headers.begin();
    RUVIA_CHECK_EQ(it->name(), std::string_view("Set-Cookie"));
    RUVIA_CHECK_EQ(it->value(), std::string_view("theme=light"));
    ++it;
    RUVIA_CHECK_EQ(it->name(), std::string_view("Set-Cookie"));
    RUVIA_CHECK_EQ(it->value(), std::string_view("sid=abcdef; Path=/; HttpOnly; SameSite=Lax"));
}

RUVIA_TEST(session_cookie_secure_flag_appended_for_secure_requests) {
    auto response = makeResponse();

    ruvia::detail::appendSessionCookieHeader(response,
                                            std::pmr::new_delete_resource(),
                                            "abcdef",
                                            true);

    const auto& headers = response.headers();
    RUVIA_CHECK_EQ(headers.size(), std::size_t{1});
    const auto it = headers.begin();
    RUVIA_CHECK_EQ(it->name(), std::string_view("Set-Cookie"));
    RUVIA_CHECK_EQ(it->value(), std::string_view("sid=abcdef; Path=/; HttpOnly; SameSite=Lax; Secure"));
}
