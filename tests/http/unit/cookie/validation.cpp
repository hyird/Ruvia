#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_set_cookie.h"

namespace {

// True if a public cookie plan rejects the options.
bool rejects(const ruvia::cookie_options& options) {
    try {
        (void)ruvia::set_cookie_plan("sid", "value", options);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

TEST(Ruvia, cookie_public_validation_and_prefix_parsing) {
    EXPECT_TRUE(ruvia::is_valid_cookie_value("session-token"));
    EXPECT_TRUE(!ruvia::is_valid_cookie_value("invalid;value"));
    EXPECT_TRUE(ruvia::is_valid_cookie_value("\"abc\""));
    EXPECT_TRUE(ruvia::is_valid_cookie_value("\"\""));
    EXPECT_TRUE(!ruvia::is_valid_cookie_value("\"abc"));
    EXPECT_TRUE(!ruvia::is_valid_cookie_value("\"a\"b\""));
    EXPECT_TRUE(!ruvia::is_valid_cookie_value("\""));
    const ruvia::cookie_options no_attributes{.path_ = ""};
    const ruvia::set_cookie_plan quoted("sid", "\"abc\"", no_attributes);
    std::string quoted_wire(quoted.size(), '\0');
    quoted.write(quoted_wire.data());
    EXPECT_EQ(quoted_wire, "sid=\"abc\"");
    EXPECT_TRUE(ruvia::cookie_name_prefix("__secure-id") == ruvia::cookie_prefix::secure);
    EXPECT_TRUE(ruvia::cookie_name_prefix("__HOST-id") == ruvia::cookie_prefix::host);
    EXPECT_TRUE(!ruvia::cookie_name_prefix("id").has_value());
    EXPECT_TRUE(ruvia::cookie_prefix_requirements_met(ruvia::cookie_prefix::secure, true, "/a", "example.com"));
    EXPECT_TRUE(!ruvia::cookie_prefix_requirements_met(ruvia::cookie_prefix::secure, false, "/", ""));
    EXPECT_TRUE(ruvia::cookie_prefix_requirements_met(ruvia::cookie_prefix::host, true, "/", ""));
    EXPECT_TRUE(!ruvia::cookie_prefix_requirements_met(ruvia::cookie_prefix::host, true, "", ""));
    EXPECT_TRUE(!ruvia::cookie_prefix_requirements_met(ruvia::cookie_prefix::host, true, "/", "example.com"));
    EXPECT_EQ(ruvia::http_cookie_prefix_text(ruvia::cookie_prefix::host), std::string_view("__Host-"));
}

TEST(Ruvia, cookie_borrowed_text_accepts_stable_string_owners) {
    const std::string path = "/account";
    const std::string domain = "example.com";
    std::string name = "sid";
    const std::string value = "value";

    ruvia::cookie_options options;
    options.path_ = path;
    options.domain_ = domain;
    EXPECT_TRUE(!rejects(options));

    const ruvia::set_cookie_plan plan(name, value, options);
    std::string wire(plan.size(), '\0');
    plan.write(wire.data());
    name.assign("changed");
    EXPECT_EQ(wire, std::string("sid=value; Path=/account; Domain=example.com"));
    const auto parsed_value = ruvia::parse_set_cookie(wire);
    EXPECT_TRUE(parsed_value.has_value());
    if (parsed_value) {
        EXPECT_EQ(parsed_value->name(), "sid");
        EXPECT_EQ(parsed_value->path(), "/account");
        EXPECT_EQ(parsed_value->domain(), "example.com");
        EXPECT_TRUE(parsed_value->name().data() == wire.data());
        EXPECT_TRUE(parsed_value->path().data() == wire.data() + wire.find("/account"));
        EXPECT_TRUE(parsed_value->domain().data() == wire.data() + wire.find("example.com"));
    }
}

TEST(Ruvia, cookie_expires_formats_historical_dates_at_second_resolution) {
    using namespace std::chrono;
    ruvia::cookie_options options;
    options.expires_ = system_clock::time_point{seconds{-315619200}};
    EXPECT_TRUE(!rejects(options));
    const ruvia::set_cookie_plan historical("sid", "value", options);
    std::string wire(historical.size(), '\0');
    historical.write(wire.data());
    EXPECT_EQ(wire, std::string("sid=value; Path=/; Expires=Fri, 01 Jan 1960 00:00:00 GMT"));

    options.expires_ = system_clock::time_point{} - system_clock::duration{1};
    const ruvia::set_cookie_plan fractional("sid", "value", options);
    wire.resize(fractional.size());
    fractional.write(wire.data());
    EXPECT_EQ(wire, std::string("sid=value; Path=/; Expires=Wed, 31 Dec 1969 23:59:59 GMT"));
}

TEST(Ruvia, cookie_expires_rejects_dates_before_the_cookie_calendar_range) {
    using namespace std::chrono;
    constexpr auto cutoff = duration_cast<seconds>(sys_days{year{1601} / January / 1}.time_since_epoch());
    // Clocks with nanosecond int64 storage cannot represent dates this early;
    // wider calendar ranges (including MSVC's 100ns clock) exercise the edge.
    if constexpr (ceil<seconds>(system_clock::duration::min()) <= cutoff) {
        ruvia::cookie_options options;
        options.expires_ = system_clock::time_point{duration_cast<system_clock::duration>(cutoff)};
        EXPECT_TRUE(!rejects(options));
        const ruvia::set_cookie_plan first("sid", "value", options);
        std::string wire(first.size(), '\0');
        first.write(wire.data());
        EXPECT_TRUE(wire.find("01 Jan 1601") != std::string::npos);
        options.expires_ = *options.expires_ - system_clock::duration{1};
        EXPECT_TRUE(rejects(options));
        bool rejected = false;
        try {
            (void)ruvia::set_cookie_plan("sid", "value", options);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
}

TEST(Ruvia, cookie_samesite_enum_maps_to_wire_tokens) {
    ruvia::cookie_options strict;
    strict.same_site_ = ruvia::cookie_same_site::strict;
    EXPECT_TRUE(!rejects(strict));

    ruvia::cookie_options lax;
    lax.same_site_ = ruvia::cookie_same_site::lax;
    EXPECT_TRUE(!rejects(lax));

    ruvia::cookie_options none;
    none.same_site_ = ruvia::cookie_same_site::none;
    none.secure_ = ruvia::cookie_attribute_policy::emit;
    EXPECT_TRUE(!rejects(none));

    EXPECT_TRUE(!ruvia::cookie_options{}.same_site_.has_value());
}

TEST(Ruvia, cookie_samesite_none_requires_secure) {
    ruvia::cookie_options insecure_none;
    insecure_none.same_site_ = ruvia::cookie_same_site::none;
    insecure_none.secure_ = ruvia::cookie_attribute_policy::omit;
    EXPECT_TRUE(rejects(insecure_none));  // RFC 6265bis §5.5

    ruvia::cookie_options secure_none;
    secure_none.same_site_ = ruvia::cookie_same_site::none;
    secure_none.secure_ = ruvia::cookie_attribute_policy::emit;
    EXPECT_TRUE(!rejects(secure_none));
}

TEST(Ruvia, cookie_domain_requires_dns_subdomain_syntax) {
    const auto accepts_domain = [](std::string_view domain) {
        ruvia::cookie_options options;
        options.domain_ = domain;
        return !rejects(options);
    };

    EXPECT_TRUE(accepts_domain("example.com"));
    EXPECT_TRUE(accepts_domain("EXAMPLE.com"));
    EXPECT_TRUE(accepts_domain("localhost"));
    EXPECT_TRUE(accepts_domain("3.example"));
    EXPECT_TRUE(accepts_domain("xn--bcher-kva.example"));

    // Domain= is a DNS subdomain, not the generic Set-Cookie av-octet syntax
    // used by Path. Emitting any of these values produces a non-conforming
    // Set-Cookie field that user agents ignore or interpret inconsistently.
    EXPECT_TRUE(!accepts_domain(".example.com"));
    EXPECT_TRUE(!accepts_domain("example.com."));
    EXPECT_TRUE(!accepts_domain("bad domain.example"));
    EXPECT_TRUE(!accepts_domain("bad_domain.example"));
    EXPECT_TRUE(!accepts_domain("-bad.example"));
    EXPECT_TRUE(!accepts_domain("bad-.example"));
    EXPECT_TRUE(!accepts_domain("bad..example"));
    EXPECT_TRUE(!accepts_domain("caf\xc3\xa9.example"));
    EXPECT_TRUE(!accepts_domain(std::string(64, 'a') + ".example"));
}

TEST(Ruvia, cookie_validation_rejects_injection_and_bad_options) {
    const auto rejects_cookie = [](std::string_view name, std::string_view value,
                                    const ruvia::cookie_options& options) {
        try {
            (void)ruvia::set_cookie_plan(name, value, options);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    const ruvia::cookie_options clean;
    EXPECT_TRUE(!rejects_cookie("sid", "value", clean));
    EXPECT_TRUE(rejects_cookie("bad name", "value", clean));  // space -> not a token name
    EXPECT_TRUE(rejects_cookie("sid", "a;b", clean));         // ';' in value
    ruvia::cookie_options bad_path;
    bad_path.path_ = "a;b";
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_path));  // ';' in path attribute
    // Inputs the Set-Cookie parser would trim, drop, or ignore cannot be found
    // again for same-name replacement.
    for (const std::string_view path : {" /", "/ ", " "}) {
        ruvia::cookie_options spaced_path;
        spaced_path.path_ = path;
        EXPECT_TRUE(rejects_cookie("sid", "value", spaced_path));
    }
    const std::string long_path(1025, 'p');
    ruvia::cookie_options oversized_path;
    oversized_path.path_ = long_path;
    EXPECT_TRUE(rejects_cookie("sid", "value", oversized_path));
    oversized_path.path_ = std::string_view(long_path).substr(1);
    EXPECT_TRUE(!rejects_cookie("sid", "value", oversized_path));
    const std::string long_value(4093, 'v');
    EXPECT_TRUE(!rejects_cookie("sid", long_value, clean));
    EXPECT_TRUE(rejects_cookie("sidx", long_value, clean));
    ruvia::cookie_options typed_prefix;
    typed_prefix.secure_ = ruvia::cookie_attribute_policy::emit;
    typed_prefix.prefix_ = ruvia::cookie_prefix::secure;
    EXPECT_TRUE(rejects_cookie("sid", long_value, typed_prefix));
    ruvia::cookie_options bad_priority;
    bad_priority.priority_ = static_cast<ruvia::cookie_priority>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_priority));
    ruvia::cookie_options bad_same_site;
    bad_same_site.same_site_ = static_cast<ruvia::cookie_same_site>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_same_site));
    ruvia::cookie_options bad_prefix;
    bad_prefix.prefix_ = static_cast<ruvia::cookie_prefix>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_prefix));
    ruvia::cookie_options partitioned;
    partitioned.partitioned_ = ruvia::cookie_attribute_policy::emit;  // partitioned requires Secure
    EXPECT_TRUE(rejects_cookie("sid", "value", partitioned));
    ruvia::cookie_options bad_http_only;
    bad_http_only.http_only_ = static_cast<ruvia::cookie_attribute_policy>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_http_only));
    ruvia::cookie_options bad_secure;
    bad_secure.secure_ = static_cast<ruvia::cookie_attribute_policy>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_secure));
    ruvia::cookie_options bad_partitioned;
    bad_partitioned.partitioned_ = static_cast<ruvia::cookie_attribute_policy>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_partitioned));
}

TEST(Ruvia, cookie_secure_prefix_requires_secure) {
    ruvia::cookie_options secured;
    secured.prefix_ = ruvia::cookie_prefix::secure;
    secured.secure_ = ruvia::cookie_attribute_policy::emit;
    EXPECT_TRUE(!rejects(secured));

    ruvia::cookie_options insecure;
    insecure.prefix_ = ruvia::cookie_prefix::secure;
    insecure.secure_ = ruvia::cookie_attribute_policy::omit;
    EXPECT_TRUE(rejects(insecure));  // __Secure- requires Secure
}

TEST(Ruvia, cookie_host_prefix_requires_secure_root_path_no_domain) {
    // __Host- is the strictest prefix (RFC 6265bis 4.1.3.2).
    ruvia::cookie_options valid;
    valid.prefix_ = ruvia::cookie_prefix::host;
    valid.secure_ = ruvia::cookie_attribute_policy::emit;  // path defaults to "/", domain is empty
    EXPECT_TRUE(!rejects(valid));

    ruvia::cookie_options not_secure = valid;
    not_secure.secure_ = ruvia::cookie_attribute_policy::omit;
    EXPECT_TRUE(rejects(not_secure));

    ruvia::cookie_options sub_path = valid;
    sub_path.path_ = "/sub";
    EXPECT_TRUE(rejects(sub_path));  // Path must be exactly "/"

    ruvia::cookie_options with_domain = valid;
    with_domain.domain_ = "example.com";
    EXPECT_TRUE(rejects(with_domain));  // Domain must be absent
}

TEST(Ruvia, cookie_literal_prefix_name_enforces_requirements) {
    const auto rejects_with_name = [](std::string_view name, const ruvia::cookie_options& options) {
        try {
            (void)ruvia::set_cookie_plan(name, "value", options);
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };

    // RFC 6265bis user agents match these prefixes case-insensitively. Reject
    // every spelling the UA would reject instead of emitting a silently dropped
    // cookie.
    ruvia::cookie_options insecure;                            // Secure defaults to omitted
    EXPECT_TRUE(rejects_with_name("__Secure-tok", insecure));  // __Secure- requires Secure
    EXPECT_TRUE(rejects_with_name("__secure-tok", insecure));
    EXPECT_TRUE(rejects_with_name("__SeCuRe-tok", insecure));

    ruvia::cookie_options host_bad_domain;
    host_bad_domain.secure_ = ruvia::cookie_attribute_policy::emit;
    host_bad_domain.domain_ = "example.com";  // __Host- forbids Domain
    EXPECT_TRUE(rejects_with_name("__Host-sid", host_bad_domain));
    EXPECT_TRUE(rejects_with_name("__HOST-sid", host_bad_domain));
    EXPECT_TRUE(rejects_with_name("__hOsT-sid", host_bad_domain));

    // A literal-prefixed name that meets the constraints is accepted.
    ruvia::cookie_options ok_secure;
    ok_secure.secure_ = ruvia::cookie_attribute_policy::emit;
    EXPECT_TRUE(!rejects_with_name("__Secure-tok", ok_secure));
    EXPECT_TRUE(!rejects_with_name("__sEcUrE-tok", ok_secure));
    ruvia::cookie_options ok_host;
    ok_host.secure_ = ruvia::cookie_attribute_policy::emit;  // path defaults to "/", domain empty
    EXPECT_TRUE(!rejects_with_name("__Host-sid", ok_host));
    EXPECT_TRUE(!rejects_with_name("__hOsT-sid", ok_host));

    // A name that only resembles a prefix (missing the trailing '-') is unaffected.
    ruvia::cookie_options plain;
    EXPECT_TRUE(!rejects_with_name("__Secured", plain));
    EXPECT_TRUE(!rejects_with_name("__Hostname", plain));
}

TEST(Ruvia, cookie_writer_leaves_lifetime_caps_to_user_agents) {
    // RFC 6265bis' 400-day cookie-age-limit binds user agents, not servers.
    ruvia::cookie_options over_cap;
    over_cap.max_age_ = std::chrono::seconds(34560001);
    EXPECT_TRUE(!rejects(over_cap));

    ruvia::cookie_options far_expires;
    far_expires.expires_ = std::chrono::system_clock::now() + std::chrono::hours(24 * 500);
    EXPECT_TRUE(!rejects(far_expires));

    ruvia::cookie_options deletion;
    deletion.max_age_ = std::chrono::seconds(0);
    EXPECT_TRUE(!rejects(deletion));

    ruvia::cookie_options negative;
    negative.max_age_ = std::chrono::seconds(-1);
    EXPECT_TRUE(rejects(negative));
}

TEST(Ruvia, cookie_request_pair_keeps_equals_for_empty_names) {
    const auto pair = [&](std::string_view name, std::string_view value) {
        std::string wire(ruvia::cookie_request_pair_size(name, value), '\0');
        const auto* const end = ruvia::write_cookie_request_pair(wire.data(), name, value);
        EXPECT_TRUE(end == wire.data() + wire.size());
        return wire;
    };
    EXPECT_EQ(pair("sid", "abc"), std::string("sid=abc"));
    // A nameless value that looks like a cookie-pair must stay behind '='.
    EXPECT_EQ(pair("", "session=forged"), std::string("=session=forged"));
    EXPECT_EQ(pair("", "sid"), std::string("=sid"));
    EXPECT_EQ(pair("sid", ""), std::string("sid="));
    EXPECT_EQ(pair(std::string_view("\0", 1), std::string_view("\0", 1)), std::string("\0=\0", 3));
}

TEST(Ruvia, cookie_request_pair_accepts_inputs_inside_the_written_bytes) {
    // Each input is consumed before an output byte overwrites it.
    std::string buffer("valuename");
    buffer.resize(16, '#');
    const std::string_view value(buffer.data(), 5);
    const std::string_view name(buffer.data() + 5, 4);
    const auto* const end = ruvia::write_cookie_request_pair(buffer.data(), name, value);
    EXPECT_TRUE(end == buffer.data() + 10);
    EXPECT_EQ(std::string_view(buffer.data(), 10), std::string_view("name=value"));
}

TEST(Ruvia, cookie_request_pair_preserves_aliased_inputs_and_terminators) {
    std::string header = "a";
    header.reserve(16);
    const std::string_view terminator(header.data() + header.size(), 1);
    ruvia::append_cookie_request_pair(header, "b", terminator);
    EXPECT_EQ(header, std::string("a; b=\0", 6));

    std::pmr::string empty;
    const std::string_view empty_terminator(empty.data(), 1);
    ruvia::append_cookie_request_pair(empty, empty_terminator, empty_terminator);
    EXPECT_EQ(std::string_view(empty), std::string_view("\0=\0", 3));

    std::string growing = "sid=abc";
    growing.resize(growing.capacity(), 'x');
    const auto original = growing;
    const std::string_view name(growing.data(), 3);
    const std::string_view value(growing.data() + 4, growing.size() - 3);
    ruvia::append_cookie_request_pair(growing, name, value);
    auto expected = original + "; sid=" + original.substr(4);
    expected.push_back('\0');
    EXPECT_EQ(growing, expected);
}
