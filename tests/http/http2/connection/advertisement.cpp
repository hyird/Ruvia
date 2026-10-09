#include <array>
#include <stdexcept>

#include "ruvia/http/http2_connection.h"

#include "test_harness.h"
namespace {
void transfer(ruvia::http2_connection& from, ruvia::http2_connection& to) {
    const auto bytes_value = from.pending_output();
    const auto status = to.feed(bytes_value);
    if (status != ruvia::http2_feed_result::accepted && status != ruvia::http2_feed_result::need_input) {
        throw std::runtime_error("advertisement transfer failed");
    }
    (void)from.consume_output(bytes_value.size());
}
}  // namespace
RUVIA_TEST(http2_connection_origin_and_alternative_service_events_are_owned_and_role_checked) {
    auto client = ruvia::http2_connection::client({.receive_origin_advertisements_ = true});
    auto server = ruvia::http2_connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    const std::array<std::string_view, 1> origins{"https://example.test"};
    RUVIA_CHECK(server.submit_origin_advertisement(origins) == ruvia::http2_submit_status::accepted);
    RUVIA_CHECK(server.submit_alternative_service_advertisement(0, origins[0], "h3=\":443\"") == ruvia::http2_submit_status::accepted);
    transfer(server, client);
    auto origin = client.next_event();
    auto alt = client.next_event();
    RUVIA_CHECK(origin && origin->origin_advertisement() && origin->origin_advertisement()->origins_[0] == origins[0]);
    RUVIA_CHECK(alt && alt->alternative_service_advertisement() && alt->alternative_service_advertisement()->field_value_ == "h3=\":443\"");
    RUVIA_CHECK(client.submit_origin_advertisement(origins) == ruvia::http2_submit_status::invalid_state);
    for (std::size_t i = 0; i < 16; ++i) {
        RUVIA_CHECK(server.submit_alternative_service_advertisement(0, origins[0], "clear") == ruvia::http2_submit_status::accepted);
        transfer(server, client);
        RUVIA_CHECK(client.next_event()->alternative_service_advertisement());
    }
    RUVIA_CHECK(alt->alternative_service_advertisement()->field_value_ == "h3=\":443\"");
}
RUVIA_TEST(http2_connection_ignores_origin_without_authenticated_origin_context) {
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    RUVIA_CHECK(server.submit_origin_advertisement(std::array<std::string_view, 1>{"https://example.test"}) == ruvia::http2_submit_status::accepted);
    transfer(server, client);
    RUVIA_CHECK(!client.next_event());
}
