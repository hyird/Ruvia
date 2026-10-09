#include <memory_resource>
#include <string_view>
#include <variant>

#include "ruvia/http/Http1Connect.h"
#include "ruvia/http/HttpResponseServer.h"

#include "test_harness.h"

RUVIA_TEST(http1ConnectHeadTransfersToTunnelWithoutMessageFramingForEverySuccessfulStatus) {
    std::pmr::unsynchronized_pool_resource resource;
    for (const auto version : {ruvia::HttpProtocolVersion::kHttp10, ruvia::HttpProtocolVersion::kHttp11}) {
        for (unsigned status = 200; status != 300; ++status) {
            ruvia::HttpResponse response({.resource = &resource});
            response.status(ruvia::HttpStatusCode::fromValue(static_cast<std::uint16_t>(status)));
            response.header("x-tunnel", "established");
            const auto plan = ruvia::prepareHttp1ConnectResponseHead(response, version);
            RUVIA_CHECK((plan.index() == 0));
            if ((plan.index() != 0)) {
                continue;
            }
            ruvia::HttpResponseHeadBuffer head{std::pmr::polymorphic_allocator<char>(&resource)};
            ruvia::appendHttp1ResponseHead(response, head, std::get<0>(plan));
            RUVIA_CHECK(head.view().starts_with(version == ruvia::HttpProtocolVersion::kHttp10 ? "HTTP/1.0 " : "HTTP/1.1 "));
            RUVIA_CHECK(head.view().find("x-tunnel: established\r\n") != std::string_view::npos);
            RUVIA_CHECK(head.view().find("Content-Length:") == std::string_view::npos);
            RUVIA_CHECK(head.view().find("Transfer-Encoding:") == std::string_view::npos);
            RUVIA_CHECK(head.view().ends_with("\r\n\r\n"));
        }
    }
}

RUVIA_TEST(http1ConnectHeadRejectsFailedStatusPayloadAndFramingFieldsBeforeCommit) {
    std::pmr::unsynchronized_pool_resource resource;
    for (const auto name : {"Content-Length", "Transfer-Encoding"}) {
        ruvia::HttpResponse response({.resource = &resource});
        response.header(name, name == std::string_view("Content-Length") ? "0" : "chunked");
        RUVIA_CHECK((ruvia::prepareHttp1ConnectResponseHead(response, ruvia::HttpProtocolVersion::kHttp11).index() != 0));
    }
    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kForbidden);
    RUVIA_CHECK((ruvia::prepareHttp1ConnectResponseHead(response, ruvia::HttpProtocolVersion::kHttp11).index() != 0));
    response.status(ruvia::http_status::kOk);
    response.body("HTTP payload");
    RUVIA_CHECK((ruvia::prepareHttp1ConnectResponseHead(response, ruvia::HttpProtocolVersion::kHttp11).index() != 0));
}
