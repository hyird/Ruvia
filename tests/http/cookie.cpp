#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

#include "http_cookie.h"
#include "http_cookie_jar.h"
#include "http_date.h"

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
    const auto rejects_cookie = [](std::string_view name, std::string_view value, const ruvia::cookie_options& options) {
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
    ruvia::cookie_options negative_max_age;
    negative_max_age.max_age_ = std::chrono::seconds{-1};
    EXPECT_TRUE(rejects_cookie("sid", "value", negative_max_age));
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

    std::string empty;
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

TEST(Ruvia, cookie_request_pair_preserves_contents_when_allocations_are_rejected) {
    {
        std::string header;
        header.reserve(1024);
        const std::string value(300, 'v');
        const auto expected = std::string("\0=", 2) + value;
        try {
            ruvia::append_cookie_request_pair(header, std::string_view(header.data(), 1), value);
        } catch (const std::bad_alloc&) {
            ASSERT_TRUE(header.empty());
            ruvia::append_cookie_request_pair(header, std::string_view(header.data(), 1), value);
        }
        EXPECT_EQ(std::string_view(header), std::string_view(expected));
    }
}

TEST(Ruvia, cookie_request_pair_snapshots_only_overlapping_inputs) {
    for (const bool alias_name : {false, true}) {
        for (const auto aliased_size : {3U, 300U}) {
            SCOPED_TRACE(alias_name);
            SCOPED_TRACE(aliased_size);
            const std::string aliased(aliased_size, alias_name ? 'n' : 'v');
            const std::string independent(300, alias_name ? 'v' : 'n');
            const auto expected = alias_name ? aliased + "=" + independent : independent + "=" + aliased;
            std::string output = aliased;
            output.resize(expected.size());
            const std::string_view borrowed(output.data(), aliased.size());

            (void)ruvia::write_cookie_request_pair(output.data(), alias_name ? borrowed : independent, alias_name ? independent : borrowed);

            EXPECT_EQ(output, expected);
        }
    }
}

TEST(Ruvia, cookie_request_pair_preserves_both_long_aliased_inputs_on_failure_and_retry) {
    const std::string name(300, 'n');
    const std::string value(300, 'v');
    std::string output = value + name;
    output.resize(name.size() + 1 + value.size());
    const auto original = output;
    const std::string_view borrowed_name(output.data() + value.size(), name.size());
    const std::string_view borrowed_value(output.data(), value.size());

    try {
        (void)ruvia::write_cookie_request_pair(output.data(), borrowed_name, borrowed_value);
    } catch (const std::bad_alloc&) {
        ASSERT_EQ(output, original);
        (void)ruvia::write_cookie_request_pair(output.data(), borrowed_name, borrowed_value);
    }

    EXPECT_EQ(output, name + "=" + value);
}

TEST(Ruvia, cookie_plan_serialization_emits_fields_without_attributes) {
    const ruvia::cookie_options options{.path_ = ""};
    const ruvia::set_cookie_plan plan("sid", "value", options);
    std::string wire(plan.size(), '\0');
    plan.write(wire.data());
    EXPECT_EQ(wire, "sid=value");
    const auto parsed_value = ruvia::parse_set_cookie(wire);
    EXPECT_TRUE(parsed_value.has_value());
    if (parsed_value) {
        EXPECT_EQ(parsed_value->name(), "sid");
        EXPECT_TRUE(parsed_value->name().data() == wire.data());
        EXPECT_TRUE(parsed_value->path().empty());
        EXPECT_TRUE(parsed_value->domain().empty());
    }
}

TEST(Ruvia, cookie_plan_serialization_formats_exact_max_age_widths_and_prefixed_fields) {
    constexpr std::int64_t ages[]{1, 9, 10, 99, 100, 999, 1000, ruvia::max_cookie_age_seconds};
    for (const auto age : ages) {
        const ruvia::cookie_options options{.path_ = "/", .max_age_ = std::chrono::seconds{age}, .prefix_ = ruvia::cookie_prefix::host, .secure_ = ruvia::cookie_attribute_policy::emit};
        const ruvia::set_cookie_plan plan("sid", "value", options);
        std::string wire(plan.size(), '\0');
        plan.write(wire.data());
        EXPECT_EQ(wire, std::string("__Host-sid=value; Path=/; Max-Age=") + std::to_string(age).c_str() + "; Secure");
        const auto parsed_value = ruvia::parse_set_cookie(wire);
        EXPECT_TRUE(parsed_value.has_value());
        if (parsed_value) {
            EXPECT_EQ(parsed_value->name(), "__Host-sid");
            EXPECT_EQ(parsed_value->path(), "/");
            EXPECT_TRUE(parsed_value->domain().empty());
            EXPECT_TRUE(parsed_value->name().data() == wire.data());
            EXPECT_TRUE(parsed_value->path().data() == wire.data() + wire.find('/'));
        }
    }
}

TEST(Ruvia, cookie_plan_serialization_writes_zero_max_age_as_deletion_expires) {
    // max-age-av = "Max-Age=" non-zero-digit *DIGIT (RFC 6265 section 4.1.1).
    const ruvia::cookie_options options{.path_ = "/", .expires_ = std::chrono::system_clock::time_point{std::chrono::hours{24 * 365 * 40}}, .max_age_ = std::chrono::seconds{0}};
    const ruvia::set_cookie_plan plan("sid", "", options);
    std::string wire(plan.size(), '\0');
    plan.write(wire.data());
    EXPECT_EQ(wire, "sid=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
}

TEST(RuviaHttpPublicApi, cookie_jar_scopes_prefixes_and_deletes_by_max_age) {
    const auto now = std::chrono::system_clock::now();
    ruvia::http_cookie_jar jar(ruvia::http_scheme::https, "sub.example.com", {.max_cookies_ = 8, .max_cookie_bytes_ = 1024});
    jar.add_configured_cookie("static", "1");
    EXPECT_TRUE(jar.absorb_set_cookie("/app/login", "sid=abc; Path=/app; Secure; HttpOnly", now));
    EXPECT_TRUE(jar.absorb_set_cookie("/app/login", "__Host-token=xyz; Path=/; Secure", now));
    // __Host- with Domain must be rejected.
    EXPECT_FALSE(jar.absorb_set_cookie("/app/login", "__Host-bad=1; Domain=example.com; Path=/; Secure", now));
    EXPECT_EQ(jar.cookie_header("/app/dashboard", true, now), "sid=abc; static=1; __Host-token=xyz");
    EXPECT_EQ(jar.cookie_header("/other", true, now), "static=1; __Host-token=xyz");
    // Max-Age=0 deletes the stored cookie.
    EXPECT_TRUE(jar.absorb_set_cookie("/app/logout", "sid=deleted; Path=/app; Max-Age=0", now));
    EXPECT_EQ(jar.cookie_header("/app/dashboard", true, now), "static=1; __Host-token=xyz");
}

TEST(Ruvia, set_cookie_parser_handles_deterministic_arbitrary_bytes) {
    std::uint64_t state_value = 0x5345'5443'4F4F'4B49ULL;
    const auto next_value = [&state_value]() {
        state_value ^= state_value << 7U;
        state_value ^= state_value >> 9U;
        return state_value;
    };
    const auto verify_borrowed_fields = [&](std::string_view input, const ruvia::http_set_cookie_view& parsed_value) {
        const auto is_borrowed_input_text = [input](std::string_view value) {
            return value.empty() || input.find(value) != std::string_view::npos;
        };
        EXPECT_TRUE(is_borrowed_input_text(parsed_value.name()));
        EXPECT_TRUE(is_borrowed_input_text(parsed_value.value()));
        EXPECT_TRUE(is_borrowed_input_text(parsed_value.path()));
        EXPECT_TRUE(is_borrowed_input_text(parsed_value.domain()));
        EXPECT_TRUE(!parsed_value.has(static_cast<ruvia::http_set_cookie_attribute>(0)));
        EXPECT_TRUE(!parsed_value.has(static_cast<ruvia::http_set_cookie_attribute>(static_cast<std::uint8_t>(ruvia::http_set_cookie_attribute::secure) | static_cast<std::uint8_t>(ruvia::http_set_cookie_attribute::partitioned))));
    };
    constexpr std::string_view cookie_octets = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%&'*+-.^_`|~";

    for (std::size_t sample = 0; sample < 4096; ++sample) {
        std::string input(static_cast<std::size_t>(next_value() % 1025U), '\0');
        for (auto& byte : input) {
            byte = static_cast<char>(next_value());
        }

        const auto parsed_value = ruvia::parse_set_cookie(input);
        if (parsed_value.has_value()) {
            verify_borrowed_fields(input, *parsed_value);
        }

        std::string accepted_input("sid=");
        for (std::size_t index = 0; index < next_value() % 129U; ++index) {
            accepted_input.push_back(cookie_octets[next_value() % cookie_octets.size()]);
        }
        accepted_input.append("; Path=/api; Domain=.example.test; Secure; SameSite=None");
        const auto accepted = ruvia::parse_set_cookie(accepted_input);
        EXPECT_TRUE(accepted.has_value());
        if (accepted.has_value()) {
            verify_borrowed_fields(accepted_input, *accepted);
        }
    }
}

TEST(Ruvia, set_cookie_parser_exposes_client_storage_fields) {
    const auto parsed_value = ruvia::parse_set_cookie("sid=abc; Path=/api; Domain=.example.com; Max-Age=60; Secure; HttpOnly");
    EXPECT_TRUE(parsed_value.has_value());
    EXPECT_TRUE(parsed_value->name() == "sid");
    EXPECT_TRUE(parsed_value->value() == "abc");
    EXPECT_TRUE(parsed_value->path() == "/api");
    EXPECT_TRUE(parsed_value->domain() == "example.com");
    EXPECT_TRUE(parsed_value->max_age_seconds() == 60);
    EXPECT_TRUE(parsed_value->has(ruvia::http_set_cookie_attribute::secure));
    EXPECT_TRUE(parsed_value->has(ruvia::http_set_cookie_attribute::http_only));
    EXPECT_TRUE(parsed_value->http_only());
    EXPECT_TRUE(!parsed_value->same_site().has_value());
    EXPECT_TRUE(!parsed_value->same_site_none());
    EXPECT_TRUE(!parsed_value->has(static_cast<ruvia::http_set_cookie_attribute>(static_cast<std::uint8_t>(ruvia::http_set_cookie_attribute::secure) | static_cast<std::uint8_t>(ruvia::http_set_cookie_attribute::partitioned))));
}

TEST(Ruvia, set_cookie_parser_accepts_user_agent_cookie_pair_grammar) {
    const auto spaced_name = ruvia::parse_set_cookie("bad name=value");
    EXPECT_TRUE(spaced_name.has_value());
    if (!spaced_name) {
        return;
    }
    EXPECT_TRUE(spaced_name->name() == "bad name");

    const auto spaced_value = ruvia::parse_set_cookie("name=bad value");
    EXPECT_TRUE(spaced_value.has_value());
    if (!spaced_value) {
        return;
    }
    EXPECT_TRUE(spaced_value->value() == "bad value");
}

TEST(Ruvia, set_cookie_parser_ignores_nameless_cookies) {
    // RFC 6265 section 5.2 steps 2 and 5.
    EXPECT_TRUE(!ruvia::parse_set_cookie("sid; Path=/").has_value());
    EXPECT_TRUE(!ruvia::parse_set_cookie("=value").has_value());
    EXPECT_TRUE(!ruvia::parse_set_cookie(" =value").has_value());
}

TEST(Ruvia, set_cookie_parser_rejects_invalid_received_cookie) {
    EXPECT_TRUE(!ruvia::parse_set_cookie("=").has_value());
    EXPECT_TRUE(!ruvia::parse_set_cookie("name=bad\x01value").has_value());

    std::string oversized(4097, 'v');
    EXPECT_TRUE(!ruvia::parse_set_cookie(oversized).has_value());
}

TEST(Ruvia, set_cookie_parser_follows_user_agent_domain_and_path_algorithm) {
    // RFC 6265 section 5.2.3: "Domain=." leaves an empty cookie-domain (host-only);
    // an empty Domain value is ignored rather than overriding an earlier one.
    const auto dot = ruvia::parse_set_cookie("a=b; Domain=.");
    EXPECT_TRUE(dot.has_value() && dot->domain().empty());
    const auto later_empty = ruvia::parse_set_cookie("a=b; Domain=example.com; Domain=");
    EXPECT_TRUE(later_empty.has_value() && later_empty->domain() == "example.com");
    const auto later_dot = ruvia::parse_set_cookie("a=b; Domain=example.com; Domain=.");
    EXPECT_TRUE(later_dot.has_value() && later_dot->domain().empty());
    const auto underscore = ruvia::parse_set_cookie("a=b; Domain=my_host.example");
    EXPECT_TRUE(underscore.has_value() && underscore->domain() == "my_host.example");

    // Section 5.2.4: Path never rejects the cookie.
    const auto tab = ruvia::parse_set_cookie("a=b; Path=/a\tb");
    EXPECT_TRUE(tab.has_value() && tab->path() == "/a\tb");
    const auto utf8 = ruvia::parse_set_cookie("a=b; Path=/caf\xc3\xa9");
    EXPECT_TRUE(utf8.has_value() && utf8->path() == "/caf\xc3\xa9");
    const auto relative = ruvia::parse_set_cookie("a=b; Path=/api; Path=relative");
    EXPECT_TRUE(relative.has_value() && relative->path().empty());
}

TEST(Ruvia, set_cookie_parser_ignores_oversized_attribute_value) {
    std::string value = "sid=abc; Path=";
    value.append(1025, 'p');
    const auto parsed_value = ruvia::parse_set_cookie(value);
    EXPECT_TRUE(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    EXPECT_TRUE(parsed_value->path().empty());
}

TEST(Ruvia, set_cookie_parser_tracks_storage_security_attributes) {
    const auto parsed_value = ruvia::parse_set_cookie("sid=abc; Path=/rel; SameSite=None");
    EXPECT_TRUE(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    EXPECT_TRUE(parsed_value->path() == "/rel");
    EXPECT_TRUE(parsed_value->same_site_none());

    const auto overridden = ruvia::parse_set_cookie("sid=abc; SameSite=None; SameSite=Lax");
    EXPECT_TRUE(overridden.has_value());
    if (!overridden) {
        return;
    }
    EXPECT_TRUE(parsed_value->same_site_none());
    EXPECT_TRUE(parsed_value->same_site() == ruvia::cookie_same_site::none);
    EXPECT_TRUE(!overridden->same_site_none());
    EXPECT_TRUE(overridden->same_site() == ruvia::cookie_same_site::lax);
    const auto strict = ruvia::parse_set_cookie("sid=abc; samesite=STRICT");
    EXPECT_TRUE(strict.has_value() && strict->same_site() == ruvia::cookie_same_site::strict);
    const auto unknown_same_site = ruvia::parse_set_cookie("sid=abc; SameSite=Strict; SameSite=Unknown");
    EXPECT_TRUE(unknown_same_site.has_value() && !unknown_same_site->same_site().has_value() && !unknown_same_site->same_site_none());
}

TEST(Ruvia, set_cookie_parser_preserves_quoted_cookie_value) {
    const auto parsed_value = ruvia::parse_set_cookie("name=\"value\"; Path=/");
    EXPECT_TRUE(parsed_value.has_value());
    EXPECT_TRUE(parsed_value->value() == "\"value\"");
}

TEST(Ruvia, set_cookie_parser_treats_empty_domain_as_host_only) {
    const auto parsed_value = ruvia::parse_set_cookie("sid=abc; Domain=; Path=/api");
    EXPECT_TRUE(parsed_value.has_value());
    EXPECT_TRUE(parsed_value->name() == "sid");
    EXPECT_TRUE(parsed_value->domain().empty());
    EXPECT_TRUE(parsed_value->path() == "/api");

    const std::string overlong_with_nul = std::string("sid=abc; Custom=") + std::string(1100, 'a') + std::string("\0b", 2);
    EXPECT_TRUE(!ruvia::parse_set_cookie(overlong_with_nul).has_value());
}

TEST(Ruvia, set_cookie_parser_saturates_max_age_overflow) {
    const auto positive = ruvia::parse_set_cookie("sid=abc; Max-Age=999999999999999999999999999999999999");
    EXPECT_TRUE(positive.has_value());
    EXPECT_TRUE(positive->max_age_seconds() == std::chrono::duration_cast<std::chrono::seconds>(std::chrono::days(400)).count());

    const auto negative = ruvia::parse_set_cookie("sid=abc; Max-Age=-999999999999999999999999999999999999");
    EXPECT_TRUE(negative.has_value());
    EXPECT_TRUE(negative->max_age_seconds() == std::numeric_limits<std::int64_t>::min());
}

TEST(Ruvia, set_cookie_parser_ignores_overflowing_max_age_with_trailing_bytes) {
    const auto parsed_value = ruvia::parse_set_cookie("sid=abc; Max-Age=60; Max-Age=999999999999999999999999suffix; Path=/api");
    ASSERT_TRUE(parsed_value.has_value());
    EXPECT_EQ(parsed_value->max_age_seconds(), 60);
    EXPECT_EQ(parsed_value->path(), "/api");
}

TEST(Ruvia, set_cookie_parser_ignores_invalid_later_expires_attribute) {
    const auto parsed_value = ruvia::parse_set_cookie("sid=abc; Expires=Sun, 06 Nov 1994 08:49:37 GMT; Expires=not-a-date");
    EXPECT_TRUE(parsed_value.has_value());
    EXPECT_TRUE(parsed_value->expires().has_value());
}

TEST(Ruvia, set_cookie_parser_uses_cookie_date_token_grammar) {
    const auto hyphenated = ruvia::parse_set_cookie("sid=abc; Expires=Wed, 09-Jun-2021 10:18:14 GMT");
    EXPECT_TRUE(hyphenated.has_value());
    if (!hyphenated) {
        return;
    }
    EXPECT_TRUE(hyphenated->expires() == ruvia::parse_http_date("Wed, 09 Jun 2021 10:18:14 GMT"));

    const auto short_year = ruvia::parse_set_cookie("sid=abc; Expires=Thursday, 01-Jan-70 00:00:00 GMT");
    EXPECT_TRUE(short_year.has_value());
    if (!short_year) {
        return;
    }
    EXPECT_TRUE(short_year->expires() == ruvia::parse_http_date("Thu, 01 Jan 1970 00:00:00 GMT"));
}

TEST(Ruvia, set_cookie_parser_accepts_date_token_suffixes) {
    const auto expected = std::chrono::system_clock::to_time_t(std::chrono::sys_days{std::chrono::year{2021} / 6 / 9} + std::chrono::hours{10} + std::chrono::minutes{18} + std::chrono::seconds{14});
    constexpr std::string_view dates[]{"Wed, 09th Jun 2021 10:18:14 GMT", "Wed, 09 June 2021 10:18:14 GMT", "Wed, 09 jUn123 2021 10:18:14 GMT", "Wed, 09 Jun 2021year123 10:18:14 GMT", "Wed, 09 Jun 2021 10:18:14clock123 GMT", "Wed, 09 Jun 2021 10:18:14:GMT", "10:18:14clock 09th June123 2021year", "2021year June123 09th 10:18:14clock"};
    for (const auto date : dates) {
        const auto input = std::string("sid=abc; Expires=") + std::string(date);
        const auto parsed_value = ruvia::parse_set_cookie(input);
        EXPECT_TRUE(parsed_value.has_value());
        if (parsed_value) {
            EXPECT_TRUE(parsed_value->expires() == expected);
        }
    }

    struct year_case final {
        std::string_view value_;
        int year_;
    };
    constexpr year_case years[]{{"70year", 1970}, {"69year", 2069}, {"00year", 2000}};
    for (const auto& entry : years) {
        const auto input = std::string("sid=abc; Expires=01 January ") + std::string(entry.value_) + " 0:0:0clock";
        const auto parsed_value = ruvia::parse_set_cookie(input);
        EXPECT_TRUE(parsed_value.has_value());
        if (parsed_value) {
            const auto year_start = std::chrono::system_clock::to_time_t(std::chrono::sys_days{std::chrono::year{entry.year_} / 1 / 1});
            EXPECT_TRUE(parsed_value->expires() == year_start);
        }
    }

    EXPECT_TRUE(!ruvia::parse_http_date("Wed, 09th June 2021year 10:18:14clock GMT"));
}

TEST(Ruvia, set_cookie_parser_preserves_date_token_widths_and_calendar_bounds) {
    constexpr std::string_view dates[]{"Wed, 009th Jun 2021 10:18:14clock", "Wed, 09 Jun 02021year 10:18:14clock", "Wed, 09 Jun 1year 10:18:14clock", "Wed, 09 Jun 2021 010:18:14clock", "Wed, 09 Jun 2021 10:018:14clock", "Wed, 09 Jun 2021 10:18:014clock", "Wed, 09 Jun 2021 10x:18:14clock", "Wed, 09 Jun 2021 10:18x:14clock", "Wed, 09 Jux 2021 10:18:14clock", "Wed, 32nd June 2021year 10:18:14clock", "Wed, 29th February 2021year 10:18:14clock", "Wed, 09th June 1600year 10:18:14clock", "Wed, 09th June 2021year 24:18:14clock", "Wed, 09th June 2021year 10:60:14clock", "Wed, 09th June 2021year 10:18:60clock", "32nd 09 June 2021year 10:18:14clock", "09th June 2021year 24:00:00clock 10:18:14clock"};
    for (const auto date : dates) {
        const auto input = std::string("sid=abc; Expires=") + std::string(date);
        const auto parsed_value = ruvia::parse_set_cookie(input);
        EXPECT_TRUE(parsed_value.has_value());
        if (parsed_value) {
            EXPECT_TRUE(!parsed_value->expires());
        }
    }
}

TEST(Ruvia, cookie_jar_deduplicates_and_expires_by_partitioned_four_tuple) {
    const auto now = std::chrono::system_clock::from_time_t(1'700'000'000);
    ruvia::http_cookie_jar jar(ruvia::http_scheme::https, "example.test");
    EXPECT_TRUE(jar.absorb_set_cookie("/", "sid=part1; Path=/; Secure; Partitioned", now));
    EXPECT_TRUE(jar.absorb_set_cookie("/", "sid=unpart1; Path=/; Secure", now));
    EXPECT_EQ(jar.size(), std::size_t{2});

    // Updating the unpartitioned cookie leaves the partitioned entry intact.
    EXPECT_TRUE(jar.absorb_set_cookie("/", "sid=unpart2; Path=/; Secure", now));
    EXPECT_EQ(jar.size(), std::size_t{2});
    const auto both = jar.cookie_header("/", true, now);
    EXPECT_NE(both.find("sid=part1"), std::string::npos);
    EXPECT_NE(both.find("sid=unpart2"), std::string::npos);

    // Expiring the partitioned cookie removes only the partitioned entry.
    EXPECT_TRUE(jar.absorb_set_cookie("/", "sid=; Path=/; Max-Age=0; Secure; Partitioned", now));
    EXPECT_EQ(jar.size(), std::size_t{1});
    EXPECT_EQ(jar.cookie_header("/", true, now), "sid=unpart2");
}
