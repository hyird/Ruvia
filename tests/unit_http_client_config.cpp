#include "test_harness.h"

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <string>
#include <string_view>

#include "http/client/HttpClientConfigValidation.h"
#include "ruvia/http/HttpClient.h"

namespace {

using ruvia::HttpClientConfig;
using ruvia::detail::makeHttpClientHostHeader;
using ruvia::detail::validateHttpClientConfig;

template <typename Fn>
bool throwsOn(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

HttpClientConfig configWithHost(std::string_view host, std::uint16_t port, bool tls = false) {
    HttpClientConfig config;
    config.host.assign(host.data(), host.size());
    config.port = port;
    config.tls = tls;
    return config;
}

std::string hostHeader(const HttpClientConfig& config) {
    const auto header = makeHttpClientHostHeader(config, std::pmr::get_default_resource());
    return std::string(header.data(), header.size());
}

}  // namespace

RUVIA_TEST(http_client_config_validation_checks_every_field) {
    using std::chrono::milliseconds;

    // A populated config is valid; unlike the DB/Redis configs there is no default
    // host, so the host must be supplied.
    RUVIA_CHECK(!throwsOn([] { validateHttpClientConfig(configWithHost("example.com", 80)); }));

    // Host, port and pool size each have a required-value guard.
    RUVIA_CHECK(throwsOn([] { validateHttpClientConfig(configWithHost("", 80)); }));
    RUVIA_CHECK(throwsOn([] { validateHttpClientConfig(configWithHost("example.com", 0)); }));
    RUVIA_CHECK(throwsOn([] {
        auto c = configWithHost("example.com", 80);
        c.poolSizePerWorker = 0;
        validateHttpClientConfig(c);
    }));

    // Every one of the three timeouts must be non-negative -- a negative value in
    // any of them is rejected (verifies the whole fold is wired, not just one).
    RUVIA_CHECK(throwsOn([] {
        auto c = configWithHost("example.com", 80);
        c.connectTimeout = milliseconds(-1);
        validateHttpClientConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        auto c = configWithHost("example.com", 80);
        c.requestTimeout = milliseconds(-1);
        validateHttpClientConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        auto c = configWithHost("example.com", 80);
        c.acquireTimeout = milliseconds(-1);
        validateHttpClientConfig(c);
    }));
}

RUVIA_TEST(http_client_host_header_brackets_ipv6_and_omits_default_port) {
    // The default port for the scheme is omitted: 80 for http, 443 for https.
    RUVIA_CHECK_EQ(hostHeader(configWithHost("example.com", 80, false)), std::string("example.com"));
    RUVIA_CHECK_EQ(hostHeader(configWithHost("example.com", 443, true)), std::string("example.com"));

    // A non-default port is appended.
    RUVIA_CHECK_EQ(hostHeader(configWithHost("example.com", 8080, false)), std::string("example.com:8080"));

    // The default depends on the scheme: 80 is not default under TLS, and 443 is
    // not default without TLS, so each is included.
    RUVIA_CHECK_EQ(hostHeader(configWithHost("example.com", 80, true)), std::string("example.com:80"));
    RUVIA_CHECK_EQ(hostHeader(configWithHost("example.com", 443, false)), std::string("example.com:443"));

    // An IPv6 literal (the host contains ':') is wrapped in brackets, and the
    // default-port omission still applies inside the bracketed form.
    RUVIA_CHECK_EQ(hostHeader(configWithHost("::1", 80, false)), std::string("[::1]"));
    RUVIA_CHECK_EQ(hostHeader(configWithHost("::1", 8080, false)), std::string("[::1]:8080"));
    RUVIA_CHECK_EQ(hostHeader(configWithHost("2001:db8::1", 443, true)), std::string("[2001:db8::1]"));
}

#endif  // RUVIA_ENABLE_HTTP_CLIENT
