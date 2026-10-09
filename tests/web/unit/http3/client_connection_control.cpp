#include "http3_client_connection_fixture.h"

RUVIA_TEST(http3_client_upload_exchange_waits_for_continue_and_preserves_producer_inactivity_and_trailers) {
    test_identity_files identity;
    local_http3_peer peer(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().handle();
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https,
                                                         .host_ = "localhost",
                                                         .port_ = peer.port(),
                                                         .connect_timeout_ = 10s,
                                                         .write_timeout_ = 100ms,
                                                         .request_timeout_ = 12s,
                                                         .protocol_ = ruvia::http_client_protocol::http3_only,
                                                         .ca_file_ = identity.certificate().string()});
        std::exception_ptr failure;
        try {
            auto exchange = co_await client.open_request({.method_ = "POST", .target_ = "/upload"},
                {.content_length_ = 6, .expectation_ = ruvia::http_client_request_expectation::continue_value, .continue_timeout_ = 200ms});
            co_await exchange.body().write("abc");
            // An application's producer pause does not consume write inactivity.
            (void)co_await ruvia::sleep_for(worker, 150ms);
            co_await exchange.body().write("def");
            const std::array<ruvia::http_header_view, 1> trailers{{{"x-end", "retained"}}};
            co_await exchange.body().end(trailers);
            RUVIA_CHECK(exchange.body().complete());
            auto response = co_await exchange.response();
            RUVIA_CHECK(response.protocol_version() == ruvia::http_protocol_version::http3);
            RUVIA_CHECK_EQ(response.status().value(), 200);
            auto body = co_await response.body().read_all(16);
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(body.bytes().data()), body.size()), "abcdef");
            RUVIA_CHECK_EQ(peer.request_payload_bytes(), std::size_t{6});
            RUVIA_CHECK(peer.upload_trailer_observed());
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
