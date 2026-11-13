#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "http_client_response_fixture.h"
#include "test_exceptions.h"

using namespace ruvia::test;

// HTTP/1 client responses: reading a response head off the wire.

namespace {

constexpr std::size_t repeated_field_count = 160;

std::string repeated_response_wire(std::string_view status_line, std::string_view trailing_fields) {
    std::string wire(status_line);
    wire.append("\r\n");
    for (std::size_t index = 0; index < repeated_field_count; ++index) {
        wire.append("X-Repeat: value-");
        wire.append(std::to_string(index));
        wire.append("\r\n");
    }
    wire.append(trailing_fields);
    wire.append("\r\n");
    return wire;
}

void check_repeated_response_fields(const http_client_response_head& head, std::size_t expected_count) {
    const auto fields = head.headers();
    EXPECT_EQ(fields.size(), expected_count);
    if (fields.size() < repeated_field_count) {
        return;
    }
    for (std::size_t index = 0; index < repeated_field_count; ++index) {
        EXPECT_EQ(fields[index].name(), std::string_view("X-Repeat"));
        EXPECT_EQ(fields[index].value(), std::string("value-") + std::to_string(index).c_str());
    }
}

}  // namespace

TEST(Ruvia, http_client_upgrade_after_many_response_fields_validates_offered_protocol) {
    const ruvia::http_header_view offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket, IRC/6.9"},
    };
    const auto wire = repeated_response_wire("HTTP/1.1 101 Switching Protocols",
        "Connection: Upgrade\r\nUpgrade: irc/6.9\r\n");
    const auto accepted = parse_wire("GET", wire, http1_close_policy::allow_reuse, offered);
    EXPECT_TRUE(accepted.parsed() != nullptr);
    if (const auto* parsed = accepted.parsed()) {
        EXPECT_TRUE(parsed->head().status() == ruvia::http_status::switching_protocols);
        EXPECT_EQ(parsed->consumed_bytes(), wire.size());
        EXPECT_TRUE(parsed->plan().protocol_upgrade() != nullptr);
        EXPECT_EQ(active_plan_alternative_count(parsed->plan()), std::size_t{1});
        check_repeated_response_fields(parsed->head(), repeated_field_count + 2);
        const auto fields = parsed->head().headers();
        if (fields.size() == repeated_field_count + 2) {
            EXPECT_EQ(fields[repeated_field_count].name(), std::string_view("Connection"));
            EXPECT_EQ(fields[repeated_field_count].value(), std::string_view("Upgrade"));
            EXPECT_EQ(fields.back().name(), std::string_view("Upgrade"));
            EXPECT_EQ(fields.back().value(), std::string_view("irc/6.9"));
        }
    }
    const auto unoffered_wire = repeated_response_wire("HTTP/1.1 101 Switching Protocols",
        "Connection: Upgrade\r\nUpgrade: IRC/6.10\r\n");
    const auto unoffered = parse_wire("GET", unoffered_wire, http1_close_policy::allow_reuse, offered);
    EXPECT_TRUE(unoffered.failure() != nullptr);
    if (unoffered.failure() != nullptr) {
        EXPECT_TRUE(*unoffered.failure() == http1_client_response_parse_error::invalid_protocol_switch);
    }
    const auto unsolicited = parse_wire("GET", wire);
    EXPECT_TRUE(unsolicited.failure() != nullptr);
    if (unsolicited.failure() != nullptr) {
        EXPECT_TRUE(*unsolicited.failure() == http1_client_response_parse_error::invalid_protocol_switch);
    }
}
