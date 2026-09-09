#include <string>
#include <string_view>

#include "ruvia/core/detail/util/DnsHost.h"

#include "test_harness.h"

namespace {

using ruvia::detail::isValidDnsHost;

}  // namespace

RUVIA_TEST(dns_host_accepts_ascii_labels_and_root_dot) {
    RUVIA_CHECK(isValidDnsHost("localhost"));
    RUVIA_CHECK(isValidDnsHost("Example.xn--bcher-kva"));
    RUVIA_CHECK(isValidDnsHost("example.com."));
    RUVIA_CHECK(!isValidDnsHost("example.com.."));
    RUVIA_CHECK(!isValidDnsHost("."));
}

RUVIA_TEST(dns_host_enforces_label_and_total_lengths) {
    const auto label = std::string(63, 'a');
    const auto longest = label + "." + label + "." + label + "." + std::string(61, 'b');
    RUVIA_CHECK(isValidDnsHost(label));
    RUVIA_CHECK(isValidDnsHost(longest));
    RUVIA_CHECK(isValidDnsHost(longest + "."));
    RUVIA_CHECK(!isValidDnsHost(std::string(64, 'a')));
    RUVIA_CHECK(!isValidDnsHost(longest + "b"));
}

RUVIA_TEST(dns_host_rejects_non_ascii_bounded_and_numeric_ipv4_names) {
    constexpr char bounded[] = "example.com/trailing";
    constexpr char embeddedNull[] = "example.com\0suffix";
    RUVIA_CHECK(isValidDnsHost(std::string_view(bounded, 11)));
    RUVIA_CHECK(!isValidDnsHost(std::string_view(embeddedNull, sizeof(embeddedNull) - 1)));
    constexpr char nonAscii[] = "caf\xC3\xA9.example";
    RUVIA_CHECK(!isValidDnsHost(std::string_view(nonAscii, sizeof(nonAscii) - 1)));
    RUVIA_CHECK(!isValidDnsHost("bad host"));
    RUVIA_CHECK(!isValidDnsHost("bad_name.example"));
    RUVIA_CHECK(!isValidDnsHost("-bad.example"));
    RUVIA_CHECK(!isValidDnsHost("bad-.example"));
    RUVIA_CHECK(!isValidDnsHost("127.0.0.1"));
    RUVIA_CHECK(!isValidDnsHost("999.999.999.999"));
    RUVIA_CHECK(!isValidDnsHost("1.2.3.4."));
    RUVIA_CHECK(isValidDnsHost("1.2.3.example"));
}
