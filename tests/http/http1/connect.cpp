#include <memory_resource>
#include <string_view>
#include <variant>

#include "ruvia/http/http1_connect.h"
#include "ruvia/http/http_response_server.h"

#include "test_harness.h"

RUVIA_TEST(http1_connect_head_transfers_to_tunnel_without_message_framing_for_every_successful_status) {
    std::pmr::unsynchronized_pool_resource resource;
    for (const auto version : {ruvia::http_protocol_version::http10, ruvia::http_protocol_version::http11}) {
        for (unsigned status = 200; status != 300; ++status) {
            ruvia::http_response response({.resource_ = &resource});
            response.status(ruvia::http_status_code::from_value(static_cast<std::uint16_t>(status)));
            response.header("x-tunnel", "established");
            const auto plan = ruvia::prepare_http1_connect_response_head(response, version);
            RUVIA_CHECK((plan.index() == 0));
            if ((plan.index() != 0)) {
                continue;
            }
            ruvia::http_response_head_buffer head{std::pmr::polymorphic_allocator<char>(&resource)};
            ruvia::append_http1_response_head(response, head, std::get<0>(plan));
            RUVIA_CHECK(head.view().starts_with(version == ruvia::http_protocol_version::http10 ? "HTTP/1.0 " : "HTTP/1.1 "));
            RUVIA_CHECK(head.view().find("x-tunnel: established\r\n") != std::string_view::npos);
            RUVIA_CHECK(head.view().find("Content-Length:") == std::string_view::npos);
            RUVIA_CHECK(head.view().find("Transfer-Encoding:") == std::string_view::npos);
            RUVIA_CHECK(head.view().ends_with("\r\n\r\n"));
        }
    }
}

RUVIA_TEST(http1_connect_head_rejects_failed_status_payload_and_framing_fields_before_commit) {
    std::pmr::unsynchronized_pool_resource resource;
    for (const auto name : {"Content-Length", "Transfer-Encoding"}) {
        ruvia::http_response response({.resource_ = &resource});
        response.header(name, name == std::string_view("Content-Length") ? "0" : "chunked");
        RUVIA_CHECK((ruvia::prepare_http1_connect_response_head(response, ruvia::http_protocol_version::http11).index() != 0));
    }
    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::forbidden);
    RUVIA_CHECK((ruvia::prepare_http1_connect_response_head(response, ruvia::http_protocol_version::http11).index() != 0));
    response.status(ruvia::http_status::ok);
    response.body("HTTP payload");
    RUVIA_CHECK((ruvia::prepare_http1_connect_response_head(response, ruvia::http_protocol_version::http11).index() != 0));
}
