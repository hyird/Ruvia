#include <array>
#include <chrono>
#include <initializer_list>
#include <memory_resource>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ruvia/http/http_header.h"
#include "ruvia/web/http_client_types.h"

#include "client/client_request_policy.h"
#include "client/http_client_config_storage.h"
#include "client/http_client_request_storage.h"
#include "failing_memory_resource.h"
#include "test_harness.h"

RUVIA_TEST(client_cookie_reclaims_expired_count_and_byte_capacity_on_receive) {
    auto* const resource = std::pmr::get_default_resource();
    const std::array first_headers{
        ruvia::http_header::copy_of("Set-Cookie", "old=one; Max-Age=1", resource)};
    const std::array next_headers{
        ruvia::http_header::copy_of("Set-Cookie", "new=two", resource)};
    ruvia::detail::http_client_request_storage request("GET", "/", resource);

    for (const bool count_limit : {false, true}) {
        ruvia::http_client_config options;
        options.host_ = "example.test";
        options.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
        options.max_cookies_ = count_limit ? 1 : 2;
        options.max_cookie_bytes_ = count_limit ? 1024 : 7;
        const ruvia::detail::http_client_config_storage config(options, resource);
        ruvia::detail::client_request_policy policy(config, resource);
        policy.retain_response_cookies(request, first_headers);

        std::pmr::vector<ruvia::http_header_view> headers(resource);
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

RUVIA_TEST(client_cookie_batch_insertion_preserves_path_order_and_capacity) {
    auto* const resource = std::pmr::get_default_resource();
    ruvia::http_client_config options;
    options.host_ = "example.test";
    options.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    options.max_cookies_ = 24;
    const ruvia::detail::http_client_config_storage config(options, resource);
    ruvia::detail::client_request_policy policy(config, resource);
    ruvia::detail::http_client_request_storage request("GET", "/account/profile", resource);
    const std::array<std::string_view, 3> paths{"/", "/account", "/account/profile"};
    std::vector<ruvia::http_header> received;
    for (std::size_t index = 0; index < options.max_cookies_; ++index) {
        const auto value = "c" + std::to_string(index) + "=v; Path=" + std::string(paths[index % paths.size()]);
        received.push_back(ruvia::http_header::copy_of("Set-Cookie", value, resource));
    }
    policy.retain_response_cookies(request, received);
    const std::array rejected{ruvia::http_header::copy_of("Set-Cookie", "extra=v", resource)};
    policy.retain_response_cookies(request, rejected);

    std::string expected;
    for (std::size_t group = paths.size(); group > 0; --group) {
        for (std::size_t index = group - 1; index < options.max_cookies_; index += paths.size()) {
            if (!expected.empty()) {
                expected.append("; ");
            }
            expected.append("c" + std::to_string(index) + "=v");
        }
    }
    std::pmr::vector<ruvia::http_header_view> headers(resource);
    std::pmr::string cookie_header(resource);
    policy.append_headers(request, headers, cookie_header);
    RUVIA_CHECK_EQ(std::string_view(cookie_header), std::string_view(expected));
}

RUVIA_TEST(client_cookie_configured_and_received_entries_share_capacity) {
    auto* const resource = std::pmr::get_default_resource();
    ruvia::http_client_config options;
    options.host_ = "example.test";
    options.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    options.max_cookies_ = 3;
    options.cookies_ = {{"seed", "first"}, {"seed", "last"}, {"other", "two"}};
    const ruvia::detail::http_client_config_storage config(options, resource);
    ruvia::detail::client_request_policy policy(config, resource);
    ruvia::detail::http_client_request_storage request("GET", "/account", resource);
    const std::array received_value{
        ruvia::http_header::copy_of("Set-Cookie", "leaf=three; Path=/account", resource),
        ruvia::http_header::copy_of("Set-Cookie", "extra=four", resource)};
    policy.retain_response_cookies(request, received_value);

    std::pmr::vector<ruvia::http_header_view> headers(resource);
    std::pmr::string cookie_header(resource);
    policy.append_headers(request, headers, cookie_header);
    RUVIA_CHECK_EQ(cookie_header, std::string_view("leaf=three; seed=last; other=two"));
}

RUVIA_TEST(client_cookie_insertion_failure_preserves_contents_and_releases_storage) {
    auto* const resource = std::pmr::get_default_resource();
    ruvia::http_client_config options;
    options.host_ = "example.test";
    options.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    const ruvia::detail::http_client_config_storage config(options, resource);
    ruvia::detail::http_client_request_storage request("GET", "/account", resource);
    const std::array initial_value{ruvia::http_header::copy_of("Set-Cookie", "root=one", resource)};
    const std::string value(64, 'x');
    const std::array next_value{
        ruvia::http_header::copy_of("Set-Cookie", "fresh=" + value + "; Path=/account", resource)};

    for (const auto allowed_allocations : {std::size_t{0}, std::size_t{1}}) {
        failing_memory_resource memory;
        {
            ruvia::detail::client_request_policy policy(config, &memory);
            policy.retain_response_cookies(request, initial_value);
            const auto original_allocations = memory.live_allocations();
            memory.fail_after(allowed_allocations);
            RUVIA_CHECK(ruvia::testing::throws_on([&] { policy.retain_response_cookies(request, next_value); }));
            RUVIA_CHECK_EQ(memory.live_allocations(), original_allocations);
            memory.allow_allocations();

            std::pmr::vector<ruvia::http_header_view> headers(resource);
            std::pmr::string cookie_header(resource);
            policy.append_headers(request, headers, cookie_header);
            RUVIA_CHECK_EQ(cookie_header, std::string_view("root=one"));
            headers.clear();
            cookie_header.clear();

            policy.retain_response_cookies(request, next_value);
            policy.append_headers(request, headers, cookie_header);
            RUVIA_CHECK_EQ(std::string_view(cookie_header), "fresh=" + value + "; root=one");
        }
        RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
    }
}
