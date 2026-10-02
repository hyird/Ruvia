#include <memory_resource>
#include <optional>
#include <string>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/web/detail/client/HttpClientAdvertisementQueue.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

RUVIA_TEST(http_client_advertisements_own_results_bound_queues_and_reclaim_repeated_operations) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto& worker = attachment.loop().handle();
    auto run = [&]() -> ruvia::Task<void> {
        ruvia::test::CountingMemoryResource memory;
        std::pmr::unsynchronized_pool_resource sourceMemory;
        ruvia::HttpOriginAdvertisement source(&sourceMemory);
        source.origins.emplace_back("https://example.test");
        ruvia::HttpAlternativeServiceAdvertisement service(&sourceMemory);
        service.streamId = 1;
        service.fieldValue.assign("h3=\":443\"; ma=86400");
        std::optional<ruvia::HttpClientAdvertisement> retained;
        {
            ruvia::detail::HttpClientAdvertisementQueue queue(worker,
                {.receiveOrigins = true, .receiveAlternativeServices = true, .maxQueuedAdvertisements = 2, .maxRetainedBytes = 2048},
                &memory, memory);
            RUVIA_CHECK(!queue.next());
            RUVIA_CHECK(queue.retain(3, ruvia::HttpProtocolVersion::kHttp3, source));
            retained = queue.next();
            RUVIA_CHECK(retained && retained->origins() && !retained->alternativeService());
            RUVIA_CHECK(retained->connectionSlot() == 3 && retained->protocolVersion() == ruvia::HttpProtocolVersion::kHttp3);
            source.origins.front().assign("https://changed.test");
            RUVIA_CHECK(retained->origins()->origins.front() == "https://example.test");
            const auto heldBytes = queue.retainedBytes();
            std::size_t warmAllocations{};
            for (unsigned repeat = 0; repeat != 128; ++repeat) {
                {
                    RUVIA_CHECK(queue.retain(1, service));
                    auto result = queue.next();
                    RUVIA_CHECK(result && result->alternativeService());
                    RUVIA_CHECK(result->alternativeService()->streamId == 1);
                    RUVIA_CHECK(result->alternativeService()->fieldValue == service.fieldValue);
                    RUVIA_CHECK(queue.retainedBytes() > heldBytes);
                }
                RUVIA_CHECK_EQ(queue.retainedBytes(), heldBytes);
                if (repeat == 16) {
                    warmAllocations = memory.liveAllocations();
                } else if (repeat > 16) {
                    RUVIA_CHECK_EQ(memory.liveAllocations(), warmAllocations);
                }
            }
            RUVIA_CHECK(queue.retain(0, service));
            RUVIA_CHECK(queue.retain(1, service));
            RUVIA_CHECK(!queue.retain(2, service));
            RUVIA_CHECK(queue.dropped() == 1);
            queue.clear();
            RUVIA_CHECK_EQ(queue.retainedBytes(), heldBytes);
            service.fieldValue.assign(2048, 'x');
            RUVIA_CHECK(!queue.retain(0, service));
            RUVIA_CHECK(queue.dropped() == 2);
            queue.retire();
        }
        RUVIA_CHECK(memory.liveAllocations() > 0);
        RUVIA_CHECK(retained->origins()->origins.front() == "https://example.test");
        retained.reset();
        RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
        attachment.stop();
        co_return;
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

RUVIA_TEST(http_client_advertisement_budget_counts_results_held_outside_the_queue) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto run = [&]() -> ruvia::Task<void> {
        std::pmr::unsynchronized_pool_resource pool;
        ruvia::detail::HttpClientAdvertisementQueue queue(worker,
            {.receiveOrigins = true, .receiveAlternativeServices = true, .maxRetainedBytes = 4096}, &pool);
        ruvia::HttpOriginAdvertisement origins(&pool);
        origins.origins.emplace_back(std::string(1000, 'a'));
        RUVIA_CHECK(queue.retain(0, ruvia::HttpProtocolVersion::kHttp3, origins));
        auto held = queue.next();
        RUVIA_CHECK(held.has_value() && !queue.next());
        ruvia::HttpAlternativeServiceAdvertisement service(&pool);
        service.fieldValue.assign(3000, 's');
        RUVIA_CHECK(!queue.retain(0, service));
        RUVIA_CHECK(queue.dropped() == 1);
        held.reset();
        RUVIA_CHECK(queue.retainedBytes() == 0);
        RUVIA_CHECK(queue.retain(0, service));
        queue.clear();
        RUVIA_CHECK(queue.retainedBytes() == 0);
        queue.retire();
        attachment.stop();
        co_return;
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

RUVIA_TEST(http_client_advertisements_disabled_queue_allocates_no_storage) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource memory;
    ruvia::detail::HttpClientAdvertisementQueue queue(worker, {}, &memory, memory);
    RUVIA_CHECK_EQ(memory.allocationCount(), std::size_t{0});
    auto run = [&]() -> ruvia::Task<void> {
        ruvia::HttpOriginAdvertisement origins(&memory);
        RUVIA_CHECK(!queue.retain(0, ruvia::HttpProtocolVersion::kHttp2, origins));
        RUVIA_CHECK(!queue.next() && queue.dropped() == 0 && queue.retainedBytes() == 0);
        queue.retire();
        RUVIA_CHECK_EQ(memory.allocationCount(), std::size_t{0});
        attachment.stop();
        co_return;
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

RUVIA_TEST(http_client_origin_observation_requires_authenticated_tls_and_positive_bounds) {
    for (const auto scheme : {ruvia::HttpScheme::kHttp, ruvia::HttpScheme::kHttps}) {
        ruvia::HttpClientConfig config{.scheme = scheme, .host = "example.test"};
        config.advertisements.receiveOrigins = true;
        config.tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification;
        bool rejected = false;
        try {
            ruvia::detail::validateHttpClientConfig(config);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
    ruvia::HttpClientConfig config{.host = "example.test"};
    config.advertisements.receiveOrigins = true;
    ruvia::detail::validateHttpClientConfig(config);
    config.advertisements.maxQueuedAdvertisements = 0;
    bool rejected = false;
    try {
        ruvia::detail::validateHttpClientConfig(config);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}
