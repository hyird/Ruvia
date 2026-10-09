#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"

#include "server/http_server_auto_https.h"
#include "test_harness.h"

namespace {

using ruvia::http_header_view;
using ruvia::http_request;
using ruvia::request_memory;
using ruvia::worker_memory;
using ruvia::detail::make_auto_https_redirect_response;

http_request make_request(request_memory& memory, std::string_view host, std::string_view target) {
    const http_header_view headers[] = {http_header_view{"Host", host}};
    auto [request, error] = ruvia::make_parsed_http_request("GET", target,
        host.empty() ? std::span<const http_header_view>{} : std::span<const http_header_view>{headers},
        {}, memory.resource());
    if (error) {
        throw std::runtime_error("invalid test request");
    }
    return std::move(request);
}

}  // namespace

RUVIA_TEST(auto_https_redirect_uses_host_without_request_port) {
    worker_memory worker;
    request_memory memory(worker);

    const auto with_port = make_auto_https_redirect_response(
        make_request(memory, "example.com:8080", "/x?q=1"), memory, 443);
    RUVIA_CHECK_EQ(with_port.status(), ruvia::http_status::permanent_redirect);
    RUVIA_CHECK_EQ(
        with_port.header("Location").value_or(std::string_view{}), std::string_view("https://example.com/x?q=1"));
    RUVIA_CHECK_EQ(
        with_port.header("Cache-Control").value_or(std::string_view{}), std::string_view("private"));

    const auto ipv6 = make_auto_https_redirect_response(
        make_request(memory, "[2001:db8::1]:80", "/"), memory, 443);
    RUVIA_CHECK_EQ(ipv6.header("Location").value_or(std::string_view{}),
        std::string_view("https://[2001:db8::1]/"));

    const auto empty_query = make_auto_https_redirect_response(
        make_request(memory, "example.com", "/x?"), memory, 443);
    RUVIA_CHECK_EQ(empty_query.header("Location").value_or(std::string_view{}),
        std::string_view("https://example.com/x?"));

    const auto encoded_question = make_auto_https_redirect_response(
        make_request(memory, "example.com", "/x%3F"), memory, 443);
    RUVIA_CHECK_EQ(encoded_question.header("Location").value_or(std::string_view{}),
        std::string_view("https://example.com/x%3F"));

    const auto absolute_empty_query = make_auto_https_redirect_response(
        make_request(memory, "example.com:80", "http://example.com/x?"), memory, 443);
    RUVIA_CHECK_EQ(absolute_empty_query.header("Location").value_or(std::string_view{}),
        std::string_view("https://example.com/x?"));

    const auto custom_port = make_auto_https_redirect_response(
        make_request(memory, "example.com", "/app"), memory, 8443);
    RUVIA_CHECK_EQ(custom_port.header("Location").value_or(std::string_view{}),
        std::string_view("https://example.com:8443/app"));
}
