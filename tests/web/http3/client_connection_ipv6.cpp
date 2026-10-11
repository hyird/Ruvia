#include "http3_client_connection_fixture.h"

// The peer listens on [::1] only. DNS and the UDP socket must receive the
// unbracketed literal, the request authority the bracketed uri-host with its
// port, and TLS the configured server name.
RUVIA_TEST(http3_client_reaches_an_ipv6_literal_origin) {
    test_identity_files identity;
    local_http3_peer peer(identity, false, "::1");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https,
                                                         .host_ = "::1",
                                                         .port_ = peer.port(),
                                                         .connection_count_ = 1,
                                                         .request_timeout_ = 5s,
                                                         .protocol_ = ruvia::http_client_protocol::http3_only,
                                                         .tls_ = {.ca_file_ = identity.certificate().string(), .server_name_ = "localhost"}});
        std::exception_ptr failure;
        try {
            auto response = co_await client.send({.target_ = "/authority"});
            RUVIA_CHECK_EQ(response.status().value(), 200);
            auto body = co_await response.body().read_all(64);
            const auto expected = "[::1]:" + std::to_string(peer.port()) + " ::1";
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(body.bytes().data()), body.size()), std::string_view(expected));
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    run_client_task(attachment, run());
    peer.rethrow_if_failed();
}
