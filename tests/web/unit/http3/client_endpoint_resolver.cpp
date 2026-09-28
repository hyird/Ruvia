#include <chrono>
#include <future>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/web/detail/http3/Http3QuicClientEndpointResolver.h"

#include "test_harness.h"

namespace {
class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{};
    std::size_t returns{};
    std::size_t liveBytes{};
    bool reject{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (reject) {
            throw std::bad_alloc();
        }
        void* const memory = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++allocations;
        liveBytes += bytes;
        return memory;
    }
    void do_deallocate(void* memory, std::size_t bytes, std::size_t alignment) override {
        ++returns;
        liveBytes -= bytes;
        std::pmr::new_delete_resource()->deallocate(memory, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

using Resolver = ruvia::detail::Http3QuicClientEndpointResolver;
ruvia::Task<void> observe(ruvia::Task<Resolver::Result> operation,
    std::optional<Resolver::Result>& result) {
    result.emplace(co_await std::move(operation));
    co_return;
}
}  // namespace

RUVIA_TEST(http3QuicClientEndpointResolverOwnsHostBeforeLazyDnsStarts) {
    asio::io_context io;
    asio::ip::udp::socket peer(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    CountingResource pool;
    {
        ruvia::detail::Http3QuicClientEndpointResolver resolver(io, &pool);
        std::string hostname = "localhost";
        auto cold = resolver.resolve(hostname, peer.local_endpoint().port(),
            std::chrono::steady_clock::now() + std::chrono::seconds(2));
        hostname.assign(128, 'z');
        std::optional<Resolver::Result> resolved;
        auto future = asio::co_spawn(io,
            ruvia::asAwaitable(observe(std::move(cold), resolved)), asio::use_future);
        io.run();
        future.get();
        RUVIA_CHECK(resolved.has_value());
        if (!resolved) {
            return;
        }
        RUVIA_CHECK(resolved->status ==
                    ruvia::detail::Http3QuicClientEndpointResolver::Status::kResolved);
        RUVIA_CHECK(!resolved->endpoints.empty());
        for (const auto& endpoint : resolved->endpoints) {
            RUVIA_CHECK(endpoint.port() == peer.local_endpoint().port());
            RUVIA_CHECK(endpoint.address().is_loopback());
        }
        RUVIA_CHECK(pool.allocations > pool.returns);
    }
    RUVIA_CHECK_EQ(pool.allocations, pool.returns);
    RUVIA_CHECK_EQ(pool.liveBytes, 0U);
}

RUVIA_TEST(http3QuicClientEndpointResolverUsesOneAbsoluteDeadline) {
    asio::io_context io;
    std::pmr::unsynchronized_pool_resource pool;
    ruvia::detail::Http3QuicClientEndpointResolver resolver(io, &pool);
    const auto expired = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    std::optional<Resolver::Result> result;
    auto future = asio::co_spawn(io,
        ruvia::asAwaitable(observe(resolver.resolve("localhost", 443, expired), result)),
        asio::use_future);
    io.run();
    future.get();
    RUVIA_CHECK(result && result->status ==
                              ruvia::detail::Http3QuicClientEndpointResolver::Status::kTimeout);
    RUVIA_CHECK_EQ(io.poll(), 0U);
}

RUVIA_TEST(http3QuicClientEndpointResolverInterruptDrainsCallbacksAndCanResolveAgain) {
    asio::io_context io;
    CountingResource pool;
    {
        Resolver resolver(io, &pool);
        std::optional<Resolver::Result> interrupted;
        auto future = asio::co_spawn(io,
            ruvia::asAwaitable(observe(resolver.resolve("localhost", 443,
                                           std::chrono::steady_clock::now() + std::chrono::seconds(2)),
                interrupted)),
            asio::use_future);
        RUVIA_CHECK(io.poll_one() != 0);
        resolver.interrupt();
        io.restart();
        io.run();
        future.get();
        RUVIA_CHECK(interrupted && interrupted->status == Resolver::Status::kInterrupted);
        RUVIA_CHECK_EQ(io.poll(), 0U);
        std::optional<Resolver::Result> repeated;
        auto next = asio::co_spawn(io,
            ruvia::asAwaitable(observe(resolver.resolve("127.0.0.1", 443,
                                           std::chrono::steady_clock::now() + std::chrono::seconds(2)),
                repeated)),
            asio::use_future);
        io.restart();
        io.run();
        next.get();
        RUVIA_CHECK(repeated && repeated->status == Resolver::Status::kResolved);
        RUVIA_CHECK(repeated && !repeated->endpoints.empty());
        RUVIA_CHECK_EQ(io.poll(), 0U);
    }
    RUVIA_CHECK_EQ(pool.allocations, pool.returns);
    RUVIA_CHECK_EQ(pool.liveBytes, 0U);
}

RUVIA_TEST(http3QuicClientEndpointResolverStopDrainsResolverAndTimer) {
    asio::io_context io;
    CountingResource pool;
    {
        ruvia::detail::Http3QuicClientEndpointResolver resolver(io, &pool);
        std::optional<Resolver::Result> cancelled;
        auto future = asio::co_spawn(io,
            ruvia::asAwaitable(observe(resolver.resolve("localhost", 443,
                                           std::chrono::steady_clock::now() + std::chrono::seconds(1)),
                cancelled)),
            asio::use_future);
        RUVIA_CHECK(io.poll_one() != 0);
        resolver.requestStop();
        io.restart();
        io.run();
        future.get();
        RUVIA_CHECK(cancelled && cancelled->status ==
                                     ruvia::detail::Http3QuicClientEndpointResolver::Status::kStopped);
        RUVIA_CHECK_EQ(io.poll(), 0U);
        std::optional<Resolver::Result> stopped;
        auto afterStop = asio::co_spawn(io,
            ruvia::asAwaitable(observe(resolver.resolve("localhost", 443,
                                           std::chrono::steady_clock::now() + std::chrono::seconds(1)),
                stopped)),
            asio::use_future);
        io.restart();
        io.run();
        afterStop.get();
        RUVIA_CHECK(stopped && stopped->status ==
                                   ruvia::detail::Http3QuicClientEndpointResolver::Status::kStopped);
    }
    RUVIA_CHECK_EQ(pool.allocations, pool.returns);
    RUVIA_CHECK_EQ(pool.liveBytes, 0U);
}

RUVIA_TEST(http3QuicClientEndpointResolverReturnsEachOperationMemoryAndPreservesPriorResult) {
    asio::io_context io;
    CountingResource pool;
    {
        Resolver resolver(io, &pool);
        std::optional<Resolver::Result> retained;
        auto first = asio::co_spawn(io,
            ruvia::asAwaitable(observe(resolver.resolve("localhost", 443,
                                           std::chrono::steady_clock::now() + std::chrono::seconds(3)),
                retained)),
            asio::use_future);
        io.run();
        first.get();
        RUVIA_CHECK(retained && retained->status == Resolver::Status::kResolved);
        if (!retained || retained->endpoints.empty()) {
            return;
        }
        const auto priorEndpoint = retained->endpoints.front();
        const auto* priorAddress = retained->endpoints.data();
        const auto baseline = pool.liveBytes;
        for (int i = 0; i < 12; ++i) {
            std::optional<Resolver::Result> current;
            auto future = asio::co_spawn(io,
                ruvia::asAwaitable(observe(resolver.resolve("localhost", 443,
                                               std::chrono::steady_clock::now() + std::chrono::seconds(3)),
                    current)),
                asio::use_future);
            io.restart();
            io.run();
            future.get();
            RUVIA_CHECK(current && current->status == Resolver::Status::kResolved);
            RUVIA_CHECK(pool.liveBytes >= baseline);
            current.reset();
            RUVIA_CHECK_EQ(pool.liveBytes, baseline);
            RUVIA_CHECK(retained->endpoints.data() == priorAddress &&
                        retained->endpoints.front() == priorEndpoint);
        }
    }
    RUVIA_CHECK_EQ(pool.allocations, pool.returns);
    RUVIA_CHECK_EQ(pool.liveBytes, 0U);
}

RUVIA_TEST(http3QuicClientEndpointResolverAllocationFailureReleasesAndAllowsNextResolve) {
    asio::io_context io;
    CountingResource pool;
    {
        Resolver resolver(io, &pool);
        std::optional<Resolver::Result> failed;
        auto future = asio::co_spawn(io,
            ruvia::asAwaitable(observe(resolver.resolve("localhost", 443,
                                           std::chrono::steady_clock::now() + std::chrono::seconds(3)),
                failed)),
            asio::use_future);
        pool.reject = true;
        io.run();
        pool.reject = false;
        bool outOfMemory{};
        try {
            future.get();
        } catch (const std::bad_alloc&) {
            outOfMemory = true;
        }
        RUVIA_CHECK(outOfMemory);
        RUVIA_CHECK(!failed.has_value());
        RUVIA_CHECK_EQ(pool.liveBytes, 0U);
        std::optional<Resolver::Result> recovered;
        auto retry = asio::co_spawn(io,
            ruvia::asAwaitable(observe(resolver.resolve("localhost", 443,
                                           std::chrono::steady_clock::now() + std::chrono::seconds(3)),
                recovered)),
            asio::use_future);
        io.restart();
        io.run();
        retry.get();
        RUVIA_CHECK(recovered && recovered->status == Resolver::Status::kResolved);
    }
    RUVIA_CHECK_EQ(pool.allocations, pool.returns);
    RUVIA_CHECK_EQ(pool.liveBytes, 0U);
}

RUVIA_TEST(http3QuicClientEndpointResolverColdDropReturnsOwnedHost) {
    asio::io_context io;
    CountingResource pool;
    {
        ruvia::detail::Http3QuicClientEndpointResolver resolver(io, &pool);
        {
            auto cold = resolver.resolve("rather-long-valid-domain-for-cold-drop.invalid", 443,
                std::chrono::steady_clock::now() + std::chrono::seconds(1));
            RUVIA_CHECK(pool.allocations > pool.returns);
        }
        RUVIA_CHECK_EQ(pool.allocations, pool.returns);
        RUVIA_CHECK_EQ(pool.liveBytes, 0U);
    }
    RUVIA_CHECK_EQ(pool.allocations, pool.returns);
}

RUVIA_TEST(http3QuicClientEndpointResolverRejectsInvalidInputBeforeStarting) {
    asio::io_context io;
    std::pmr::unsynchronized_pool_resource pool;
    ruvia::detail::Http3QuicClientEndpointResolver resolver(io, &pool);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)resolver.resolve("", 443, {}); }));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)resolver.resolve("localhost", 0, {}); }));
}
