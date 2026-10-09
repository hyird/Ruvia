#include "dns_host.h"

#include <string>
#include <string_view>

#include "test_harness.h"

namespace {

using ruvia::detail::is_valid_dns_host;

}  // namespace

RUVIA_TEST(dns_host_accepts_ascii_labels_and_root_dot) {
    RUVIA_CHECK(is_valid_dns_host("localhost"));
    RUVIA_CHECK(is_valid_dns_host("Example.xn--bcher-kva"));
    RUVIA_CHECK(is_valid_dns_host("example.com."));
    RUVIA_CHECK(!is_valid_dns_host("example.com.."));
    RUVIA_CHECK(!is_valid_dns_host("."));
}

RUVIA_TEST(dns_host_enforces_label_and_total_lengths) {
    const auto label = std::string(63, 'a');
    const auto longest = label + "." + label + "." + label + "." + std::string(61, 'b');
    RUVIA_CHECK(is_valid_dns_host(label));
    RUVIA_CHECK(is_valid_dns_host(longest));
    RUVIA_CHECK(is_valid_dns_host(longest + "."));
    RUVIA_CHECK(!is_valid_dns_host(std::string(64, 'a')));
    RUVIA_CHECK(!is_valid_dns_host(longest + "b"));
}

RUVIA_TEST(dns_host_rejects_non_ascii_bounded_and_numeric_ipv4_names) {
    constexpr char bounded[] = "example.com/trailing";
    constexpr char embedded_null[] = "example.com\0suffix";
    RUVIA_CHECK(is_valid_dns_host(std::string_view(bounded, 11)));
    RUVIA_CHECK(!is_valid_dns_host(std::string_view(embedded_null, sizeof(embedded_null) - 1)));
    constexpr char non_ascii[] = "caf\xC3\xA9.example";
    RUVIA_CHECK(!is_valid_dns_host(std::string_view(non_ascii, sizeof(non_ascii) - 1)));
    RUVIA_CHECK(!is_valid_dns_host("bad host"));
    RUVIA_CHECK(!is_valid_dns_host("bad_name.example"));
    RUVIA_CHECK(!is_valid_dns_host("-bad.example"));
    RUVIA_CHECK(!is_valid_dns_host("bad-.example"));
    RUVIA_CHECK(!is_valid_dns_host("127.0.0.1"));
    RUVIA_CHECK(!is_valid_dns_host("999.999.999.999"));
    RUVIA_CHECK(!is_valid_dns_host("1.2.3.4."));
    RUVIA_CHECK(is_valid_dns_host("1.2.3.example"));
}
