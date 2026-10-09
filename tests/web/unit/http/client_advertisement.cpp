#include <memory_resource>
#include <optional>
#include <string>

#include "ruvia/core/event_loop_attachment.h"

#include "client/http_client_advertisement_queue.h"
#include "client/http_client_config_validation.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

RUVIA_TEST(http_client_advertisements_own_results_bound_queues_and_reclaim_repeated_operations) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto& worker_value = attachment.loop().handle();
    auto run = [&]() -> ruvia::task<void> {
        ruvia::test::counting_memory_resource memory;
        std::pmr::unsynchronized_pool_resource source_memory;
        ruvia::http_origin_advertisement source_value(&source_memory);
        source_value.origins_.emplace_back("https://example.test");
        ruvia::http_alternative_service_advertisement service(&source_memory);
        service.stream_id_ = 1;
        service.field_value_.assign("h3=\":443\"; ma=86400");
        std::optional<ruvia::http_client_advertisement> retained;
        {
            ruvia::detail::http_client_advertisement_queue queue(worker_value,
                {.receive_origins_ = true, .receive_alternative_services_ = true, .max_queued_advertisements_ = 2, .max_retained_bytes_ = 2048},
                &memory, memory);
            RUVIA_CHECK(!queue.next());
            RUVIA_CHECK(queue.retain(3, ruvia::http_protocol_version::http3, source_value));
            retained = queue.next();
            RUVIA_CHECK(retained && retained->origins() && !retained->alternative_service());
            RUVIA_CHECK(retained->connection_slot() == 3 && retained->protocol_version() == ruvia::http_protocol_version::http3);
            source_value.origins_.front().assign("https://changed.test");
            RUVIA_CHECK(retained->origins()->origins_.front() == "https://example.test");
            const auto held_bytes = queue.retained_bytes();
            std::size_t warm_allocations{};
            for (unsigned repeat = 0; repeat != 128; ++repeat) {
                {
                    RUVIA_CHECK(queue.retain(1, service));
                    auto result_value = queue.next();
                    RUVIA_CHECK(result_value && result_value->alternative_service());
                    RUVIA_CHECK(result_value->alternative_service()->stream_id_ == 1);
                    RUVIA_CHECK(result_value->alternative_service()->field_value_ == service.field_value_);
                    RUVIA_CHECK(queue.retained_bytes() > held_bytes);
                }
                RUVIA_CHECK_EQ(queue.retained_bytes(), held_bytes);
                if (repeat == 16) {
                    warm_allocations = memory.live_allocations();
                } else if (repeat > 16) {
                    RUVIA_CHECK_EQ(memory.live_allocations(), warm_allocations);
                }
            }
            RUVIA_CHECK(queue.retain(0, service));
            RUVIA_CHECK(queue.retain(1, service));
            RUVIA_CHECK(!queue.retain(2, service));
            RUVIA_CHECK(queue.dropped() == 1);
            queue.clear();
            RUVIA_CHECK_EQ(queue.retained_bytes(), held_bytes);
            service.field_value_.assign(2048, 'x');
            RUVIA_CHECK(!queue.retain(0, service));
            RUVIA_CHECK(queue.dropped() == 2);
            queue.retire();
        }
        RUVIA_CHECK(memory.live_allocations() > 0);
        RUVIA_CHECK(retained->origins()->origins_.front() == "https://example.test");
        retained.reset();
        RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
        attachment.stop();
        co_return;
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

RUVIA_TEST(http_client_advertisement_budget_counts_results_held_outside_the_queue) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    auto run = [&]() -> ruvia::task<void> {
        std::pmr::unsynchronized_pool_resource pool;
        ruvia::detail::http_client_advertisement_queue queue(worker_value,
            {.receive_origins_ = true, .receive_alternative_services_ = true, .max_retained_bytes_ = 4096}, &pool);
        ruvia::http_origin_advertisement origins(&pool);
        origins.origins_.emplace_back(std::string(1000, 'a'));
        RUVIA_CHECK(queue.retain(0, ruvia::http_protocol_version::http3, origins));
        auto held = queue.next();
        RUVIA_CHECK(held.has_value() && !queue.next());
        ruvia::http_alternative_service_advertisement service(&pool);
        service.field_value_.assign(3000, 's');
        RUVIA_CHECK(!queue.retain(0, service));
        RUVIA_CHECK(queue.dropped() == 1);
        held.reset();
        RUVIA_CHECK(queue.retained_bytes() == 0);
        RUVIA_CHECK(queue.retain(0, service));
        queue.clear();
        RUVIA_CHECK(queue.retained_bytes() == 0);
        queue.retire();
        attachment.stop();
        co_return;
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

RUVIA_TEST(http_client_advertisements_disabled_queue_does_not_allocate_operation_storage) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource memory;
    {
        ruvia::detail::http_client_advertisement_queue queue(worker_value, {}, &memory, memory);
        // MSVC Debug STL may account for list proxy/sentinel metadata at
        // construction. Verify disabled queue operations allocate nothing
        // beyond that container baseline, then release it on destruction.
        const auto queue_construction_baseline = memory.live_allocations();
        auto run = [&]() -> ruvia::task<void> {
            {
                ruvia::http_origin_advertisement origins(&memory);
                const auto operation_allocation_baseline = memory.allocation_count();
                RUVIA_CHECK(!queue.retain(0, ruvia::http_protocol_version::http2, origins));
                RUVIA_CHECK(!queue.next() && queue.dropped() == 0 && queue.retained_bytes() == 0);
                queue.retire();
                RUVIA_CHECK_EQ(memory.allocation_count(), operation_allocation_baseline);
            }
            RUVIA_CHECK_EQ(memory.live_allocations(), queue_construction_baseline);
            attachment.stop();
            co_return;
        };
        auto root = attachment.loop().start(run());
        attachment.run();
        root.get();
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http_client_origin_observation_requires_authenticated_tls_and_positive_bounds) {
    for (const auto scheme : {ruvia::http_scheme::http, ruvia::http_scheme::https}) {
        ruvia::http_client_config config{.scheme_ = scheme, .host_ = "example.test"};
        config.advertisements_.receive_origins_ = true;
        config.tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification;
        bool rejected = false;
        try {
            ruvia::detail::validate_http_client_config(config);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
    ruvia::http_client_config config{.host_ = "example.test"};
    config.advertisements_.receive_origins_ = true;
    ruvia::detail::validate_http_client_config(config);
    config.advertisements_.max_queued_advertisements_ = 0;
    bool rejected = false;
    try {
        ruvia::detail::validate_http_client_config(config);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}
