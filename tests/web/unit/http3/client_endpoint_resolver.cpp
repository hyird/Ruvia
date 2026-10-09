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

#include "ruvia/core/asio_task.h"

#include "http3/http3_quic_client_endpoint_resolver.h"
#include "test_harness.h"

namespace {
class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{};
    std::size_t returns_{};
    std::size_t live_bytes_{};
    bool reject_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_ && bytes_value >= 32) {
            throw std::bad_alloc();
        }
        void* const memory = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++allocations_;
        live_bytes_ += bytes_value;
        return memory;
    }
    void do_deallocate(void* memory, std::size_t bytes_value, std::size_t alignment) override {
        ++returns_;
        live_bytes_ -= bytes_value;
        std::pmr::new_delete_resource()->deallocate(memory, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

using resolver_type = ruvia::detail::http3_quic_client_endpoint_resolver;
ruvia::task<void> observe(ruvia::task<resolver_type::result_type> operation,
    std::optional<resolver_type::result_type>& result_value) {
    result_value.emplace(co_await std::move(operation));
    co_return;
}
}  // namespace

RUVIA_TEST(http3_quic_client_endpoint_resolver_owns_host_before_lazy_dns_starts) {
    asio::io_context io;
    asio::ip::udp::socket peer(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    counting_resource pool;
    {
        ruvia::detail::http3_quic_client_endpoint_resolver resolver(io, &pool);
        std::string hostname = "localhost";
        auto cold = resolver.resolve(hostname, peer.local_endpoint().port(),
            std::chrono::steady_clock::now() + std::chrono::seconds(2));
        hostname.assign(128, 'z');
        std::optional<resolver_type::result_type> resolved;
        auto future = asio::co_spawn(io,
            ruvia::as_awaitable(observe(std::move(cold), resolved)), asio::use_future);
        io.run();
        future.get();
        RUVIA_CHECK(resolved.has_value());
        if (!resolved) {
            return;
        }
        RUVIA_CHECK(resolved->status_ ==
                    ruvia::detail::http3_quic_client_endpoint_resolver::status_type::resolved);
        RUVIA_CHECK(!resolved->endpoints_.empty());
        for (const auto& endpoint : resolved->endpoints_) {
            RUVIA_CHECK(endpoint.port() == peer.local_endpoint().port());
            RUVIA_CHECK(endpoint.address().is_loopback());
        }
        RUVIA_CHECK(pool.allocations_ > pool.returns_);
    }
    RUVIA_CHECK_EQ(pool.allocations_, pool.returns_);
    RUVIA_CHECK_EQ(pool.live_bytes_, 0U);
}

RUVIA_TEST(http3_quic_client_endpoint_resolver_uses_one_absolute_deadline) {
    asio::io_context io;
    std::pmr::unsynchronized_pool_resource pool;
    ruvia::detail::http3_quic_client_endpoint_resolver resolver(io, &pool);
    const auto expired = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    std::optional<resolver_type::result_type> result;
    auto future = asio::co_spawn(io,
        ruvia::as_awaitable(observe(resolver.resolve("localhost", 443, expired), result)),
        asio::use_future);
    io.run();
    future.get();
    RUVIA_CHECK(result && result->status_ ==
                              ruvia::detail::http3_quic_client_endpoint_resolver::status_type::timeout);
    RUVIA_CHECK_EQ(io.poll(), 0U);
}

RUVIA_TEST(http3_quic_client_endpoint_resolver_interrupt_drains_callbacks_and_can_resolve_again) {
    asio::io_context io;
    counting_resource pool;
    {
        resolver_type resolver(io, &pool);
        std::optional<resolver_type::result_type> interrupted;
        auto future = asio::co_spawn(io,
            ruvia::as_awaitable(observe(resolver.resolve("localhost", 443,
                                            std::chrono::steady_clock::now() + std::chrono::seconds(2)),
                interrupted)),
            asio::use_future);
        RUVIA_CHECK(io.poll_one() != 0);
        resolver.interrupt();
        io.restart();
        io.run();
        future.get();
        RUVIA_CHECK(interrupted && interrupted->status_ == resolver_type::status_type::interrupted);
        RUVIA_CHECK_EQ(io.poll(), 0U);
        std::optional<resolver_type::result_type> repeated;
        auto next_value = asio::co_spawn(io,
            ruvia::as_awaitable(observe(resolver.resolve("127.0.0.1", 443,
                                            std::chrono::steady_clock::now() + std::chrono::seconds(2)),
                repeated)),
            asio::use_future);
        io.restart();
        io.run();
        next_value.get();
        RUVIA_CHECK(repeated && repeated->status_ == resolver_type::status_type::resolved);
        RUVIA_CHECK(repeated && !repeated->endpoints_.empty());
        RUVIA_CHECK_EQ(io.poll(), 0U);
    }
    RUVIA_CHECK_EQ(pool.allocations_, pool.returns_);
    RUVIA_CHECK_EQ(pool.live_bytes_, 0U);
}

RUVIA_TEST(http3_quic_client_endpoint_resolver_stop_drains_resolver_and_timer) {
    asio::io_context io;
    counting_resource pool;
    {
        ruvia::detail::http3_quic_client_endpoint_resolver resolver(io, &pool);
        std::optional<resolver_type::result_type> cancelled;
        auto future = asio::co_spawn(io,
            ruvia::as_awaitable(observe(resolver.resolve("localhost", 443,
                                            std::chrono::steady_clock::now() + std::chrono::seconds(1)),
                cancelled)),
            asio::use_future);
        RUVIA_CHECK(io.poll_one() != 0);
        resolver.request_stop();
        io.restart();
        io.run();
        future.get();
        RUVIA_CHECK(cancelled && cancelled->status_ ==
                                     ruvia::detail::http3_quic_client_endpoint_resolver::status_type::stopped);
        RUVIA_CHECK_EQ(io.poll(), 0U);
        std::optional<resolver_type::result_type> stopped;
        auto after_stop = asio::co_spawn(io,
            ruvia::as_awaitable(observe(resolver.resolve("localhost", 443,
                                            std::chrono::steady_clock::now() + std::chrono::seconds(1)),
                stopped)),
            asio::use_future);
        io.restart();
        io.run();
        after_stop.get();
        RUVIA_CHECK(stopped && stopped->status_ ==
                                   ruvia::detail::http3_quic_client_endpoint_resolver::status_type::stopped);
    }
    RUVIA_CHECK_EQ(pool.allocations_, pool.returns_);
    RUVIA_CHECK_EQ(pool.live_bytes_, 0U);
}

RUVIA_TEST(http3_quic_client_endpoint_resolver_returns_each_operation_memory_and_preserves_prior_result) {
    asio::io_context io;
    counting_resource pool;
    {
        resolver_type resolver(io, &pool);
        std::optional<resolver_type::result_type> retained;
        auto first = asio::co_spawn(io,
            ruvia::as_awaitable(observe(resolver.resolve("localhost", 443,
                                            std::chrono::steady_clock::now() + std::chrono::seconds(3)),
                retained)),
            asio::use_future);
        io.run();
        first.get();
        RUVIA_CHECK(retained && retained->status_ == resolver_type::status_type::resolved);
        if (!retained || retained->endpoints_.empty()) {
            return;
        }
        const auto prior_endpoint = retained->endpoints_.front();
        const auto* prior_address = retained->endpoints_.data();
        const auto baseline = pool.live_bytes_;
        for (int i = 0; i < 12; ++i) {
            std::optional<resolver_type::result_type> current;
            auto future = asio::co_spawn(io,
                ruvia::as_awaitable(observe(resolver.resolve("localhost", 443,
                                                std::chrono::steady_clock::now() + std::chrono::seconds(3)),
                    current)),
                asio::use_future);
            io.restart();
            io.run();
            future.get();
            RUVIA_CHECK(current && current->status_ == resolver_type::status_type::resolved);
            RUVIA_CHECK(pool.live_bytes_ >= baseline);
            current.reset();
            RUVIA_CHECK_EQ(pool.live_bytes_, baseline);
            RUVIA_CHECK(retained->endpoints_.data() == prior_address &&
                        retained->endpoints_.front() == prior_endpoint);
        }
    }
    RUVIA_CHECK_EQ(pool.allocations_, pool.returns_);
    RUVIA_CHECK_EQ(pool.live_bytes_, 0U);
}

RUVIA_TEST(http3_quic_client_endpoint_resolver_allocation_failure_releases_and_allows_next_resolve) {
    asio::io_context io;
    counting_resource pool;
    {
        resolver_type resolver(io, &pool);
        std::optional<resolver_type::result_type> failed;
        auto future = asio::co_spawn(io,
            ruvia::as_awaitable(observe(resolver.resolve("localhost", 443,
                                            std::chrono::steady_clock::now() + std::chrono::seconds(3)),
                failed)),
            asio::use_future);
        pool.reject_ = true;
        io.run();
        pool.reject_ = false;
        bool out_of_memory{};
        try {
            future.get();
        } catch (const std::bad_alloc&) {
            out_of_memory = true;
        }
        RUVIA_CHECK(out_of_memory);
        RUVIA_CHECK(!failed.has_value());
        RUVIA_CHECK_EQ(pool.live_bytes_, 0U);
        std::optional<resolver_type::result_type> recovered;
        auto retry = asio::co_spawn(io,
            ruvia::as_awaitable(observe(resolver.resolve("localhost", 443,
                                            std::chrono::steady_clock::now() + std::chrono::seconds(3)),
                recovered)),
            asio::use_future);
        io.restart();
        io.run();
        retry.get();
        RUVIA_CHECK(recovered && recovered->status_ == resolver_type::status_type::resolved);
    }
    RUVIA_CHECK_EQ(pool.allocations_, pool.returns_);
    RUVIA_CHECK_EQ(pool.live_bytes_, 0U);
}

RUVIA_TEST(http3_quic_client_endpoint_resolver_cold_drop_returns_owned_host) {
    asio::io_context io;
    counting_resource pool;
    {
        ruvia::detail::http3_quic_client_endpoint_resolver resolver(io, &pool);
        {
            auto cold = resolver.resolve("rather-long-valid-domain-for-cold-drop.invalid", 443,
                std::chrono::steady_clock::now() + std::chrono::seconds(1));
            RUVIA_CHECK(pool.allocations_ > pool.returns_);
        }
        RUVIA_CHECK_EQ(pool.allocations_, pool.returns_);
        RUVIA_CHECK_EQ(pool.live_bytes_, 0U);
    }
    RUVIA_CHECK_EQ(pool.allocations_, pool.returns_);
}

RUVIA_TEST(http3_quic_client_endpoint_resolver_rejects_invalid_input_before_starting) {
    asio::io_context io;
    std::pmr::unsynchronized_pool_resource pool;
    ruvia::detail::http3_quic_client_endpoint_resolver resolver(io, &pool);
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)resolver.resolve("", 443, {}); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)resolver.resolve("localhost", 0, {}); }));
}
