#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/web/detail/server/tls/HttpServerAutoHttps.h"

#include "test_harness.h"

namespace {

using ruvia::HttpHeaderView;
using ruvia::HttpRequest;
using ruvia::RequestMemory;
using ruvia::WorkerMemory;
using ruvia::detail::makeAutoHttpsRedirectResponse;

HttpRequest makeRequest(RequestMemory& memory, std::string_view host, std::string_view target) {
    const HttpHeaderView headers[] = {HttpHeaderView{"Host", host}};
    auto [request, error] = ruvia::makeParsedHttpRequest("GET", target,
        host.empty() ? std::span<const HttpHeaderView>{} : std::span<const HttpHeaderView>{headers},
        {}, memory.resource());
    if (error) {
        throw std::runtime_error("invalid test request");
    }
    return std::move(request);
}

}  // namespace

RUVIA_TEST(auto_https_redirect_uses_host_without_request_port) {
    WorkerMemory worker;
    RequestMemory memory(worker);

    const auto withPort = makeAutoHttpsRedirectResponse(
        makeRequest(memory, "example.com:8080", "/x?q=1"), memory, 443);
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
