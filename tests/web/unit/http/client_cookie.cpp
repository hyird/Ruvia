#include <array>
#include <chrono>
#include <initializer_list>
#include <memory_resource>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/web/HttpClientTypes.h"

#include "client/HttpClientConfigStorage.h"
#include "client/HttpClientRequestStorage.h"
#include "client/client_request_policy.h"
#include "test_harness.h"

RUVIA_TEST(client_cookie_reclaims_expired_count_and_byte_capacity_on_receive) {
    auto* const resource = std::pmr::get_default_resource();
    const std::array first_headers{
        ruvia::HttpHeader::copyOf("Set-Cookie", "old=one; Max-Age=1", resource)};
    const std::array next_headers{
        ruvia::HttpHeader::copyOf("Set-Cookie", "new=two", resource)};
    ruvia::detail::HttpClientRequestStorage request("GET", "/", resource);

    for (const bool count_limit : {false, true}) {
        ruvia::HttpClientConfig options;
        options.host = "example.test";
        options.receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend;
        options.maxCookies = count_limit ? 1 : 2;
        options.maxCookieBytes = count_limit ? 1024 : 7;
        const ruvia::detail::HttpClientConfigStorage config(options, resource);
        ruvia::detail::client_request_policy policy(config, resource);
        policy.retain_response_cookies(request, first_headers);

        std::pmr::vector<ruvia::HttpHeaderView> headers(resource);
        std::pmr::string cookie_header(resource);
        policy.append_headers(request, headers, cookie_header);
        RUVIA_CHECK_EQ(cookie_header, std::string_view("old=one"));
        headers.clear();
        cookie_header.clear();
        // The existing cookie expires while the next request is in flight,
        // so no outgoing-header call has reclaimed its storage yet.
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        policy.retain_response_cookies(request, next_headers);

        policy.append_headers(request, headers, cookie_header);
        RUVIA_CHECK_EQ(cookie_header, std::string_view("new=two"));
    }
}
