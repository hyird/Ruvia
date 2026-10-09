#include <chrono>
#include <concepts>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_set_cookie.h"
#include "ruvia/http/http_set_cookie_plan.h"

#include "cookie/cookie_validation.h"
#include "test_harness.h"

namespace {

// True if validate_cookie rejects the options (throws invalid_argument).
bool rejects(const ruvia::cookie_options& options) {
    try {
        ruvia::detail::validate_cookie("sid", "value", options);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(cookie_public_validation_and_prefix_parsing) {
    RUVIA_CHECK(ruvia::is_valid_cookie_value("session-token"));
    RUVIA_CHECK(!ruvia::is_valid_cookie_value("invalid;value"));
    RUVIA_CHECK(ruvia::cookie_name_starts_with_ignore_case("__secure-id", "__Secure-"));
    RUVIA_CHECK(!ruvia::cookie_name_starts_with_ignore_case("id", "__Host-"));
    RUVIA_CHECK_EQ(ruvia::http_cookie_prefix_text(ruvia::cookie_prefix::host),
        std::string_view("__Host-"));
}

RUVIA_TEST(cookie_borrowed_text_accepts_stable_string_owners) {
    const std::string path = "/account";
    const std::string domain = "example.com";
    std::string name = "sid";
    const std::string value = "value";

    ruvia::cookie_options options;
    options.path_ = path;
    options.domain_ = domain;
    RUVIA_CHECK(!rejects(options));

    const ruvia::set_cookie_plan plan(name, value, options);
    std::string wire(plan.size(), '\0');
    plan.write(wire.data());
    name.assign("changed");
    RUVIA_CHECK_EQ(wire, std::string("sid=value; Path=/account; Domain=example.com"));
    const auto parsed_value = ruvia::parse_set_cookie(wire);
    RUVIA_CHECK(parsed_value.has_value());
    if (parsed_value) {
        RUVIA_CHECK_EQ(parsed_value->name(), "sid");
        RUVIA_CHECK_EQ(parsed_value->path(), "/account");
        RUVIA_CHECK_EQ(parsed_value->domain(), "example.com");
        RUVIA_CHECK(parsed_value->name().data() == wire.data());
        RUVIA_CHECK(parsed_value->path().data() == wire.data() + wire.find("/account"));
        RUVIA_CHECK(parsed_value->domain().data() == wire.data() + wire.find("example.com"));
    }
}

RUVIA_TEST(cookie_expires_formats_historical_dates_at_second_resolution) {
    using namespace std::chrono;
    ruvia::cookie_options options;
    options.expires_ = system_clock::time_point{seconds{-315619200}};
    RUVIA_CHECK(!rejects(options));
    const ruvia::set_cookie_plan historical("sid", "value", options);
    std::string wire(historical.size(), '\0');
    historical.write(wire.data());
    RUVIA_CHECK_EQ(wire, std::string("sid=value; Path=/; Expires=Fri, 01 Jan 1960 00:00:00 GMT"));

    options.expires_ = system_clock::time_point{} - system_clock::duration{1};
    const ruvia::set_cookie_plan fractional("sid", "value", options);
    wire.resize(fractional.size());
    fractional.write(wire.data());
    RUVIA_CHECK_EQ(wire, std::string("sid=value; Path=/; Expires=Wed, 31 Dec 1969 23:59:59 GMT"));
}

RUVIA_TEST(cookie_expires_rejects_dates_before_the_cookie_calendar_range) {
    using namespace std::chrono;
    constexpr auto cutoff = duration_cast<seconds>(sys_days{year{1601} / January / 1}.time_since_epoch());
    // Clocks with nanosecond int64 storage cannot represent dates this early;
    // wider calendar ranges (including MSVC's 100ns clock) exercise the edge.
    if constexpr (ceil<seconds>(system_clock::duration::min()) <= cutoff) {
        ruvia::cookie_options options;
        options.expires_ = system_clock::time_point{duration_cast<system_clock::duration>(cutoff)};
        RUVIA_CHECK(!rejects(options));
        const ruvia::set_cookie_plan first("sid", "value", options);
        std::string wire(first.size(), '\0');
        first.write(wire.data());
        RUVIA_CHECK(wire.find("01 Jan 1601") != std::string::npos);
        options.expires_ = *options.expires_ - system_clock::duration{1};
        RUVIA_CHECK(rejects(options));
        bool rejected = false;
        try {
            (void)ruvia::set_cookie_plan("sid", "value", options);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
}

RUVIA_TEST(cookie_plan_rejects_wrapped_wire_length_before_scanning) {
    const auto oversized_name = std::string_view("x", std::numeric_limits<std::size_t>::max());
    bool length_error = false;
    try {
        ruvia::cookie_options options;
        (void)ruvia::set_cookie_plan(oversized_name, "value", options);
    } catch (const std::length_error&) {
        length_error = true;
    } catch (...) {
    }
    RUVIA_CHECK(length_error);
}

RUVIA_TEST(cookie_samesite_enum_maps_to_wire_tokens) {
    ruvia::cookie_options strict;
    strict.same_site_ = ruvia::cookie_same_site::strict;
    RUVIA_CHECK(!rejects(strict));

    ruvia::cookie_options lax;
    lax.same_site_ = ruvia::cookie_same_site::lax;
    RUVIA_CHECK(!rejects(lax));

    ruvia::cookie_options none;
    none.same_site_ = ruvia::cookie_same_site::none;
    none.secure_ = ruvia::cookie_attribute_policy::emit;
    RUVIA_CHECK(!rejects(none));

    RUVIA_CHECK(!ruvia::cookie_options{}.same_site_.has_value());
    RUVIA_CHECK_EQ(ruvia::detail::cookie_same_site_token(ruvia::cookie_same_site::strict),
        std::string_view("Strict"));
    RUVIA_CHECK_EQ(
        ruvia::detail::cookie_same_site_token(ruvia::cookie_same_site::lax), std::string_view("Lax"));
    RUVIA_CHECK_EQ(
        ruvia::detail::cookie_same_site_token(ruvia::cookie_same_site::none), std::string_view("None"));
}

RUVIA_TEST(cookie_samesite_none_requires_secure) {
    ruvia::cookie_options insecure_none;
    insecure_none.same_site_ = ruvia::cookie_same_site::none;
    insecure_none.secure_ = ruvia::cookie_attribute_policy::omit;
    RUVIA_CHECK(rejects(insecure_none));  // RFC 6265bis §5.5

    ruvia::cookie_options secure_none;
    secure_none.same_site_ = ruvia::cookie_same_site::none;
    secure_none.secure_ = ruvia::cookie_attribute_policy::emit;
    RUVIA_CHECK(!rejects(secure_none));
}

RUVIA_TEST(cookie_value_char_validation) {
    using ruvia::detail::is_valid_cookie_value;
    RUVIA_CHECK(is_valid_cookie_value("abc123"));
    RUVIA_CHECK(is_valid_cookie_value("a-b_c.d~e"));
    RUVIA_CHECK(is_valid_cookie_value(""));                            // an empty value is valid
    RUVIA_CHECK(!is_valid_cookie_value("a b"));                        // space
    RUVIA_CHECK(!is_valid_cookie_value("a;b"));                        // ';' would inject an attribute
    RUVIA_CHECK(!is_valid_cookie_value("a,b"));                        // ','
    RUVIA_CHECK(!is_valid_cookie_value("a\"b"));                       // '"'
    RUVIA_CHECK(!is_valid_cookie_value("a\\b"));                       // backslash
    RUVIA_CHECK(!is_valid_cookie_value(std::string_view("a\rb", 3)));  // CR
    RUVIA_CHECK(
        !is_valid_cookie_value(std::string_view("a\x7f"
                                                "b",
            3)));  // DEL
}

RUVIA_TEST(cookie_path_octets_follow_set_cookie_grammar) {
    using ruvia::detail::is_valid_cookie_attribute;
    RUVIA_CHECK(is_valid_cookie_attribute("/path/to"));
    RUVIA_CHECK(is_valid_cookie_attribute("example.com"));
    RUVIA_CHECK(is_valid_cookie_attribute(""));
    RUVIA_CHECK(!is_valid_cookie_attribute("a;b"));                        // ';' would inject another attribute
    RUVIA_CHECK(!is_valid_cookie_attribute(std::string_view("a\rb", 3)));  // CR (header injection)
    RUVIA_CHECK(!is_valid_cookie_attribute(std::string_view("a\nb", 3)));  // LF
    RUVIA_CHECK(!is_valid_cookie_attribute(std::string_view("a\0b", 3)));  // NUL
    // Non-CR/LF control bytes are also forbidden HTTP field-value octets (RFC 9110
    // 5.5) and previously slipped through into the raw Set-Cookie value.
    RUVIA_CHECK(
        !is_valid_cookie_attribute("a\x0b"
                                   "b"));  // vertical tab
    RUVIA_CHECK(
        !is_valid_cookie_attribute("a\x0c"
                                   "b"));  // form feed
    RUVIA_CHECK(
        !is_valid_cookie_attribute("a\x01"
                                   "b"));  // SOH
    RUVIA_CHECK(
        !is_valid_cookie_attribute("a\x7f"
                                   "b"));  // DEL
    // RFC 6265bis av-octet is ASCII %x20-3A / %x3C-7E. SP is
    // valid, but HTAB and obs-text are not cookie Path bytes even though the
    // surrounding HTTP field-value grammar can carry them.
    RUVIA_CHECK(is_valid_cookie_attribute("/a path"));            // SP
    RUVIA_CHECK(!is_valid_cookie_attribute("a\tb"));              // HTAB
    RUVIA_CHECK(!is_valid_cookie_attribute("caf\xc3\xa9/path"));  // obs-text
}

RUVIA_TEST(cookie_domain_requires_dns_subdomain_syntax) {
    const auto accepts_domain = [](std::string_view domain) {
        ruvia::cookie_options options;
        options.domain_ = domain;
        return !rejects(options);
    };

    RUVIA_CHECK(accepts_domain("example.com"));
    RUVIA_CHECK(accepts_domain("EXAMPLE.com"));
    RUVIA_CHECK(accepts_domain("localhost"));
    RUVIA_CHECK(accepts_domain("3.example"));
    RUVIA_CHECK(accepts_domain("xn--bcher-kva.example"));

    // Domain= is a DNS subdomain, not the generic Set-Cookie av-octet syntax
    // used by Path. Emitting any of these values produces a non-conforming
    // Set-Cookie field that user agents ignore or interpret inconsistently.
    RUVIA_CHECK(!accepts_domain(".example.com"));
    RUVIA_CHECK(!accepts_domain("example.com."));
    RUVIA_CHECK(!accepts_domain("bad domain.example"));
    RUVIA_CHECK(!accepts_domain("bad_domain.example"));
    RUVIA_CHECK(!accepts_domain("-bad.example"));
    RUVIA_CHECK(!accepts_domain("bad-.example"));
    RUVIA_CHECK(!accepts_domain("bad..example"));
    RUVIA_CHECK(!accepts_domain("caf\xc3\xa9.example"));
    RUVIA_CHECK(!accepts_domain(std::string(64, 'a') + ".example"));
}

RUVIA_TEST(cookie_priority_enum_maps_to_wire_tokens) {
    using ruvia::detail::cookie_priority_token;
    RUVIA_CHECK(!ruvia::cookie_options{}.priority_.has_value());
    RUVIA_CHECK_EQ(cookie_priority_token(ruvia::cookie_priority::low), std::string_view("Low"));
    RUVIA_CHECK_EQ(cookie_priority_token(ruvia::cookie_priority::medium), std::string_view("Medium"));
    RUVIA_CHECK_EQ(cookie_priority_token(ruvia::cookie_priority::high), std::string_view("High"));
}

RUVIA_TEST(cookie_validation_rejects_injection_and_bad_options) {
    const auto rejects_cookie = [](std::string_view name, std::string_view value,
                                    const ruvia::cookie_options& options) {
        try {
            ruvia::detail::validate_cookie(name, value, options);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    const ruvia::cookie_options clean;
    RUVIA_CHECK(!rejects_cookie("sid", "value", clean));
    RUVIA_CHECK(rejects_cookie("bad name", "value", clean));  // space -> not a token name
    RUVIA_CHECK(rejects_cookie("sid", "a;b", clean));         // ';' in value
    ruvia::cookie_options bad_path;
    bad_path.path_ = "a;b";
    RUVIA_CHECK(rejects_cookie("sid", "value", bad_path));  // ';' in path attribute
    ruvia::cookie_options bad_priority;
    bad_priority.priority_ = static_cast<ruvia::cookie_priority>(255);
    RUVIA_CHECK(rejects_cookie("sid", "value", bad_priority));
    ruvia::cookie_options bad_same_site;
    bad_same_site.same_site_ = static_cast<ruvia::cookie_same_site>(255);
    RUVIA_CHECK(rejects_cookie("sid", "value", bad_same_site));
    ruvia::cookie_options bad_prefix;
    bad_prefix.prefix_ = static_cast<ruvia::cookie_prefix>(255);
    RUVIA_CHECK(rejects_cookie("sid", "value", bad_prefix));
    ruvia::cookie_options partitioned;
    partitioned.partitioned_ = ruvia::cookie_attribute_policy::emit;  // partitioned requires Secure
    RUVIA_CHECK(rejects_cookie("sid", "value", partitioned));
    ruvia::cookie_options bad_http_only;
    bad_http_only.http_only_ = static_cast<ruvia::cookie_attribute_policy>(255);
    RUVIA_CHECK(rejects_cookie("sid", "value", bad_http_only));
    ruvia::cookie_options bad_secure;
    bad_secure.secure_ = static_cast<ruvia::cookie_attribute_policy>(255);
    RUVIA_CHECK(rejects_cookie("sid", "value", bad_secure));
    ruvia::cookie_options bad_partitioned;
    bad_partitioned.partitioned_ = static_cast<ruvia::cookie_attribute_policy>(255);
    RUVIA_CHECK(rejects_cookie("sid", "value", bad_partitioned));
}

RUVIA_TEST(cookie_secure_prefix_requires_secure) {
    ruvia::cookie_options secured;
    secured.prefix_ = ruvia::cookie_prefix::secure;
    secured.secure_ = ruvia::cookie_attribute_policy::emit;
    RUVIA_CHECK(!rejects(secured));

    ruvia::cookie_options insecure;
    insecure.prefix_ = ruvia::cookie_prefix::secure;
    insecure.secure_ = ruvia::cookie_attribute_policy::omit;
    RUVIA_CHECK(rejects(insecure));  // __Secure- requires Secure
}

RUVIA_TEST(cookie_host_prefix_requires_secure_root_path_no_domain) {
    // __Host- is the strictest prefix (RFC 6265bis 4.1.3.2).
    ruvia::cookie_options valid;
    valid.prefix_ = ruvia::cookie_prefix::host;
    valid.secure_ = ruvia::cookie_attribute_policy::emit;  // path defaults to "/", domain is empty
    RUVIA_CHECK(!rejects(valid));

    ruvia::cookie_options not_secure = valid;
    not_secure.secure_ = ruvia::cookie_attribute_policy::omit;
    RUVIA_CHECK(rejects(not_secure));

    ruvia::cookie_options sub_path = valid;
    sub_path.path_ = "/sub";
    RUVIA_CHECK(rejects(sub_path));  // Path must be exactly "/"

    ruvia::cookie_options with_domain = valid;
    with_domain.domain_ = "example.com";
    RUVIA_CHECK(rejects(with_domain));  // Domain must be absent
}

RUVIA_TEST(cookie_literal_prefix_name_enforces_requirements) {
    const auto rejects_with_name = [](std::string_view name, const ruvia::cookie_options& options) {
        try {
            ruvia::detail::validate_cookie(name, "value", options);
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };

    // RFC 6265bis user agents match these prefixes case-insensitively. Reject
    // every spelling the UA would reject instead of emitting a silently dropped
    // cookie.
    ruvia::cookie_options insecure;                            // Secure defaults to omitted
    RUVIA_CHECK(rejects_with_name("__Secure-tok", insecure));  // __Secure- requires Secure
    RUVIA_CHECK(rejects_with_name("__secure-tok", insecure));
    RUVIA_CHECK(rejects_with_name("__SeCuRe-tok", insecure));

    ruvia::cookie_options host_bad_domain;
    host_bad_domain.secure_ = ruvia::cookie_attribute_policy::emit;
    host_bad_domain.domain_ = "example.com";  // __Host- forbids Domain
    RUVIA_CHECK(rejects_with_name("__Host-sid", host_bad_domain));
    RUVIA_CHECK(rejects_with_name("__HOST-sid", host_bad_domain));
    RUVIA_CHECK(rejects_with_name("__hOsT-sid", host_bad_domain));

    // A literal-prefixed name that meets the constraints is accepted.
    ruvia::cookie_options ok_secure;
    ok_secure.secure_ = ruvia::cookie_attribute_policy::emit;
    RUVIA_CHECK(!rejects_with_name("__Secure-tok", ok_secure));
    RUVIA_CHECK(!rejects_with_name("__sEcUrE-tok", ok_secure));
    ruvia::cookie_options ok_host;
    ok_host.secure_ = ruvia::cookie_attribute_policy::emit;  // path defaults to "/", domain empty
    RUVIA_CHECK(!rejects_with_name("__Host-sid", ok_host));
    RUVIA_CHECK(!rejects_with_name("__hOsT-sid", ok_host));

    // A name that only resembles a prefix (missing the trailing '-') is unaffected.
    ruvia::cookie_options plain;
    RUVIA_CHECK(!rejects_with_name("__Secured", plain));
    RUVIA_CHECK(!rejects_with_name("__Hostname", plain));
}

RUVIA_TEST(cookie_max_age_capped_at_400_days) {
    ruvia::cookie_options at_cap;
    at_cap.max_age_ = std::chrono::seconds(34560000);  // exactly 400 days is allowed
    RUVIA_CHECK(!rejects(at_cap));

    ruvia::cookie_options over_cap;
    over_cap.max_age_ = std::chrono::seconds(34560001);
    RUVIA_CHECK(rejects(over_cap));

    ruvia::cookie_options deletion;
    deletion.max_age_ = std::chrono::seconds(0);
    RUVIA_CHECK(!rejects(deletion));

    ruvia::cookie_options negative;
    negative.max_age_ = std::chrono::seconds(-1);
    RUVIA_CHECK(rejects(negative));
}

RUVIA_TEST(cookie_request_pair_keeps_equals_for_empty_names) {
    std::string header;
    ruvia::append_cookie_request_pair(header, "sid", "abc");
    RUVIA_CHECK_EQ(header, std::string("sid=abc"));
    ruvia::append_cookie_request_pair(header, "", "session=forged");
    // A nameless value that looks like a cookie-pair must stay behind '='.
    RUVIA_CHECK_EQ(header, std::string("sid=abc; =session=forged"));
    std::string nameless;
    ruvia::append_cookie_request_pair(nameless, "", "sid");
    RUVIA_CHECK_EQ(nameless, std::string("=sid"));
}
