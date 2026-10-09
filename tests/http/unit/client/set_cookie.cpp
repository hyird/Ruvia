#include <chrono>
#include <concepts>
#include <cstdint>
#include <ctime>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_set_cookie.h"

#include "test_harness.h"

RUVIA_TEST(set_cookie_parser_handles_deterministic_arbitrary_bytes) {
    std::uint64_t state_value = 0x5345'5443'4F4F'4B49ULL;
    const auto next_value = [&state_value]() {
        state_value ^= state_value << 7U;
        state_value ^= state_value >> 9U;
        return state_value;
    };
    const auto verify_borrowed_fields = [&](std::string_view input,
                                            const ruvia::http_set_cookie_view& parsed_value) {
        const auto is_borrowed_input_text = [input](std::string_view value) {
            return value.empty() || input.find(value) != std::string_view::npos;
        };
        RUVIA_CHECK(is_borrowed_input_text(parsed_value.name()));
        RUVIA_CHECK(is_borrowed_input_text(parsed_value.value()));
        RUVIA_CHECK(is_borrowed_input_text(parsed_value.path()));
        RUVIA_CHECK(is_borrowed_input_text(parsed_value.domain()));
        RUVIA_CHECK(!parsed_value.has(static_cast<ruvia::http_set_cookie_attribute>(0)));
        RUVIA_CHECK(!parsed_value.has(static_cast<ruvia::http_set_cookie_attribute>(
            static_cast<std::uint8_t>(ruvia::http_set_cookie_attribute::secure) |
            static_cast<std::uint8_t>(ruvia::http_set_cookie_attribute::path))));
    };
    constexpr std::string_view cookie_octets =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%&'*+-.^_`|~";

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
        RUVIA_CHECK(accepted.has_value());
        if (accepted.has_value()) {
            verify_borrowed_fields(accepted_input, *accepted);
        }
    }
}

RUVIA_TEST(set_cookie_parser_exposes_client_storage_fields) {
    const auto parsed_value = ruvia::parse_set_cookie(
        "sid=abc; Path=/api; Domain=.example.com; Max-Age=60; Secure; HttpOnly");
    RUVIA_CHECK(parsed_value.has_value());
    RUVIA_CHECK(parsed_value->name() == "sid");
    RUVIA_CHECK(parsed_value->value() == "abc");
    RUVIA_CHECK(parsed_value->path() == "/api");
    RUVIA_CHECK(parsed_value->domain() == "example.com");
    RUVIA_CHECK(parsed_value->max_age_seconds() == 60);
    RUVIA_CHECK(parsed_value->has(ruvia::http_set_cookie_attribute::secure));
    RUVIA_CHECK(!parsed_value->has(static_cast<ruvia::http_set_cookie_attribute>(
        static_cast<std::uint8_t>(ruvia::http_set_cookie_attribute::secure) |
        static_cast<std::uint8_t>(ruvia::http_set_cookie_attribute::path))));
}

RUVIA_TEST(set_cookie_parser_accepts_user_agent_cookie_pair_grammar) {
    const auto nameless = ruvia::parse_set_cookie("sid; Path=/");
    RUVIA_CHECK(nameless.has_value());
    if (!nameless) {
        return;
    }
    RUVIA_CHECK(nameless->name().empty());
    RUVIA_CHECK(nameless->value() == "sid");

    const auto spaced_name = ruvia::parse_set_cookie("bad name=value");
    RUVIA_CHECK(spaced_name.has_value());
    if (!spaced_name) {
        return;
    }
    RUVIA_CHECK(spaced_name->name() == "bad name");

    const auto spaced_value = ruvia::parse_set_cookie("name=bad value");
    RUVIA_CHECK(spaced_value.has_value());
    if (!spaced_value) {
        return;
    }
    RUVIA_CHECK(spaced_value->value() == "bad value");

    const auto empty_name = ruvia::parse_set_cookie("=value");
    RUVIA_CHECK(empty_name.has_value());
    if (!empty_name) {
        return;
    }
    RUVIA_CHECK(empty_name->name().empty());
    RUVIA_CHECK(empty_name->value() == "value");
}

RUVIA_TEST(set_cookie_parser_rejects_invalid_received_cookie) {
    RUVIA_CHECK(!ruvia::parse_set_cookie("=").has_value());
    RUVIA_CHECK(!ruvia::parse_set_cookie("name=bad\x01value").has_value());
    RUVIA_CHECK(!ruvia::parse_set_cookie("name=value; Domain=bad_domain").has_value());
    RUVIA_CHECK(!ruvia::parse_set_cookie("name=value; Path=/bad\tpath").has_value());

    std::string oversized(4097, 'v');
    RUVIA_CHECK(!ruvia::parse_set_cookie(oversized).has_value());
}

RUVIA_TEST(set_cookie_parser_ignores_oversized_attribute_value) {
    std::string value = "sid=abc; Path=";
    value.append(1025, 'p');
    const auto parsed_value = ruvia::parse_set_cookie(value);
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    RUVIA_CHECK(parsed_value->path().empty());
    RUVIA_CHECK(!parsed_value->has(ruvia::http_set_cookie_attribute::path));
}

RUVIA_TEST(set_cookie_parser_tracks_storage_security_attributes) {
    const auto parsed_value = ruvia::parse_set_cookie("sid=abc; Path=relative; SameSite=None");
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    RUVIA_CHECK(parsed_value->has(ruvia::http_set_cookie_attribute::path));
    RUVIA_CHECK(parsed_value->path() == "relative");
    RUVIA_CHECK(parsed_value->has(ruvia::http_set_cookie_attribute::same_site_none));

    const auto overridden = ruvia::parse_set_cookie("sid=abc; SameSite=None; SameSite=Lax");
    RUVIA_CHECK(overridden.has_value());
    if (!overridden) {
        return;
    }
    RUVIA_CHECK(!overridden->has(ruvia::http_set_cookie_attribute::same_site_none));
}

RUVIA_TEST(set_cookie_parser_preserves_quoted_cookie_value) {
    const auto parsed_value = ruvia::parse_set_cookie("name=\"value\"; Path=/");
    RUVIA_CHECK(parsed_value.has_value());
    RUVIA_CHECK(parsed_value->value() == "\"value\"");
}

RUVIA_TEST(set_cookie_parser_treats_empty_domain_as_host_only) {
    const auto parsed_value = ruvia::parse_set_cookie("sid=abc; Domain=; Path=/api");
    RUVIA_CHECK(parsed_value.has_value());
    RUVIA_CHECK(parsed_value->name() == "sid");
    RUVIA_CHECK(parsed_value->domain().empty());
    RUVIA_CHECK(parsed_value->path() == "/api");

    const auto overrides_earlier_domain =
        ruvia::parse_set_cookie("sid=abc; Domain=example.com; Domain=");
    RUVIA_CHECK(overrides_earlier_domain.has_value());
    RUVIA_CHECK(overrides_earlier_domain->domain().empty());
}

RUVIA_TEST(set_cookie_parser_saturates_max_age_overflow) {
    const auto positive =
        ruvia::parse_set_cookie("sid=abc; Max-Age=999999999999999999999999999999999999");
    RUVIA_CHECK(positive.has_value());
    RUVIA_CHECK(positive->max_age_seconds() ==
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::days(400)).count());

    const auto negative =
        ruvia::parse_set_cookie("sid=abc; Max-Age=-999999999999999999999999999999999999");
    RUVIA_CHECK(negative.has_value());
    RUVIA_CHECK(negative->max_age_seconds() == std::numeric_limits<std::int64_t>::min());
}

RUVIA_TEST(set_cookie_parser_ignores_invalid_later_expires_attribute) {
    const auto parsed_value =
        ruvia::parse_set_cookie("sid=abc; Expires=Sun, 06 Nov 1994 08:49:37 GMT; Expires=not-a-date");
    RUVIA_CHECK(parsed_value.has_value());
    RUVIA_CHECK(parsed_value->expires().has_value());
}

RUVIA_TEST(set_cookie_parser_uses_cookie_date_token_grammar) {
    const auto hyphenated = ruvia::parse_set_cookie("sid=abc; Expires=Wed, 09-Jun-2021 10:18:14 GMT");
    RUVIA_CHECK(hyphenated.has_value());
    if (!hyphenated) {
        return;
    }
    RUVIA_CHECK(hyphenated->expires() == ruvia::parse_http_date("Wed, 09 Jun 2021 10:18:14 GMT"));

    const auto short_year =
        ruvia::parse_set_cookie("sid=abc; Expires=Thursday, 01-Jan-70 00:00:00 GMT");
    RUVIA_CHECK(short_year.has_value());
    if (!short_year) {
        return;
    }
    RUVIA_CHECK(short_year->expires() == ruvia::parse_http_date("Thu, 01 Jan 1970 00:00:00 GMT"));
}

RUVIA_TEST(set_cookie_parser_accepts_date_token_suffixes) {
    const auto expected = std::chrono::system_clock::to_time_t(
        std::chrono::sys_days{std::chrono::year{2021} / 6 / 9} +
        std::chrono::hours{10} + std::chrono::minutes{18} + std::chrono::seconds{14});
    constexpr std::string_view dates[]{
        "Wed, 09th Jun 2021 10:18:14 GMT",
        "Wed, 09 June 2021 10:18:14 GMT",
        "Wed, 09 jUn123 2021 10:18:14 GMT",
        "Wed, 09 Jun 2021year123 10:18:14 GMT",
        "Wed, 09 Jun 2021 10:18:14clock123 GMT",
        "Wed, 09 Jun 2021 10:18:14:GMT",
        "10:18:14clock 09th June123 2021year",
        "2021year June123 09th 10:18:14clock"};
    for (const auto date : dates) {
        const auto input = std::string("sid=abc; Expires=") + std::string(date);
        const auto parsed_value = ruvia::parse_set_cookie(input);
        RUVIA_CHECK(parsed_value.has_value());
        if (parsed_value) {
            RUVIA_CHECK(parsed_value->expires() == expected);
        }
    }

    struct year_case final {
        std::string_view value_;
        int year_;
    };
    constexpr year_case years[]{
        {"70year", 1970}, {"69year", 2069}, {"00year", 2000}};
    for (const auto& entry : years) {
        const auto input = std::string("sid=abc; Expires=01 January ") + std::string(entry.value_) + " 0:0:0clock";
        const auto parsed_value = ruvia::parse_set_cookie(input);
        RUVIA_CHECK(parsed_value.has_value());
        if (parsed_value) {
            const auto year_start = std::chrono::system_clock::to_time_t(
                std::chrono::sys_days{std::chrono::year{entry.year_} / 1 / 1});
            RUVIA_CHECK(parsed_value->expires() == year_start);
        }
    }

    RUVIA_CHECK(!ruvia::parse_http_date("Wed, 09th June 2021year 10:18:14clock GMT"));
}

RUVIA_TEST(set_cookie_parser_preserves_date_token_widths_and_calendar_bounds) {
    constexpr std::string_view dates[]{
        "Wed, 009th Jun 2021 10:18:14clock",
        "Wed, 09 Jun 02021year 10:18:14clock",
        "Wed, 09 Jun 1year 10:18:14clock",
        "Wed, 09 Jun 2021 010:18:14clock",
        "Wed, 09 Jun 2021 10:018:14clock",
        "Wed, 09 Jun 2021 10:18:014clock",
        "Wed, 09 Jun 2021 10x:18:14clock",
        "Wed, 09 Jun 2021 10:18x:14clock",
        "Wed, 09 Jux 2021 10:18:14clock",
        "Wed, 32nd June 2021year 10:18:14clock",
        "Wed, 29th February 2021year 10:18:14clock",
        "Wed, 09th June 1600year 10:18:14clock",
        "Wed, 09th June 2021year 24:18:14clock",
        "Wed, 09th June 2021year 10:60:14clock",
        "Wed, 09th June 2021year 10:18:60clock",
        "32nd 09 June 2021year 10:18:14clock",
        "09th June 2021year 24:00:00clock 10:18:14clock"};
    for (const auto date : dates) {
        const auto input = std::string("sid=abc; Expires=") + std::string(date);
        const auto parsed_value = ruvia::parse_set_cookie(input);
        RUVIA_CHECK(parsed_value.has_value());
        if (parsed_value) {
            RUVIA_CHECK(!parsed_value->expires());
        }
    }
}
