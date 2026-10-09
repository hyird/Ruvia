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

#include "ruvia/http/HttpCache.h"
#include "ruvia/http/HttpSetCookie.h"

#include "test_harness.h"

RUVIA_TEST(set_cookie_parser_handles_deterministic_arbitrary_bytes) {
    std::uint64_t state = 0x5345'5443'4F4F'4B49ULL;
    const auto next = [&state]() {
        state ^= state << 7U;
        state ^= state >> 9U;
        return state;
    };
    const auto verifyBorrowedFields = [&](std::string_view input,
                                          const ruvia::HttpSetCookieView& parsed) {
        const auto isBorrowedInputText = [input](std::string_view value) {
            return value.empty() || input.find(value) != std::string_view::npos;
        };
        RUVIA_CHECK(isBorrowedInputText(parsed.name()));
        RUVIA_CHECK(isBorrowedInputText(parsed.value()));
        RUVIA_CHECK(isBorrowedInputText(parsed.path()));
        RUVIA_CHECK(isBorrowedInputText(parsed.domain()));
        RUVIA_CHECK(!parsed.has(static_cast<ruvia::HttpSetCookieAttribute>(0)));
        RUVIA_CHECK(!parsed.has(static_cast<ruvia::HttpSetCookieAttribute>(
            static_cast<std::uint8_t>(ruvia::HttpSetCookieAttribute::kSecure) |
            static_cast<std::uint8_t>(ruvia::HttpSetCookieAttribute::kPath))));
    };
    constexpr std::string_view cookieOctets =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%&'*+-.^_`|~";

    for (std::size_t sample = 0; sample < 4096; ++sample) {
        std::string input(static_cast<std::size_t>(next() % 1025U), '\0');
        for (auto& byte : input) {
            byte = static_cast<char>(next());
        }

        const auto parsed = ruvia::parseSetCookie(input);
        if (parsed.has_value()) {
            verifyBorrowedFields(input, *parsed);
        }

        std::string acceptedInput("sid=");
        for (std::size_t index = 0; index < next() % 129U; ++index) {
            acceptedInput.push_back(cookieOctets[next() % cookieOctets.size()]);
        }
        acceptedInput.append("; Path=/api; Domain=.example.test; Secure; SameSite=None");
        const auto accepted = ruvia::parseSetCookie(acceptedInput);
        RUVIA_CHECK(accepted.has_value());
        if (accepted.has_value()) {
            verifyBorrowedFields(acceptedInput, *accepted);
        }
    }
}

RUVIA_TEST(set_cookie_parser_exposes_client_storage_fields) {
    const auto parsed = ruvia::parseSetCookie(
        "sid=abc; Path=/api; Domain=.example.com; Max-Age=60; Secure; HttpOnly");
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK(parsed->name() == "sid");
    RUVIA_CHECK(parsed->value() == "abc");
    RUVIA_CHECK(parsed->path() == "/api");
    RUVIA_CHECK(parsed->domain() == "example.com");
    RUVIA_CHECK(parsed->maxAgeSeconds() == 60);
    RUVIA_CHECK(parsed->has(ruvia::HttpSetCookieAttribute::kSecure));
    RUVIA_CHECK(!parsed->has(static_cast<ruvia::HttpSetCookieAttribute>(
        static_cast<std::uint8_t>(ruvia::HttpSetCookieAttribute::kSecure) |
        static_cast<std::uint8_t>(ruvia::HttpSetCookieAttribute::kPath))));
}

RUVIA_TEST(set_cookie_parser_accepts_user_agent_cookie_pair_grammar) {
    const auto nameless = ruvia::parseSetCookie("sid; Path=/");
    RUVIA_CHECK(nameless.has_value());
    if (!nameless) {
        return;
    }
    RUVIA_CHECK(nameless->name().empty());
    RUVIA_CHECK(nameless->value() == "sid");

    const auto spacedName = ruvia::parseSetCookie("bad name=value");
    RUVIA_CHECK(spacedName.has_value());
    if (!spacedName) {
        return;
    }
    RUVIA_CHECK(spacedName->name() == "bad name");

    const auto spacedValue = ruvia::parseSetCookie("name=bad value");
    RUVIA_CHECK(spacedValue.has_value());
    if (!spacedValue) {
        return;
    }
    RUVIA_CHECK(spacedValue->value() == "bad value");

    const auto emptyName = ruvia::parseSetCookie("=value");
    RUVIA_CHECK(emptyName.has_value());
    if (!emptyName) {
        return;
    }
    RUVIA_CHECK(emptyName->name().empty());
    RUVIA_CHECK(emptyName->value() == "value");
}

RUVIA_TEST(set_cookie_parser_rejects_invalid_received_cookie) {
    RUVIA_CHECK(!ruvia::parseSetCookie("=").has_value());
    RUVIA_CHECK(!ruvia::parseSetCookie("name=bad\x01value").has_value());
    RUVIA_CHECK(!ruvia::parseSetCookie("name=value; Domain=bad_domain").has_value());
    RUVIA_CHECK(!ruvia::parseSetCookie("name=value; Path=/bad\tpath").has_value());

    std::string oversized(4097, 'v');
    RUVIA_CHECK(!ruvia::parseSetCookie(oversized).has_value());
}

RUVIA_TEST(set_cookie_parser_ignores_oversized_attribute_value) {
    std::string value = "sid=abc; Path=";
    value.append(1025, 'p');
    const auto parsed = ruvia::parseSetCookie(value);
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK(parsed->path().empty());
    RUVIA_CHECK(!parsed->has(ruvia::HttpSetCookieAttribute::kPath));
}

RUVIA_TEST(set_cookie_parser_tracks_storage_security_attributes) {
    const auto parsed = ruvia::parseSetCookie("sid=abc; Path=relative; SameSite=None");
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK(parsed->has(ruvia::HttpSetCookieAttribute::kPath));
    RUVIA_CHECK(parsed->path() == "relative");
    RUVIA_CHECK(parsed->has(ruvia::HttpSetCookieAttribute::kSameSiteNone));

    const auto overridden = ruvia::parseSetCookie("sid=abc; SameSite=None; SameSite=Lax");
    RUVIA_CHECK(overridden.has_value());
    if (!overridden) {
        return;
    }
    RUVIA_CHECK(!overridden->has(ruvia::HttpSetCookieAttribute::kSameSiteNone));
}

RUVIA_TEST(set_cookie_parser_preserves_quoted_cookie_value) {
    const auto parsed = ruvia::parseSetCookie("name=\"value\"; Path=/");
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK(parsed->value() == "\"value\"");
}

RUVIA_TEST(set_cookie_parser_treats_empty_domain_as_host_only) {
    const auto parsed = ruvia::parseSetCookie("sid=abc; Domain=; Path=/api");
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK(parsed->name() == "sid");
    RUVIA_CHECK(parsed->domain().empty());
    RUVIA_CHECK(parsed->path() == "/api");

    const auto overridesEarlierDomain =
        ruvia::parseSetCookie("sid=abc; Domain=example.com; Domain=");
    RUVIA_CHECK(overridesEarlierDomain.has_value());
    RUVIA_CHECK(overridesEarlierDomain->domain().empty());
}

RUVIA_TEST(set_cookie_parser_saturates_max_age_overflow) {
    const auto positive =
        ruvia::parseSetCookie("sid=abc; Max-Age=999999999999999999999999999999999999");
    RUVIA_CHECK(positive.has_value());
    RUVIA_CHECK(positive->maxAgeSeconds() ==
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::days(400)).count());

    const auto negative =
        ruvia::parseSetCookie("sid=abc; Max-Age=-999999999999999999999999999999999999");
    RUVIA_CHECK(negative.has_value());
    RUVIA_CHECK(negative->maxAgeSeconds() == std::numeric_limits<std::int64_t>::min());
}

RUVIA_TEST(set_cookie_parser_ignores_invalid_later_expires_attribute) {
    const auto parsed =
        ruvia::parseSetCookie("sid=abc; Expires=Sun, 06 Nov 1994 08:49:37 GMT; Expires=not-a-date");
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK(parsed->expires().has_value());
}

RUVIA_TEST(set_cookie_parser_uses_cookie_date_token_grammar) {
    const auto hyphenated = ruvia::parseSetCookie("sid=abc; Expires=Wed, 09-Jun-2021 10:18:14 GMT");
    RUVIA_CHECK(hyphenated.has_value());
    if (!hyphenated) {
        return;
    }
    RUVIA_CHECK(hyphenated->expires() == ruvia::parseHttpDate("Wed, 09 Jun 2021 10:18:14 GMT"));

    const auto shortYear =
        ruvia::parseSetCookie("sid=abc; Expires=Thursday, 01-Jan-70 00:00:00 GMT");
    RUVIA_CHECK(shortYear.has_value());
    if (!shortYear) {
        return;
    }
    RUVIA_CHECK(shortYear->expires() == ruvia::parseHttpDate("Thu, 01 Jan 1970 00:00:00 GMT"));
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
        const auto parsed = ruvia::parseSetCookie(input);
        RUVIA_CHECK(parsed.has_value());
        if (parsed) {
            RUVIA_CHECK(parsed->expires() == expected);
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
        const auto parsed = ruvia::parseSetCookie(input);
        RUVIA_CHECK(parsed.has_value());
        if (parsed) {
            const auto year_start = std::chrono::system_clock::to_time_t(
                std::chrono::sys_days{std::chrono::year{entry.year_} / 1 / 1});
            RUVIA_CHECK(parsed->expires() == year_start);
        }
    }

    RUVIA_CHECK(!ruvia::parseHttpDate("Wed, 09th June 2021year 10:18:14clock GMT"));
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
        const auto parsed = ruvia::parseSetCookie(input);
        RUVIA_CHECK(parsed.has_value());
        if (parsed) {
            RUVIA_CHECK(!parsed->expires());
        }
    }
}
