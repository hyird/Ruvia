#include <array>
#include <stdexcept>

#include "ruvia/http/Http2Connection.h"

#include "test_harness.h"
namespace {
void transfer(ruvia::Http2Connection& from, ruvia::Http2Connection& to) {
    const auto bytes = from.pendingOutput();
    const auto status = to.feed(bytes);
    if (status != ruvia::Http2FeedResult::kAccepted && status != ruvia::Http2FeedResult::kNeedInput) {
        throw std::runtime_error("advertisement transfer failed");
    }
    (void)from.consumeOutput(bytes.size());
}
}  // namespace
RUVIA_TEST(http2_connection_origin_and_alternative_service_events_are_owned_and_role_checked) {
    auto client = ruvia::Http2Connection::client({.receiveOriginAdvertisements = true});
    auto server = ruvia::Http2Connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    const std::array<std::string_view, 1> origins{"https://example.test"};
    RUVIA_CHECK(server.submitOriginAdvertisement(origins) == ruvia::Http2SubmitStatus::kAccepted);
    RUVIA_CHECK(server.submitAlternativeServiceAdvertisement(0, origins[0], "h3=\":443\"") == ruvia::Http2SubmitStatus::kAccepted);
    transfer(server, client);
    auto origin = client.nextEvent();
    auto alt = client.nextEvent();
    RUVIA_CHECK(origin && origin->originAdvertisement() && origin->originAdvertisement()->origins[0] == origins[0]);
    RUVIA_CHECK(alt && alt->alternativeServiceAdvertisement() && alt->alternativeServiceAdvertisement()->fieldValue == "h3=\":443\"");
    RUVIA_CHECK(client.submitOriginAdvertisement(origins) == ruvia::Http2SubmitStatus::kInvalidState);
    for (std::size_t i = 0; i < 16; ++i) {
        RUVIA_CHECK(server.submitAlternativeServiceAdvertisement(0, origins[0], "clear") == ruvia::Http2SubmitStatus::kAccepted);
        transfer(server, client);
        RUVIA_CHECK(client.nextEvent()->alternativeServiceAdvertisement());
    }
    RUVIA_CHECK(alt->alternativeServiceAdvertisement()->fieldValue == "h3=\":443\"");
}
RUVIA_TEST(http2_connection_ignores_origin_without_authenticated_origin_context) {
    auto client = ruvia::Http2Connection::client();
    auto server = ruvia::Http2Connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    RUVIA_CHECK(server.submitOriginAdvertisement(std::array<std::string_view, 1>{"https://example.test"}) == ruvia::Http2SubmitStatus::kAccepted);
    transfer(server, client);
    RUVIA_CHECK(!client.nextEvent());
}
