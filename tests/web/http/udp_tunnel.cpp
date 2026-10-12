#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/http_udp_tunnel.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace udp_tunnel_test {
struct udp_tunnel_observation {
    std::atomic<bool> eof_{};
    std::atomic<bool> read_overlap_rejected_{};
    std::atomic<bool> write_overlap_rejected_{};
    std::atomic<bool> failed_{};
};

const auto udp_observation = std::make_shared<udp_tunnel_observation>();
[[maybe_unused]] const bool udp_state_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<udp_tunnel_observation>>(
        [] { return udp_observation; });
    return true;
}();

class udp_tunnel_routes final {
    RUVIA_ROUTES_BEGIN(udp_tunnel_routes)
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/.well-known/masque/udp/target.test/53/", echo);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/.well-known/masque/udp/cancel.test/53/", delayed);
    RUVIA_GET("/udp-tunnel-sibling", sibling);
    RUVIA_ROUTES_END

    ruvia::task<void> echo(ruvia::context& c) {
        auto& observation = *c.worker_state<std::shared_ptr<udp_tunnel_observation>>();
        ruvia::http_udp_tunnel tunnel(c.tunnel().capsules());
        try {
            auto first_read = tunnel.read();
            auto last_read = tunnel.read();
            auto first_write = tunnel.send("ready");
            auto last_write = tunnel.send("second-ready");
            auto finish = tunnel.finish();
            {
                auto discarded = tunnel.send("discarded");
                static_cast<void>(discarded);
            }
            std::optional<ruvia::http_udp_datagram> datagram;
            ruvia::task_scope reads(c.worker());
            const auto receive = [&]() -> ruvia::task<void> {
                datagram = co_await std::move(first_read);
            };
            reads.spawn(receive());
            try {
                auto overlap = tunnel.read();
                static_cast<void>(co_await std::move(overlap));
            } catch (const std::logic_error&) {
                observation.read_overlap_rejected_ = true;
            }
            ruvia::task_scope writes(c.worker());
            const auto send = [&]() -> ruvia::task<void> {
                co_await std::move(first_write);
            };
            writes.spawn(send());
            try {
                auto overlap = tunnel.send("overlap");
                co_await std::move(overlap);
            } catch (const std::logic_error&) {
                observation.write_overlap_rejected_ = true;
            }
            co_await writes.join();
            co_await std::move(last_write);
            co_await reads.join();
            observation.eof_ = !(co_await std::move(last_read));
            if (datagram) {
                co_await tunnel.send(datagram->payload());
            }
            co_await std::move(finish);
        } catch (...) {
            observation.failed_ = true;
            throw;
        }
    }

    ruvia::task<void> delayed(ruvia::context& c) {
        co_await ruvia::sleep_for(c.worker(), std::chrono::milliseconds(30));
        ruvia::http_udp_tunnel tunnel(c.tunnel().capsules());
        co_await tunnel.finish();
    }

    ruvia::task<ruvia::http_response> sibling(ruvia::context& c) {
        co_return c.text("sibling");
    }
};
}  // namespace udp_tunnel_test

using udp_tunnel_test::udp_observation;

RUVIA_TEST(http1_udp_tunnel_capsules_echo_preserve_siblings_and_owned_lifetime) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    const auto& observation = udp_observation;
    observation->eof_ = false;
    observation->read_overlap_rejected_ = false;
    observation->write_overlap_rejected_ = false;
    observation->failed_ = false;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        try {
            ruvia::http_client client(
                attachment.loop(), {.scheme_ = ruvia::http_scheme::http,
                                       .host_ = "127.0.0.1",
                                       .port_ = server.endpoint().port(),
                                       .connection_count_ = 2,
                                       .request_timeout_ = std::chrono::seconds(5)});
            auto result = co_await client.handle().open_udp_tunnel(
                {.target_ = "/.well-known/masque/udp/target.test/53/"});
            RUVIA_CHECK(result.response() == nullptr && result.tunnel() != nullptr);
            RUVIA_CHECK(result.tunnel()->status() == ruvia::http_status::switching_protocols);
            RUVIA_CHECK(result.tunnel()->header("capsule-protocol") == "?1");
            RUVIA_CHECK(!result.tunnel()->header("content-length"));
            auto tunnel = std::move(*result.tunnel()).udp();
            bool oversized = false;
            try {
                co_await tunnel.send(std::string(65528, 'x'));
            } catch (const std::length_error&) {
                oversized = true;
            }
            RUVIA_CHECK(oversized);
            for (const auto expected : {std::string_view("ready"), std::string_view("second-ready")}) {
                auto ready = co_await tunnel.read();
                RUVIA_CHECK(ready.has_value());
                if (ready) {
                    RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(ready->payload().data()), ready->payload().size()), expected);
                }
            }
            std::optional<ruvia::http_udp_datagram> retained;
            const auto worker = attachment.loop().handle();
            ruvia::task_scope reader(worker);
            const auto receive = [&]() -> ruvia::task<void> {
                retained = co_await tunnel.read();
                RUVIA_CHECK(retained && std::string(reinterpret_cast<const char*>(retained->payload().data()), retained->payload().size()) == "dns-query");
                RUVIA_CHECK(!(co_await tunnel.read()));
            };
            reader.spawn(receive());
            std::string payload = "dns-query";
            auto cold_write = tunnel.send(payload);
            payload.assign("mutated");
            co_await std::move(cold_write);
            auto sibling = co_await client.handle().send({.target_ = "/udp-tunnel-sibling"});
            auto sibling_body = co_await sibling.body().read_all();
            RUVIA_CHECK(std::string_view(
                            reinterpret_cast<const char*>(sibling_body.bytes().data()), sibling_body.size()) == "sibling");
            auto late_write = tunnel.send("too-late");
            co_await tunnel.finish();
            bool ended_write_rejected = false;
            try {
                co_await std::move(late_write);
            } catch (const std::logic_error&) {
                ended_write_rejected = true;
            }
            RUVIA_CHECK(ended_write_rejected);
            co_await reader.join();
            RUVIA_CHECK(observation->eof_);
            RUVIA_CHECK(observation->read_overlap_rejected_);
            RUVIA_CHECK(observation->write_overlap_rejected_);
            RUVIA_CHECK(!observation->failed_);
            RUVIA_CHECK(retained && std::string(reinterpret_cast<const char*>(retained->payload().data()), retained->payload().size()) == "dns-query");
            auto next_sibling = co_await client.handle().send({.target_ = "/udp-tunnel-sibling"});
            auto next_sibling_body = co_await next_sibling.body().read_all();
            RUVIA_CHECK(std::string_view(
                            reinterpret_cast<const char*>(next_sibling_body.bytes().data()), next_sibling_body.size()) == "sibling");
            ruvia::stop_source cancel;
            auto pending = client.with_options({.stop_token_ = cancel.token()}).open_udp_tunnel({.target_ = "/.well-known/masque/udp/cancel.test/53/"});
            cancel.request_stop();
            bool cancelled = false;
            try {
                (void)co_await std::move(pending);
            } catch (const ruvia::http_client_error& error) {
                cancelled = error.code() == ruvia::http_client_error::code_type::cancelled;
            }
            RUVIA_CHECK(cancelled);
            co_await client.shutdown();
        } catch (...) {
            failure = std::current_exception();
        }
        server.finish();
        attachment.stop();
    };
    auto done = attachment.loop().start(run());
    attachment.run();
    done.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
