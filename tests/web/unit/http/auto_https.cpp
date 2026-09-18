#include <string_view>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/detail/server/tls/HttpServerAutoHttps.h"

#include "test_harness.h"

namespace {

using ruvia::HttpHeaderView;
using ruvia::HttpRequest;
using ruvia::RequestMemory;
using ruvia::WorkerMemory;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::makeAutoHttpsRedirectResponse;
using ruvia::detail::RequestKnownHeader;

HttpRequest makeRequest(RequestMemory& memory, std::string_view host, std::string_view path,
    std::string_view query = {}) {
    auto request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setResource(request, memory.resource());
    HttpRequestAccess::setPath(request, path);
    HttpRequestAccess::setQueryString(request, query);
    if (!host.empty()) {
        HttpRequestAccess::addHeader(request, HttpHeaderView{"Host", host},
            HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kHost));
    }
    return request;
}

}  // namespace

RUVIA_TEST(auto_https_redirect_uses_host_without_request_port) {
    WorkerMemory worker;
    RequestMemory memory(worker);

    const auto withPort = makeAutoHttpsRedirectResponse(
        makeRequest(memory, "example.com:8080", "/x", "q=1"), memory, 443);
    RUVIA_CHECK_EQ(withPort.status(), ruvia::http_status::kPermanentRedirect);
    RUVIA_CHECK_EQ(
        withPort.header("Location").value_or(std::string_view{}), std::string_view("https://example.com/x?q=1"));
    RUVIA_CHECK_EQ(
        withPort.header("Cache-Control").value_or(std::string_view{}), std::string_view("private"));

    const auto ipv6 = makeAutoHttpsRedirectResponse(
        makeRequest(memory, "[2001:db8::1]:80", "/"), memory, 443);
    RUVIA_CHECK_EQ(ipv6.header("Location").value_or(std::string_view{}),
        std::string_view("https://[2001:db8::1]/"));

    const auto customPort = makeAutoHttpsRedirectResponse(
        makeRequest(memory, "example.com", "/app"), memory, 8443);
    RUVIA_CHECK_EQ(customPort.header("Location").value_or(std::string_view{}),
        std::string_view("https://example.com:8443/app"));
}
