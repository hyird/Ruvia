#include <barrier>
#include <stdexcept>
#include <thread>

#include <asio/io_context.hpp>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/event_loop_pool.h"
#include "ruvia/web/db/db_client.h"

#include "backend_client_fixture.h"
#include "test_harness.h"

namespace {

ruvia::task<void> connect_until_stopped(ruvia::db_client& client, bool& cancelled) {
    try {
        co_await client.connect();
    } catch (const ruvia::db_error& error) {
        if (error.code() != ruvia::db_error::code_type::closing) {
            throw;
        }
        cancelled = true;
    }
}

ruvia::task<bool> can_create_operation(ruvia::db_client& client) {
    try {
        // A cold operation exercises the public connected-client contract
        // without requiring the startup peer to implement query execution.
        auto operation = client.query("SELECT 1");
        (void)operation;
        co_return true;
    } catch (const std::logic_error&) {
        co_return false;
    }
}

}  // namespace

RUVIA_TEST(db_client_rejects_duplicate_connect_without_interrupting_startup) {
    ruvia::test::backend_startup_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto loop = pool.loop(0);
    ruvia::db_client client(loop, peer.config());
    pool.start();
    auto first = loop.start(client.connect());
    peer.wait_for_startup();
    auto duplicate = loop.start(client.connect());
    duplicate.wait();
    peer.authenticate();
    first.wait();
    auto ready = loop.start(can_create_operation(client));
    ready.wait();
    auto closing = loop.start(client.shutdown());
    closing.wait();
    pool.join();
    first.get();
    closing.get();
    bool rejected = false;
    try {
        duplicate.get();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(ready.get());
}

RUVIA_TEST(db_client_rejects_duplicate_connect_without_closing_connected_client) {
    ruvia::test::backend_startup_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto loop = pool.loop(0);
    ruvia::db_client client(loop, peer.config());
    pool.start();
    auto first = loop.start(client.connect());
    peer.wait_for_startup();
    peer.authenticate();
    first.wait();
    auto duplicate = loop.start(client.connect());
    duplicate.wait();
    auto ready = loop.start(can_create_operation(client));
    ready.wait();
    auto closing = loop.start(client.shutdown());
    closing.wait();
    pool.join();
    first.get();
    closing.get();
    bool rejected = false;
    try {
        duplicate.get();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(ready.get());
}

RUVIA_TEST(db_client_fresh_close_and_loop_stop_share_worker_completion) {
    for (int iteration = 0; iteration < 64; ++iteration) {
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::db_client client(pool.loop(0), {.driver_ = ruvia::db_driver::postgresql});
        pool.start();
        std::barrier rendezvous(2);
        std::thread closer([&] {
            rendezvous.arrive_and_wait();
            client.close();
        });
        rendezvous.arrive_and_wait();
        pool.stop();
        closer.join();
        pool.join();
        RUVIA_CHECK(!client.worker().accepting());
    }
}

RUVIA_TEST(db_client_cold_connect_can_be_discarded_before_pool_start) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    {
        ruvia::db_client client(pool.loop(0), {.driver_ = ruvia::db_driver::postgresql});
        auto cold = client.connect();
        (void)cold;
    }
    pool.join();
    RUVIA_CHECK(!pool.loop(0).accepting());
}

RUVIA_TEST(db_client_event_loop_stop_awaits_retirement_of_pending_authentication) {
    for (const bool use_attachment_run : {false, true}) {
        ruvia::test::backend_startup_peer peer;
        asio::io_context io;
        auto attachment = ruvia::attach_event_loop(io);
        ruvia::db_client client(attachment.loop(), peer.config());
        bool cancelled = false;
        auto root = attachment.loop().start(connect_until_stopped(client, cancelled));
        std::thread driver([&] {
            if (use_attachment_run) {
                attachment.run();
            } else {
                io.run();
            }
        });
        std::exception_ptr peer_failure;
        try {
            peer.wait_for_startup();
        } catch (...) {
            peer_failure = std::current_exception();
        }
        attachment.stop();
        driver.join();
        root.get();
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        RUVIA_CHECK(cancelled);
        RUVIA_CHECK(!client.worker().accepting());
    }
}

RUVIA_TEST(db_client_shutdown_joins_pending_authentication) {
    ruvia::test::backend_startup_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto loop = pool.loop(0);
    ruvia::db_client client(loop, peer.config());
    pool.start();
    auto first = loop.start(client.connect());
    peer.wait_for_startup();
    auto closing = loop.start(client.shutdown());
    first.wait();
    closing.wait();
    pool.join();
    bool cancelled = false;
    try {
        first.get();
    } catch (const ruvia::db_error& error) {
        if (error.code() != ruvia::db_error::code_type::closing) {
            throw;
        }
        cancelled = true;
    }
    closing.get();
    RUVIA_CHECK(cancelled);
}
