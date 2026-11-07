#include <gtest/gtest.h>

#include <memory_resource>
#include <string_view>
#include <variant>

#include "ruvia/http/http1_response_head_error.h"
#include "ruvia/http/http1_server_semantics.h"
#include "ruvia/http/http_response_server.h"

namespace {

template <typename fn_type>
[[nodiscard]] bool throws_head_error(fn_type&& fn) {
    try {
        fn();
    } catch (const ruvia::http1_response_head_error&) {
        return true;
    }
    return false;
}

}  // namespace

TEST(Ruvia, http1_connect_head_transfers_to_tunnel_without_message_framing_for_every_successful_status) {
    std::pmr::unsynchronized_pool_resource resource;
    for (const auto version : {ruvia::http_protocol_version::http10, ruvia::http_protocol_version::http11}) {
        for (unsigned status = 200; status != 300; ++status) {
            ruvia::http_response response(&resource);
            response.status(ruvia::http_status_code::from_value(status));
            response.header("x-tunnel", "established");
            const auto plan = ruvia::prepare_http1_connect_response_head(response, version);
            ruvia::http_response_head_buffer head{std::pmr::polymorphic_allocator<char>(&resource)};
            ruvia::append_http1_response_head(response, head, plan);
            EXPECT_TRUE(head.view().starts_with("HTTP/1.1 "));
            EXPECT_TRUE(head.view().find("x-tunnel: established\r\n") != std::string_view::npos);
            EXPECT_TRUE(head.view().find("Content-Length:") == std::string_view::npos);
            EXPECT_TRUE(head.view().find("Transfer-Encoding:") == std::string_view::npos);
            EXPECT_TRUE(head.view().ends_with("\r\n\r\n"));
        }
    }
}

TEST(Ruvia, http1_connect_head_rejects_failed_status_payload_and_framing_fields_before_commit) {
    std::pmr::unsynchronized_pool_resource resource;
    for (const auto name : {"Content-Length", "Transfer-Encoding"}) {
        ruvia::http_response response(&resource);
        response.header(name, name == std::string_view("Content-Length") ? "0" : "chunked");
        EXPECT_TRUE(throws_head_error([&] {
            (void)ruvia::prepare_http1_connect_response_head(response, ruvia::http_protocol_version::http11);
        }));
    }
    ruvia::http_response response(&resource);
    response.status(ruvia::http_status::forbidden);
    EXPECT_TRUE(throws_head_error([&] {
        (void)ruvia::prepare_http1_connect_response_head(response, ruvia::http_protocol_version::http11);
    }));
    response.status(ruvia::http_status::ok);
    response.body("HTTP payload");
    EXPECT_TRUE(throws_head_error([&] {
        (void)ruvia::prepare_http1_connect_response_head(response, ruvia::http_protocol_version::http11);
    }));
}
