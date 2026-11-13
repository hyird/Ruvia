#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_fingerprint.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/proxy_protocol.h"

namespace {

// Item 1: Stateless CORS Policy
TEST(RuviaHttpPublicApi, cors_policy_preflight_and_response_rewrite) {
    ruvia::http_cors_policy_config config;
    config.allowed_origins_.emplace_back("https://app.example.com");
    config.allowed_origin_suffixes_.emplace_back(".trusted.example");
    config.allowed_origin_suffixes_.emplace_back("https://partner.test:8443");
    config.allowed_methods_.emplace_back("GET");
    config.allowed_methods_.emplace_back("POST");
    config.allowed_headers_.emplace_back("X-Custom-Auth");
    config.allowed_headers_.emplace_back("Content-Type");
    config.expose_headers_.emplace_back("X-Request-Id");
    config.expose_headers_.emplace_back("X-RateLimit-Remaining");
    config.allow_credentials_ = true;
    config.max_age_ = std::chrono::seconds(600);

    const ruvia::http_cors_policy policy(config);
    EXPECT_TRUE(policy.allows_origin("https://app.example.com"));
    EXPECT_TRUE(policy.allows_origin("https://sub.trusted.example"));
    EXPECT_TRUE(policy.allows_origin("http://a.b.trusted.example:8080"));
    EXPECT_FALSE(policy.allows_origin("https://trusted.example"));
    EXPECT_FALSE(policy.allows_origin("https://evil-trusted.example"));
    EXPECT_TRUE(policy.allows_origin("https://partner.test:8443"));
    EXPECT_TRUE(policy.allows_origin("https://api.partner.test:8443"));
    EXPECT_FALSE(policy.allows_origin("http://partner.test:8443"));
    EXPECT_FALSE(policy.allows_origin("https://partner.test:9443"));

    const auto preflight_ok = policy.evaluate_preflight({
        .method_ = "OPTIONS",
        .origin_ = "https://sub.trusted.example",
        .access_control_request_method_ = "POST",
        .access_control_request_headers_ = "content-type, x-custom-auth",
    });
    EXPECT_TRUE(preflight_ok.accepted());

    const auto find_header = [](std::span<const ruvia::http_header> headers, std::string_view name)
        -> std::optional<std::string_view> {
        for (const auto& field : headers) {
            if (ruvia::http_ascii_equals_ignore_case(field.name(), name)) {
                return field.value();
            }
        }
        return std::nullopt;
    };

    EXPECT_EQ(find_header(preflight_ok.headers(), "Access-Control-Allow-Origin"),
        std::optional<std::string_view>("https://sub.trusted.example"));
    EXPECT_EQ(find_header(preflight_ok.headers(), "Access-Control-Allow-Credentials"),
        std::optional<std::string_view>("true"));
    EXPECT_EQ(find_header(preflight_ok.headers(), "Access-Control-Allow-Methods"),
        std::optional<std::string_view>("GET, POST"));
    EXPECT_EQ(find_header(preflight_ok.headers(), "Access-Control-Allow-Headers"),
        std::optional<std::string_view>("X-Custom-Auth, Content-Type"));
    EXPECT_EQ(find_header(preflight_ok.headers(), "Access-Control-Max-Age"),
        std::optional<std::string_view>("600"));
    EXPECT_EQ(find_header(preflight_ok.headers(), "Vary"),
        std::optional<std::string_view>("Origin, Access-Control-Request-Method"));

    // Disallowed header or method in preflight is rejected.
    const auto preflight_bad_header = policy.evaluate_preflight({
        .method_ = "OPTIONS",
        .origin_ = "https://sub.trusted.example",
        .access_control_request_method_ = "POST",
        .access_control_request_headers_ = "X-Forbidden-Header",
    });
    EXPECT_FALSE(preflight_bad_header.accepted());
    EXPECT_TRUE(preflight_bad_header.headers().empty());

    // Actual response header rewrite: strips upstream Access-Control-* headers,
    // writes policy headers, and merges Origin into Vary without duplicates.
    std::vector<ruvia::http_header> response_headers;
    response_headers.emplace_back("Content-Type", "application/json");
    response_headers.emplace_back("Access-Control-Allow-Origin", "*");
    response_headers.emplace_back("Access-Control-Allow-Methods", "DELETE");
    response_headers.emplace_back("Vary", "Accept-Encoding, Origin");

    EXPECT_TRUE(
        policy.rewrite_response_headers("https://app.example.com", response_headers));
    EXPECT_EQ(find_header(response_headers, "Access-Control-Allow-Origin"),
        std::optional<std::string_view>("https://app.example.com"));
    EXPECT_EQ(find_header(response_headers, "Access-Control-Allow-Credentials"),
        std::optional<std::string_view>("true"));
    EXPECT_EQ(find_header(response_headers, "Access-Control-Expose-Headers"),
        std::optional<std::string_view>("X-Request-Id, X-RateLimit-Remaining"));
    EXPECT_FALSE(find_header(response_headers, "Access-Control-Allow-Methods").has_value());
    EXPECT_EQ(find_header(response_headers, "Vary"),
        std::optional<std::string_view>("Accept-Encoding, Origin"));

    // Server-side apply_response_headers emits Vary tokens before rejecting disallowed Origin/method/headers.
    {
        const std::array<ruvia::http_header_view, 1> bad_origin_headers{
            ruvia::http_header_view{"Origin", "https://untrusted.example"},
        };
        const auto get_bad_origin =
            ruvia::make_parsed_http_request("GET", "/resource", bad_origin_headers, {});
        ruvia::http_response get_resp;
        EXPECT_FALSE(policy.apply_response_headers(get_bad_origin, get_resp));
        EXPECT_FALSE(get_resp.header("Access-Control-Allow-Origin").has_value());
        EXPECT_EQ(get_resp.header("Vary"), std::optional<std::string_view>("Origin"));

        const std::array<ruvia::http_header_view, 3> bad_options_headers{
            ruvia::http_header_view{"Origin", "https://untrusted.example"},
            ruvia::http_header_view{"Access-Control-Request-Method", "DELETE"},
            ruvia::http_header_view{"Access-Control-Request-Headers", "X-Forbidden"},
        };
        const auto opts_bad =
            ruvia::make_parsed_http_request("OPTIONS", "/resource", bad_options_headers, {});
        ruvia::http_response opts_resp;
        EXPECT_FALSE(policy.apply_response_headers(opts_bad, opts_resp));
        EXPECT_FALSE(opts_resp.header("Access-Control-Allow-Origin").has_value());
        EXPECT_EQ(opts_resp.header("Vary"),
            std::optional<std::string_view>("Origin, Access-Control-Request-Method"));

        ruvia::http_cors_policy reflecting_policy(ruvia::http_cors_policy_config{
            .allowed_origins_ = {"https://app.example.com"},
            .reflect_request_headers_ = true,
        });
        ruvia::http_response reflect_opts_resp;
        EXPECT_FALSE(reflecting_policy.apply_response_headers(opts_bad, reflect_opts_resp));
        EXPECT_EQ(reflect_opts_resp.header("Vary"),
            std::optional<std::string_view>(
                "Origin, Access-Control-Request-Method, Access-Control-Request-Headers"));
    }

    // Vary and Connection use non-quoted comma-token scanning (unclosed quote does not hide later tokens).
    {
        std::vector<ruvia::http_header> vary_list;
        vary_list.emplace_back("Vary", "\"unclosed, Origin");
        ruvia::merge_http_vary_token(vary_list, "Origin");
        EXPECT_EQ(vary_list.size(), 1U);
        EXPECT_EQ(vary_list[0].value(), "\"unclosed, Origin");

        const auto parsed_vary = ruvia::parse_http_vary("\"unclosed, Accept-Encoding, *");
        EXPECT_TRUE(parsed_vary.is_wildcard());

        const auto parsed_conn =
            ruvia::parse_http_connection_nominated_fields("\"unclosed, close, X-Custom-Hop");
        EXPECT_TRUE(parsed_conn.has_close());
        EXPECT_TRUE(parsed_conn.contains("x-custom-hop"));
    }
}

// Item 2: Accept-Encoding candidate order, Cache-Control no-transform, and bounded streaming transcoder
TEST(RuviaHttpPublicApi, accept_encoding_candidate_order_and_transcoder) {
    const std::array gzip_first{ruvia::http_content_coding::gzip, ruvia::http_content_coding::brotli};
    const std::array br_first{ruvia::http_content_coding::brotli, ruvia::http_content_coding::gzip};

    // Equal qvalues resolve by the caller's candidate order.
    EXPECT_EQ(ruvia::negotiate_http_content_coding("gzip, br", gzip_first),
        std::optional(ruvia::http_content_coding::gzip));
    EXPECT_EQ(ruvia::negotiate_http_content_coding("gzip, br", br_first),
        std::optional(ruvia::http_content_coding::brotli));

    // Higher qvalue wins regardless of candidate order.
    EXPECT_EQ(ruvia::negotiate_http_content_coding("gzip;q=0.5, br;q=0.9", gzip_first),
        std::optional(ruvia::http_content_coding::brotli));

    // Explicit identity weight beats lower compressed weight, while implicit identity stays fallback.
    EXPECT_EQ(ruvia::negotiate_http_content_coding("gzip;q=0.5, identity;q=0.8", gzip_first),
        std::optional(ruvia::http_content_coding::identity));
    EXPECT_EQ(ruvia::negotiate_http_content_coding("gzip;q=0.5", gzip_first),
        std::optional(ruvia::http_content_coding::gzip));
    EXPECT_EQ(ruvia::negotiate_http_content_coding("zstd", gzip_first),
        std::optional(ruvia::http_content_coding::identity));
    EXPECT_EQ(ruvia::negotiate_http_content_coding("zstd, identity;q=0", gzip_first),
        std::nullopt);
    EXPECT_EQ(ruvia::negotiate_http_content_coding("*;q=0", gzip_first),
        std::nullopt);

    // Quote-aware Cache-Control: no-transform detection.
    EXPECT_FALSE(ruvia::http_cache_control_has_no_transform(
        "public, ext=\"a, no-transform, b\", max-age=60"));
    EXPECT_TRUE(ruvia::http_cache_control_has_no_transform(
        "public, ext=\"a, b\", no-transform, max-age=60"));

    // Multi-coding streaming transcoding (gzip + br -> zstd) with native codec memory cap.
    const std::string original(4096, 'R');
    const std::array source_stack{
        ruvia::http_content_coding::gzip, ruvia::http_content_coding::brotli};
    const auto double_encoded = ruvia::encode_http_content(
        source_stack, original, {.max_encoded_bytes_ = 65536});
    ASSERT_TRUE(double_encoded.has_value());

    ruvia::http_content_transcoder transcoder(source_stack, ruvia::http_content_coding::zstd,
        {.max_decoded_bytes_ = 8192, .max_encoded_bytes_ = 8192, .max_memory_bytes_ = 8 * 1024 * 1024});
    std::string transcoded;
    for (std::size_t pos = 0; pos < double_encoded->size(); pos += 17) {
        transcoder.write(
            std::string_view(*double_encoded).substr(pos, 17), transcoded, false);
    }
    transcoder.finish(transcoded);

    const auto final_decoded = ruvia::decode_http_content(ruvia::http_content_coding::zstd,
        transcoded, {.max_decoded_bytes_ = 8192});
    ASSERT_TRUE(std::holds_alternative<std::string>(final_decoded));
    EXPECT_EQ(std::string_view(std::get<std::string>(final_decoded)), original);

    // Exhausted native codec memory cap throws std::bad_alloc.
    EXPECT_THROW(
        (void)ruvia::http_content_encoder(ruvia::http_content_coding::gzip, {.max_memory_bytes_ = 64}),
        std::bad_alloc);
}

// Item 3: Strict Range mode, Content-Range parser, and http_byte_range_slicer
TEST(RuviaHttpPublicApi, strict_byte_ranges_content_range_and_streaming_slicer) {
    // Strict ascending non-overlapping mode rejects out-of-order and overlapping ranges.
    const auto strict_ok = ruvia::resolve_http_byte_range_set(
        "bytes=0-4, 5-9, 20-29", 100, {.max_ranges_ = 32, .strict_order_ = true});
    EXPECT_FALSE(strict_ok.ignored());
    ASSERT_EQ(strict_ok.size(), 3U);
    EXPECT_EQ(strict_ok[0].offset(), 0U);
    EXPECT_EQ(strict_ok[0].length(), 5U);
    EXPECT_EQ(strict_ok[1].offset(), 5U);
    EXPECT_EQ(strict_ok[1].length(), 5U);

    const auto strict_out_of_order = ruvia::resolve_http_byte_range_set(
        "bytes=20-29, 0-4", 100, {.max_ranges_ = 32, .strict_order_ = true});
    EXPECT_TRUE(strict_out_of_order.ignored());

    const auto strict_overlapping = ruvia::resolve_http_byte_range_set(
        "bytes=0-10, 10-20", 100, {.max_ranges_ = 32, .strict_order_ = true});
    EXPECT_TRUE(strict_overlapping.ignored());

    // Configurable max_ranges > 16 works.
    std::string twenty_ranges = "bytes=";
    for (std::size_t i = 0; i < 20; ++i) {
        if (i != 0) {
            twenty_ranges.push_back(',');
        }
        twenty_ranges += std::to_string(i * 2) + "-" + std::to_string(i * 2);
    }
    const auto many = ruvia::resolve_http_byte_range_set(
        twenty_ranges, 100, {.max_ranges_ = 24, .strict_order_ = true});
    EXPECT_FALSE(many.ignored());
    EXPECT_EQ(many.size(), 20U);

    // Content-Range parser.
    const auto parsed_cr = ruvia::parse_http_content_range("bytes 10-19/100");
    ASSERT_TRUE(parsed_cr.has_value());
    EXPECT_EQ(parsed_cr->first(), 10U);
    EXPECT_EQ(parsed_cr->last(), 19U);
    EXPECT_EQ(parsed_cr->total(), 100U);
    EXPECT_EQ(parsed_cr->length(), 10U);
    EXPECT_FALSE(ruvia::parse_http_content_range("bytes 20-10/100").has_value());
    EXPECT_FALSE(ruvia::parse_http_content_range("bytes 0-100/100").has_value());
    EXPECT_FALSE(ruvia::parse_http_content_range("bytes */100").has_value());

    // Streaming slicer for multipart/byteranges (including offset-based feeding).
    constexpr std::string_view payload = "0123456789abcdefghijklmnopqrstuvwxyz";
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=2-5, 10-14, 30-35", payload.size());
    const auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, payload.size(), "text/plain", "slice_boundary");
    std::string expected;
    for (const auto& seg : plan.segments()) {
        if (seg.kind_ == ruvia::http_multipart_byte_range_plan::segment_kind::metadata) {
            expected.append(plan.metadata().substr(seg.metadata_offset_, seg.metadata_length_));
        } else {
            expected.append(payload.substr(
                static_cast<std::size_t>(seg.file_offset_),
                static_cast<std::size_t>(seg.file_length_)));
        }
    }

    ruvia::http_byte_range_slicer slicer(
        ranges, payload.size(), "text/plain", "slice_boundary");
    EXPECT_TRUE(slicer.multipart());
    EXPECT_EQ(slicer.content_length(), expected.size());

    std::string sliced;
    // Feed starting from offset 2 in two partial segments, skipping the gap 15..29!
    slicer.write(2, payload.substr(2, 13), sliced);
    EXPECT_FALSE(slicer.complete());
    slicer.write(30, payload.substr(30, 6), sliced);
    EXPECT_TRUE(slicer.complete());
    slicer.finish(sliced);
    EXPECT_EQ(sliced, expected);
}

// Item 4: Trusted Proxy & Client IP Extractor
TEST(RuviaHttpPublicApi, trusted_proxy_client_ip_extractor) {
    ruvia::http_client_ip_config config;
    config.enabled_ = true;
    config.trusted_proxies_.emplace_back("127.0.0.0/8");
    config.trusted_proxies_.emplace_back("10.0.0.0/8");
    config.trusted_proxies_.emplace_back("2001:db8:1::/48");
    config.forwarding_headers_.emplace_back("X-Forwarded-For");
    config.forwarding_headers_.emplace_back("Forwarded");
    config.forwarding_headers_.emplace_back(
        "X-Real-IP", ruvia::http_forwarded_header_kind::single_ip);

    const ruvia::http_client_ip_extractor extractor(config);

    const std::array<ruvia::http_header_view, 2> headers{{
        {"X-Forwarded-For", "198.51.100.7, ::ffff:203.0.113.9, 10.1.2.3"},
        {"X-Real-IP", "192.0.2.55"},
    }};

    // Untrusted peer ignores forwarding headers.
    const auto direct = extractor.resolve("192.0.2.1", headers);
    ASSERT_TRUE(direct.has_value());
    EXPECT_FALSE(direct->from_forwarded_header());
    EXPECT_EQ(direct->address().to_string(), "192.0.2.1");

    // Trusted IPv4-mapped IPv6 peer strips 10.1.2.3 and normalizes ::ffff:203.0.113.9 to IPv4.
    const auto proxied = extractor.resolve("::ffff:127.0.0.1", headers);
    ASSERT_TRUE(proxied.has_value());
    EXPECT_TRUE(proxied->from_forwarded_header());
    EXPECT_EQ(proxied->address().family(), ruvia::ip_address_family::ipv4);
    EXPECT_EQ(proxied->address().to_string(), "203.0.113.9");

    // RFC 7239 Forwarded fallback when X-Forwarded-For is absent.
    const std::array<ruvia::http_header_view, 1> fwd_headers{{
        {"Forwarded", "for=\"[2001:db8:2::42]:8443\";proto=https, for=\"[2001:db8:1::1]\""},
    }};
    const auto from_rfc7239 = extractor.resolve("127.0.0.1", fwd_headers);
    ASSERT_TRUE(from_rfc7239.has_value());
    EXPECT_TRUE(from_rfc7239->from_forwarded_header());
    EXPECT_EQ(from_rfc7239->address().to_string(), "2001:db8:2::42");
}

// Item 5: Hop-by-hop fields & Connection nomination set
TEST(RuviaHttpPublicApi, hop_by_hop_fields_and_connection_nomination) {
    const std::array<ruvia::http_header_view, 2> headers{{
        {"Connection", "keep-alive, X-Custom-Hop, Upgrade"},
        {"Connection", "close, X-Second-Hop"},
    }};
    const auto nominated = ruvia::parse_http_connection_nominated_fields(headers);
    EXPECT_TRUE(nominated.has_keep_alive());
    EXPECT_TRUE(nominated.has_close());
    EXPECT_TRUE(nominated.has_upgrade());
    EXPECT_TRUE(nominated.contains("x-custom-hop"));
    EXPECT_TRUE(nominated.contains("X-SECOND-HOP"));

    EXPECT_TRUE(ruvia::is_http_hop_by_hop_field_name("Proxy-Authorization"));
    EXPECT_TRUE(ruvia::is_http_hop_by_hop_field_name("Transfer-Encoding"));
    EXPECT_TRUE(ruvia::is_http_hop_by_hop_or_nominated_field("X-Custom-Hop", nominated));
    EXPECT_TRUE(ruvia::is_http_hop_by_hop_or_nominated_field("Upgrade", nominated, false));
    EXPECT_FALSE(ruvia::is_http_hop_by_hop_or_nominated_field("Upgrade", nominated, true));

    EXPECT_EQ(nominated.rebuild_connection_field(true, true),
        "Upgrade, x-custom-hop, x-second-hop");
    EXPECT_EQ(nominated.rebuild_connection_field(true, false), "Upgrade");
}

// Item 6: RFC 9111 Shared Cache Primitives
TEST(RuviaHttpPublicApi, rfc9111_shared_cache_primitives) {
    const auto cc = ruvia::parse_cache_control(
        "public, max-age=120, s-maxage=300, stale-while-revalidate=30, stale-if-error=600, must-revalidate");
    EXPECT_TRUE(cc.is_public());
    EXPECT_TRUE(cc.must_revalidate());
    EXPECT_EQ(cc.max_age(), std::optional<std::uint64_t>(120));
    EXPECT_EQ(cc.s_maxage(), std::optional<std::uint64_t>(300));
    EXPECT_EQ(cc.stale_while_revalidate(), std::optional<std::uint64_t>(30));
    EXPECT_EQ(cc.stale_if_error(), std::optional<std::uint64_t>(600));

    std::vector<ruvia::http_header> resp_headers;
    resp_headers.emplace_back("Cache-Control", "public, s-maxage=300, max-age=120, must-revalidate");
    resp_headers.emplace_back("Expires", "Wed, 21 Oct 2015 07:28:00 GMT");
    ruvia::rewrite_http_response_cache_max_age(resp_headers, 45);
    ASSERT_EQ(resp_headers.size(), 1U);
    EXPECT_EQ(resp_headers[0].name(), "Cache-Control");
    EXPECT_EQ(resp_headers[0].value(), "public, must-revalidate, max-age=45");

    // Vary normalization & matching.
    const std::array<ruvia::http_header_view, 2> vary_headers{{
        {"Vary", "Origin, Accept-Encoding"},
        {"Vary", "accept-language, ORIGIN"},
    }};
    const auto vary = ruvia::parse_http_vary(vary_headers);
    EXPECT_FALSE(vary.is_wildcard());
    ASSERT_EQ(vary.field_names().size(), 3U);
    EXPECT_EQ(vary.field_names()[0], "accept-encoding");
    EXPECT_EQ(vary.field_names()[1], "accept-language");
    EXPECT_EQ(vary.field_names()[2], "origin");

    const std::array<ruvia::http_header_view, 3> req1{{
        {"Origin", "https://example.com"},
        {"Accept-Encoding", "gzip,  br"},
        {"Accept-Language", "en-US"},
    }};
    const std::array<ruvia::http_header_view, 3> req2{{
        {"Accept-Language", "en-US"},
        {"Accept-Encoding", "gzip,\tbr"},
        {"Origin", "https://example.com"},
    }};
    EXPECT_TRUE(ruvia::http_vary_requests_match(vary, req1, req2));

    // RFC 9111 §4.2.3 Age calculation.
    const auto age = ruvia::compute_http_cache_age({
        .date_value_ = 1000,
        .age_value_ = 10,
        .request_time_ = 1005,
        .response_time_ = 1008,
        .now_ = 1020,
    });
    // apparent_age = 8, response_delay = 3, corrected_age_value = 13,
    // corrected_initial_age = 13, resident_time = 12 => 25.
    EXPECT_EQ(age, 25U);

    // 304 Not Modified sanitization & merging.
    EXPECT_TRUE(ruvia::is_http_304_forbidden_header("Content-Length"));
    EXPECT_TRUE(ruvia::is_http_304_forbidden_header("Content-Type"));
    EXPECT_FALSE(ruvia::is_http_304_forbidden_header("ETag"));
    EXPECT_FALSE(ruvia::is_http_304_forbidden_header("Cache-Control"));

    std::vector<ruvia::http_header> cached;
    cached.emplace_back("Content-Type", "text/plain");
    cached.emplace_back("Content-Length", "1024");
    cached.emplace_back("ETag", "\"v1\"");
    cached.emplace_back("Cache-Control", "max-age=60");

    const std::array<ruvia::http_header_view, 4> not_modified{{
        {"ETag", "\"v2\""},
        {"Cache-Control", "max-age=300"},
        {"Content-Length", "0"},
        {"Connection", "keep-alive"},
    }};
    ruvia::merge_http_304_response_headers(cached, not_modified);
    EXPECT_EQ(ruvia::parse_cache_control(cached).max_age(), std::optional<std::uint64_t>(300));
    bool has_text_plain = false;
    bool has_len_1024 = false;
    bool has_etag_v2 = false;
    for (const auto& field : cached) {
        if (field.name() == "Content-Type" && field.value() == "text/plain") {
            has_text_plain = true;
        }
        if (field.name() == "Content-Length" && field.value() == "1024") {
            has_len_1024 = true;
        }
        if (field.name() == "ETag" && field.value() == "\"v2\"") {
            has_etag_v2 = true;
        }
    }
    EXPECT_TRUE(has_text_plain);
    EXPECT_TRUE(has_len_1024);
    EXPECT_TRUE(has_etag_v2);
}

// Item 7: Proxy request target normalization, Max-Forwards, method semantics, and HTTP/1 wire estimation
TEST(RuviaHttpPublicApi, proxy_target_max_forwards_method_semantics_and_wire_size) {
    EXPECT_EQ(
        ruvia::normalize_http_proxy_request_target("GET", "http://example.com:8080/a/b?x=1"),
        "/a/b?x=1");
    EXPECT_EQ(ruvia::normalize_http_proxy_request_target("GET", "https://example.com"), "/");
    EXPECT_EQ(
        ruvia::normalize_http_proxy_request_target("GET", "https://example.com?foo=bar"),
        "/?foo=bar");
    EXPECT_EQ(
        ruvia::normalize_http_proxy_request_target("OPTIONS", "http://example.com"), "*");
    EXPECT_EQ(
        ruvia::normalize_http_proxy_request_target("OPTIONS", "http://example.com?query"),
        "/?query");
    EXPECT_EQ(ruvia::normalize_http_proxy_request_target("GET", "/already/origin"),
        "/already/origin");

    const std::array<ruvia::http_header_view, 1> mf_five{{{"Max-Forwards", "5"}}};
    const auto parsed_mf = ruvia::parse_http_max_forwards(mf_five);
    EXPECT_TRUE(parsed_mf.is_valid());
    EXPECT_EQ(parsed_mf.current(), std::optional<std::uint64_t>(5));
    EXPECT_EQ(parsed_mf.decremented(), std::optional<std::uint64_t>(4));
    EXPECT_EQ(parsed_mf.format_decremented(), "4");

    const auto mf_zero = ruvia::parse_http_max_forwards("0");
    EXPECT_TRUE(mf_zero.exhausted());
    EXPECT_EQ(mf_zero.decremented(), std::nullopt);

    const std::array<ruvia::http_header_view, 2> mf_dup{{
        {"Max-Forwards", "5"},
        {"Max-Forwards", "4"},
    }};
    EXPECT_TRUE(ruvia::parse_http_max_forwards(mf_dup).is_invalid());
    EXPECT_TRUE(ruvia::parse_http_max_forwards("5, 4").is_invalid());

    EXPECT_FALSE(ruvia::http_method_sends_zero_content_length("GET"));
    EXPECT_FALSE(ruvia::http_method_sends_zero_content_length("HEAD"));
    EXPECT_FALSE(ruvia::http_method_sends_zero_content_length("DELETE"));
    EXPECT_FALSE(ruvia::http_method_sends_zero_content_length("OPTIONS"));
    EXPECT_FALSE(ruvia::http_method_sends_zero_content_length("TRACE"));
    EXPECT_TRUE(ruvia::http_method_sends_zero_content_length("POST"));
    EXPECT_TRUE(ruvia::http_method_sends_zero_content_length("PUT"));

    const std::array<ruvia::http_header_view, 2> wire_headers{{
        {"Content-Type", "text/plain"},
        {"Content-Length", "5"},
    }};
    // "HTTP/1.1 200 OK\r\n" (17) + "Content-Type: text/plain\r\n" (26) +
    // "Content-Length: 5\r\n" (19) + "\r\n" (2) = 64.
    EXPECT_EQ(ruvia::estimate_http1_response_head_bytes(ruvia::http_status::ok, wire_headers),
        64U);
}

// Item 8: TLS ClientHello (JA3/JA4/PeetPrint) & HTTP/2 Akamai Fingerprint
TEST(RuviaHttpPublicApi, tls_client_hello_and_http2_akamai_fingerprints) {
    // Construct a synthetic TLS 1.3 ClientHello handshake message containing
    // GREASE values, SNI ("example.com"), ALPN ("h2", "http/1.1"),
    // supported_groups (GREASE + x25519 29 + secp256r1 23), ec_point_formats (0),
    // signature_algorithms (0x0403, 0x0804), and supported_versions (GREASE, 0x0304, 0x0303).
    std::vector<std::uint8_t> body;
    const auto push_u8 = [&](std::uint8_t v) { body.push_back(v); };
    const auto push_u16 = [&](std::uint16_t v) {
        body.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xffU));
        body.push_back(static_cast<std::uint8_t>(v & 0xffU));
    };

    push_u16(0x0303);                   // legacy_version = TLS 1.2 (771)
    body.insert(body.end(), 32, 0x42);  // random
    push_u8(0);                         // empty session_id

    // Cipher suites: GREASE (0x0a0a), TLS_AES_128_GCM_SHA256 (0x1301 = 4865),
    // TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (0xc02f = 49199)
    push_u16(6);
    push_u16(0x0a0a);
    push_u16(0x1301);
    push_u16(0xc02f);

    // Compression methods: [0]
    push_u8(1);
    push_u8(0);

    std::vector<std::uint8_t> exts;
    const auto ext_u8 = [&](std::uint8_t v) { exts.push_back(v); };
    const auto ext_u16 = [&](std::uint16_t v) {
        exts.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xffU));
        exts.push_back(static_cast<std::uint8_t>(v & 0xffU));
    };

    // GREASE extension 0x1a1a
    ext_u16(0x1a1a);
    ext_u16(0);

    // server_name (0x0000): "example.com" (11 bytes)
    ext_u16(0x0000);
    ext_u16(16);
    ext_u16(14);
    ext_u8(0);
    ext_u16(11);
    for (const char ch : std::string_view("example.com")) {
        ext_u8(static_cast<std::uint8_t>(ch));
    }

    // supported_groups (0x000a): GREASE 0x2a2a, 29 (x25519), 23 (secp256r1)
    ext_u16(0x000a);
    ext_u16(8);
    ext_u16(6);
    ext_u16(0x2a2a);
    ext_u16(29);
    ext_u16(23);

    // ec_point_formats (0x000b): [0]
    ext_u16(0x000b);
    ext_u16(2);
    ext_u8(1);
    ext_u8(0);

    // signature_algorithms (0x000d): 0x0403 (1027), 0x0804 (2052)
    ext_u16(0x000d);
    ext_u16(6);
    ext_u16(4);
    ext_u16(0x0403);
    ext_u16(0x0804);

    // ALPN (0x0010): "h2", "http/1.1"
    ext_u16(0x0010);
    ext_u16(14);
    ext_u16(12);
    ext_u8(2);
    ext_u8('h');
    ext_u8('2');
    ext_u8(8);
    for (const char ch : std::string_view("http/1.1")) {
        ext_u8(static_cast<std::uint8_t>(ch));
    }

    // supported_versions (0x002b): GREASE 0x3a3a, 0x0304 (772), 0x0303 (771)
    ext_u16(0x002b);
    ext_u16(7);
    ext_u8(6);
    ext_u16(0x3a3a);
    ext_u16(0x0304);
    ext_u16(0x0303);

    push_u16(static_cast<std::uint16_t>(exts.size()));
    body.insert(body.end(), exts.begin(), exts.end());

    std::vector<std::byte> message;
    message.push_back(std::byte{0x01});
    message.push_back(static_cast<std::byte>((body.size() >> 16U) & 0xffU));
    message.push_back(static_cast<std::byte>((body.size() >> 8U) & 0xffU));
    message.push_back(static_cast<std::byte>(body.size() & 0xffU));
    for (const auto b : body) {
        message.push_back(static_cast<std::byte>(b));
    }

    const auto parsed = ruvia::parse_tls_client_hello(message);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->legacy_version(), 0x0303U);
    EXPECT_EQ(parsed->version(), 0x0304U);
    EXPECT_EQ(parsed->server_name(), "example.com");
    EXPECT_EQ(parsed->ja3_raw(), "771,4865-49199,0-10-11-13-16-43,29-23,0");
    EXPECT_EQ(parsed->ja3().size(), 32U);

    const auto tcp_ja4 = parsed->ja4(false);
    const auto quic_ja4 = parsed->ja4(true);
    EXPECT_TRUE(tcp_ja4.starts_with("t13d0206h2_"));
    EXPECT_TRUE(quic_ja4.starts_with("q13d0206h2_"));
    EXPECT_EQ(tcp_ja4.substr(1), quic_ja4.substr(1));
    EXPECT_EQ(parsed->peetprint(),
        "772-771|4865-49199|0-10-11-13-16-43|29-23|0|h2-http/1.1|1027-2052");

    // HTTP/2 Akamai fingerprint collector.
    ruvia::http2_akamai_fingerprint_collector collector;
    ruvia::http2_frame_metadata settings_frame{};
    settings_frame.type_ = ruvia::http2_frame_type::settings;
    settings_frame.stream_id_ = 0;
    settings_frame.settings_[0] = {ruvia::http2_setting_id::header_table_size, 65536};
    settings_frame.settings_[1] = {ruvia::http2_setting_id::enable_push, 0};
    settings_frame.settings_[2] = {ruvia::http2_setting_id::initial_window_size, 6291456};
    settings_frame.settings_[3] = {ruvia::http2_setting_id::max_header_list_size, 262144};
    settings_frame.settings_count_ = 4;
    collector.observe_frame(settings_frame);

    ruvia::http2_frame_metadata wu_frame{};
    wu_frame.type_ = ruvia::http2_frame_type::window_update;
    wu_frame.stream_id_ = 0;
    wu_frame.window_increment_ = 15663105;
    collector.observe_frame(wu_frame);

    ruvia::http2_frame_metadata pri_frame{};
    pri_frame.type_ = ruvia::http2_frame_type::priority;
    pri_frame.stream_id_ = 3;
    pri_frame.priority_ = ruvia::http2_received_priority{
        .stream_id_ = 3, .stream_dependency_ = 0, .weight_ = 201, .exclusive_ = false};
    collector.observe_frame(pri_frame);

    collector.observe_header(
        {.stream_id_ = 1, .phase_ = ruvia::http2_header_observation_phase::begin});
    for (const std::string_view pseudo : {":method", ":authority", ":scheme", ":path"}) {
        collector.observe_header({
            .stream_id_ = 1,
            .phase_ = ruvia::http2_header_observation_phase::field,
            .name_ = pseudo,
            .value_ = "x",
        });
    }
    collector.observe_header({
        .stream_id_ = 1,
        .phase_ = ruvia::http2_header_observation_phase::field,
        .name_ = "user-agent",
        .value_ = "Ruvia",
    });
    collector.observe_header(
        {.stream_id_ = 1, .phase_ = ruvia::http2_header_observation_phase::end});

    EXPECT_TRUE(collector.complete());
    EXPECT_FALSE(collector.failed());
    EXPECT_EQ(collector.fingerprint(),
        "1:65536;2:0;4:6291456;6:262144|15663105|3:0:0:201|m,a,s,p");
}

TEST(RuviaHttpPublicApi, media_type_compressibility_worker_codecs_cookie_jar_etag_and_http3_fingerprint) {
    // 1. Media type compressibility allowlist
    EXPECT_TRUE(ruvia::is_http_media_type_compressible("text/html; charset=utf-8"));
    EXPECT_TRUE(ruvia::is_http_media_type_compressible("application/problem+json"));
    EXPECT_TRUE(ruvia::is_http_media_type_compressible("application/wasm"));
    EXPECT_FALSE(ruvia::is_http_media_type_compressible("application/zip"));
    EXPECT_FALSE(ruvia::is_http_media_type_compressible("image/png"));

    // 2. Reusable http_worker_codecs and http_stream_encoder_lease
    ruvia::http_worker_codecs codecs;
    {
        auto lease = codecs.acquire_stream_encoder(ruvia::http_content_coding::gzip);
        std::string out;
        lease.encoder().write("streamed payload", out, true);
        lease.encoder().finish(out);
        const auto decoded = ruvia::decode_http_content(
            ruvia::http_content_coding::gzip, out, {.max_decoded_bytes_ = 4096});
        ASSERT_TRUE(std::holds_alternative<std::string>(decoded));
        EXPECT_EQ(std::get<std::string>(decoded), "streamed payload");
    }
    {
        // Second lease reuses the reset encoder from the pool.
        auto lease = codecs.acquire_stream_encoder(ruvia::http_content_coding::gzip);
        std::string out;
        lease.encoder().write("reused encoder payload", out);
        lease.encoder().finish(out);
        const auto decoded = ruvia::decode_http_content(
            ruvia::http_content_coding::gzip, out, {.max_decoded_bytes_ = 4096});
        ASSERT_TRUE(std::holds_alternative<std::string>(decoded));
        EXPECT_EQ(std::get<std::string>(decoded), "reused encoder payload");
    }

    // 3. Sans-I/O http_cookie_jar
    const auto now = std::chrono::system_clock::now();
    ruvia::http_cookie_jar jar(
        ruvia::http_scheme::https, "sub.example.com",
        {.max_cookies_ = 8, .max_cookie_bytes_ = 1024});
    jar.add_configured_cookie("static", "1");
    EXPECT_TRUE(jar.absorb_set_cookie("/app/login", "sid=abc; Path=/app; Secure; HttpOnly", now));
    EXPECT_TRUE(jar.absorb_set_cookie("/app/login", "__Host-token=xyz; Path=/; Secure", now));
    // __Host- with Domain must be rejected.
    EXPECT_FALSE(
        jar.absorb_set_cookie("/app/login", "__Host-bad=1; Domain=example.com; Path=/; Secure", now));
    EXPECT_EQ(jar.cookie_header("/app/dashboard", true, now),
        "sid=abc; static=1; __Host-token=xyz");
    EXPECT_EQ(jar.cookie_header("/other", true, now), "static=1; __Host-token=xyz");
    // Max-Age=0 deletes the stored cookie.
    EXPECT_TRUE(jar.absorb_set_cookie("/app/logout", "sid=deleted; Path=/app; Max-Age=0", now));
    EXPECT_EQ(jar.cookie_header("/app/dashboard", true, now), "static=1; __Host-token=xyz");

    // 4. Single-value ETag validation, strong/weak comparison, and weakening
    EXPECT_TRUE(ruvia::is_valid_http_entity_tag("\"v1\""));
    EXPECT_TRUE(ruvia::is_strong_http_entity_tag("\"v1\""));
    EXPECT_FALSE(ruvia::is_weak_http_entity_tag("\"v1\""));
    EXPECT_TRUE(ruvia::is_weak_http_entity_tag("W/\"v1\""));
    EXPECT_FALSE(ruvia::is_valid_http_entity_tag("v1"));
    EXPECT_TRUE(ruvia::http_strong_etag_equals("\"v1\"", "\"v1\""));
    EXPECT_FALSE(ruvia::http_strong_etag_equals("W/\"v1\"", "\"v1\""));
    EXPECT_TRUE(ruvia::http_weak_etag_equals("W/\"v1\"", "\"v1\""));
    EXPECT_EQ(ruvia::weaken_http_etag("\"v1\""), "W/\"v1\"");
    EXPECT_EQ(ruvia::weaken_http_etag("W/\"v1\""), "W/\"v1\"");

    // 5. HTTP/3 fingerprint collector
    ruvia::http3_fingerprint_collector h3_collector;
    h3_collector.observe_frame({
        .observation_sequence_ = 0,
        .stream_id_ = 2,
        .type_ = ruvia::http3_frame_type::settings,
        .payload_length_ = 12,
    });
    h3_collector.observe_field({
        .stream_id_ = 0,
        .section_ = ruvia::http3_field_section_kind::header,
        .phase_ = ruvia::http3_field_observation_phase::begin,
    });
    for (const std::string_view pseudo : {":method", ":authority", ":scheme", ":path"}) {
        h3_collector.observe_field({
            .stream_id_ = 0,
            .section_ = ruvia::http3_field_section_kind::header,
            .phase_ = ruvia::http3_field_observation_phase::field,
            .name_ = pseudo,
            .value_ = "x",
        });
    }
    h3_collector.observe_field({
        .stream_id_ = 0,
        .section_ = ruvia::http3_field_section_kind::header,
        .phase_ = ruvia::http3_field_observation_phase::end,
    });
    EXPECT_TRUE(h3_collector.complete());
    EXPECT_FALSE(h3_collector.failed());
    EXPECT_EQ(h3_collector.fingerprint(), "4:12|0|m,a,s,p");
}

TEST(RuviaHttpPublicApi, targeted_cache_control_field_qualified_private_and_staged_transcoder) {
    // 1. RFC 9213 Targeted Cache-Control (Structured Field Dictionary & priority selection)
    const auto cdn_cc = ruvia::parse_targeted_cache_control(
        "max-age=60, max-age=300, stale-while-revalidate=45; param=1, no-store=?0, must-revalidate=?1");
    ASSERT_TRUE(cdn_cc.has_value());
    EXPECT_EQ(cdn_cc->max_age(), std::optional<std::uint64_t>(300));
    EXPECT_EQ(cdn_cc->stale_while_revalidate(), std::optional<std::uint64_t>(45));
    EXPECT_FALSE(cdn_cc->no_store());
    EXPECT_TRUE(cdn_cc->must_revalidate());

    // Invalid SF-Dictionary syntax (uppercase key or unterminated string) returns std::nullopt per RFC 9213 §2.1.
    EXPECT_FALSE(ruvia::parse_targeted_cache_control("MAX-AGE=60").has_value());
    EXPECT_FALSE(ruvia::parse_targeted_cache_control("").has_value());

    const std::array<ruvia::http_header_view, 3> response_headers{{
        {"Cache-Control", "private, no-store"},
        {"CDN-Cache-Control", "max-age=600, public"},
        {"Cloudflare-CDN-Cache-Control", "invalid=\"unterminated"},
    }};
    const std::array<std::string_view, 2> target_list{{
        "Cloudflare-CDN-Cache-Control",
        "CDN-Cache-Control",
    }};
    // Invalid first targeted header is ignored; second valid targeted header wins over Cache-Control.
    const auto selected = ruvia::resolve_targeted_cache_control(response_headers, target_list);
    EXPECT_TRUE(selected.targeted_);
    EXPECT_EQ(selected.selected_header_, "CDN-Cache-Control");
    EXPECT_TRUE(selected.control_.is_public());
    EXPECT_FALSE(selected.control_.no_store());
    EXPECT_EQ(selected.control_.max_age(), std::optional<std::uint64_t>(600));

    // 2. RFC 9111 §5.2.2.7 private="field" & §5.2.2.3 no-cache="field" field-level stripping
    const auto field_priv = ruvia::parse_cache_control(
        "public, max-age=120, private=\"Set-Cookie, X-User-Secret\", no-cache=\"X-Auth-State\"");
    EXPECT_TRUE(field_priv.is_private());
    EXPECT_FALSE(field_priv.is_unqualified_private());
    EXPECT_TRUE(field_priv.is_field_qualified_private());
    EXPECT_EQ(field_priv.private_field_count(), 2U);
    EXPECT_TRUE(field_priv.has_private_field("set-cookie"));
    EXPECT_TRUE(field_priv.has_private_field("X-USER-SECRET"));
    EXPECT_FALSE(field_priv.has_private_field("Content-Type"));

    EXPECT_TRUE(field_priv.no_cache());
    EXPECT_FALSE(field_priv.is_unqualified_no_cache());
    EXPECT_TRUE(field_priv.is_field_qualified_no_cache());
    EXPECT_EQ(field_priv.no_cache_field_count(), 1U);
    EXPECT_TRUE(field_priv.has_no_cache_field("x-auth-state"));

    std::vector<ruvia::http_header> strip_headers;
    strip_headers.emplace_back("Content-Type", "application/json");
    strip_headers.emplace_back("Set-Cookie", "sid=123");
    strip_headers.emplace_back("X-User-Secret", "top-secret");
    strip_headers.emplace_back("X-Public", "kept");
    EXPECT_TRUE(ruvia::strip_http_cache_private_headers(strip_headers, field_priv));
    ASSERT_EQ(strip_headers.size(), 2U);
    EXPECT_EQ(strip_headers[0].name(), "Content-Type");
    EXPECT_EQ(strip_headers[1].name(), "X-Public");

    // Unqualified private alongside field-qualified private forbids shared-cache storage.
    const auto unqual_priv =
        ruvia::parse_cache_control("private=\"Set-Cookie\", private");
    EXPECT_TRUE(unqual_priv.is_unqualified_private());
    EXPECT_FALSE(unqual_priv.is_field_qualified_private());
    EXPECT_FALSE(ruvia::strip_http_cache_private_headers(strip_headers, unqual_priv));

    // 3. Staged content transcoder for unknown-length (chunked) responses with [min_bytes, max_bytes]
    std::string payload;
    for (int i = 0; i < 32; ++i) {
        payload.append("chunked-response-staging-compressible-line-0123456789\n");
    }
    const auto gzipped = ruvia::encode_http_content(
        ruvia::http_content_coding::gzip, payload, {.max_encoded_bytes_ = payload.size() + 128});
    ASSERT_TRUE(gzipped.has_value());

    // Case A: Decoded size within [min_bytes, max_bytes] -> transcodes gzip to zstd.
    ruvia::http_staged_content_transcoder staged(
        ruvia::http_content_coding::gzip, ruvia::http_content_coding::zstd,
        {.min_bytes_ = 100, .max_bytes_ = 4096});
    for (std::size_t pos = 0; pos < gzipped->size(); pos += 19) {
        EXPECT_TRUE(staged.write(std::string_view(*gzipped).substr(pos, 19)));
    }
    auto res_in_range = staged.finish();
    EXPECT_TRUE(res_in_range.transformed());
    EXPECT_EQ(res_in_range.coding(), ruvia::http_content_coding::zstd);
    EXPECT_EQ(res_in_range.decoded_bytes(), payload.size());
    const auto decoded_zstd = ruvia::decode_http_content(
        ruvia::http_content_coding::zstd, res_in_range.bytes(), {.max_decoded_bytes_ = 8192});
    ASSERT_TRUE(std::holds_alternative<std::string>(decoded_zstd));
    EXPECT_EQ(std::get<std::string>(decoded_zstd), payload);

    // Case B: Decoded size < min_bytes -> falls back to original untransformed stream!
    ruvia::http_staged_content_transcoder too_small(
        ruvia::http_content_coding::gzip, ruvia::http_content_coding::zstd,
        {.min_bytes_ = payload.size() + 100, .max_bytes_ = 8192});
    EXPECT_TRUE(too_small.write(*gzipped));
    auto res_small = too_small.finish();
    EXPECT_FALSE(res_small.transformed());
    EXPECT_EQ(res_small.coding(), ruvia::http_content_coding::gzip);
    EXPECT_EQ(res_small.bytes(), *gzipped);

    // Case C: Decoded size > max_bytes -> spills into passthrough_output and falls back to original stream!
    ruvia::http_staged_content_transcoder too_large(
        ruvia::http_content_coding::gzip, ruvia::http_content_coding::zstd,
        {.min_bytes_ = 16, .max_bytes_ = 256});
    std::string passthrough_stream;
    for (std::size_t pos = 0; pos < gzipped->size(); pos += 13) {
        (void)too_large.write(std::string_view(*gzipped).substr(pos, 13), passthrough_stream);
    }
    EXPECT_TRUE(too_large.passthrough());
    auto res_large = too_large.finish();
    EXPECT_FALSE(res_large.transformed());
    EXPECT_EQ(passthrough_stream + std::string(res_large.bytes()), *gzipped);
}

}  // namespace
